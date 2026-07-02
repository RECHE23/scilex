// SciLex — Python extension module (CPython Limited API, abi3).
//
// Exposes the generic maximal-munch lexer:
//   _scilex.compile(rules) -> capsule        # [(kind, pattern, skip[, in_mode[, action]]), …]
//   _scilex.tokenize(handle, text, eof) -> list  # [(kind, lexeme, offset, line, column, mode), …]
//   _scilex.scan_start(handle, text, eof) -> capsule  # a lazy scan cursor
//   _scilex.scan_next(cursor) -> tuple | None    # the next 6-token tuple, or None at the end
//   _scilex.layout(tokens, insignificant=()) -> list  # mode-aware NEWLINE/INDENT/DEDENT
//   _scilex.error                             # raised on a bad pattern, unlexable input, or bad indent
//
// A compiled lexer (held in a PyCapsule) owns its REAL regexes, so the rules are
// compiled once and reused. Tokenization runs on UTF-8 bytes — SciLex's model —
// so offset/column are byte indices (0- and 1-based) and lexemes are decoded
// back to str. On an unlexable position the raised error carries .offset/.line/
// .column (the byte position); the Python wrapper surfaces these as .position.
// When `eof` is true a terminal end_of_input token is appended/yielded.

#define PY_SSIZE_T_CLEAN
#define Py_LIMITED_API 0x030A0000
#include <Python.h>

#include <scilex/scilex.hpp>
#include <scilex/layout.hpp> // opt-in: not pulled in by scilex.hpp
#include <sciforge/binding/convert.hpp>
#include <sciforge/binding/dispatch.hpp>
#include <sciforge/binding/error.hpp>
#include <sciforge/binding/gil.hpp>
#include <sciforge/binding/module.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// The module error type (scilex.error) is created by SCIFORGE_MODULE at the bottom and
// reached through this fixed-name getter; the helpers below build positioned errors on it.
SCIFORGE_BINDING_ERROR_GETTER;

namespace {

namespace sb = sciforge::binding;

// Bridge the in-flight C++ exception to a Python error via the shared substrate:
// bad_alloc -> MemoryError, other std::exception -> scilex.error, else internal error.
PyObject* set_cpp_error() { return sb::set_cpp_error(sciforge_module_error()); }

constexpr const char* CAPSULE_NAME      = "scilex.lexer";
constexpr const char* SCAN_CAPSULE_NAME = "scilex.scan";

// Raises scilex.error carrying the byte position of the offending input, as the
// instance attributes .offset/.line/.column (the wrapper turns them into .position).
void set_positioned_error(const char* message, scilex::position where)
{
    PyObject* exc = PyObject_CallFunction(sciforge_module_error(), "s", message);
    if (exc == nullptr) {
        PyErr_SetString(sciforge_module_error(), message); // fallback: message only, no position
        return;
    }
    PyObject* offset = PyLong_FromSsize_t(static_cast<Py_ssize_t>(where.offset));
    PyObject* line   = PyLong_FromSsize_t(static_cast<Py_ssize_t>(where.line));
    PyObject* column = PyLong_FromSsize_t(static_cast<Py_ssize_t>(where.column));
    if (offset != nullptr && line != nullptr && column != nullptr) {
        PyObject_SetAttrString(exc, "offset", offset);
        PyObject_SetAttrString(exc, "line", line);
        PyObject_SetAttrString(exc, "column", column);
    }
    Py_XDECREF(offset);
    Py_XDECREF(line);
    Py_XDECREF(column);
    PyErr_SetObject(sciforge_module_error(), exc);
    Py_DECREF(exc);
}

// RAII GIL release (restores on every exit, including a throw): the shared
// sciforge::binding::gil_release, kept under the local name the call site uses.
using GilRelease = sciforge::binding::gil_release;

// Releasing the GIL costs a thread-state save/restore (plus re-acquire contention)
// that only pays off once the pure-C++ scan outlasts it. tokenize then builds
// O(tokens) Python objects under the GIL — the same serial-tail shape as REAL's
// findall — so it reuses REAL's measured 4 KB collect threshold (see BENCHMARKS.md):
// below it the toggle would dwarf a sub-millisecond call and regress multithreading.
constexpr Py_ssize_t gil_release_collect_min_bytes = 4096;

// Borrows the source bytes of a str or bytes object WITHOUT copying: for bytes, its
// buffer; for str, its cached UTF-8 (SciLex lexes UTF-8, so offsets stay byte
// indices). The buffer stays valid while `obj` is alive — both types are immutable,
// so it is stable even with the GIL released. `is_bytes` records the input type so
// the output lexemes can match it. Returns 0 on success, -1 (TypeError set) otherwise.
int read_source(PyObject* obj, const char** data, Py_ssize_t* len, bool* is_bytes)
{
    if (PyBytes_Check(obj)) {
        char*      buffer = nullptr;
        Py_ssize_t size   = 0;
        if (PyBytes_AsStringAndSize(obj, &buffer, &size) < 0) {
            return -1;
        }
        *data     = buffer;
        *len      = size;
        *is_bytes = true;
        return 0;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t  size   = 0;
        const char* buffer = PyUnicode_AsUTF8AndSize(obj, &size);
        if (buffer == nullptr) {
            return -1;
        }
        *data     = buffer;
        *len      = size;
        *is_bytes = false;
        return 0;
    }
    PyErr_SetString(PyExc_TypeError, "expected str or bytes");
    return -1;
}

// Builds the (kind, lexeme, offset, line, column, mode) tuple for one token. The
// lexeme is a str (decoded from the UTF-8 bytes) when the source was str, or bytes
// when it was bytes — matching the input type. `mode` is the name of the mode the
// token was lexed in. Returns a new reference, or null on error.
PyObject* token_tuple(const scilex::token& token, bool is_bytes, const std::string& mode)
{
    // A synthetic token (newline/indent/dedent) carries a default, null-data view;
    // use "" so an empty lexeme becomes an empty str/bytes rather than tripping on null.
    const char*      data   = token.lexeme.data() != nullptr ? token.lexeme.data() : "";
    const Py_ssize_t size   = static_cast<Py_ssize_t>(token.lexeme.size());
    PyObject*        lexeme = is_bytes ? PyBytes_FromStringAndSize(data, size)
                                       : PyUnicode_FromStringAndSize(data, size);
    if (lexeme == nullptr) {
        return nullptr;
    }
    return Py_BuildValue("(iNnnns)", token.kind, lexeme, // N steals the lexeme ref
                         static_cast<Py_ssize_t>(token.start.offset),
                         static_cast<Py_ssize_t>(token.start.line),
                         static_cast<Py_ssize_t>(token.start.column),
                         mode.c_str());
}

// Capsule destructor: frees the lexer when the Python handle is collected.
void capsule_free(PyObject* capsule)
{
    delete static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
}

// A lazy scan cursor: owns a stable copy of the source (the token views and the
// iterator point into it) and a reference to the lexer capsule (keeping the rules
// alive). The advance is deferred to the *next* scan_next so the current token is
// always returned before its successor's advance can raise — a lex error then
// surfaces only after every valid token preceding it.
struct scan_state
{
    PyObject*              lexer_capsule {nullptr}; // owned ref, keeps the lexer alive
    const scilex::lexer*   lexer         {nullptr}; // borrowed from the capsule (for mode_name)
    PyObject*              source_obj    {nullptr}; // owned ref to the str/bytes; its buffer is borrowed
    bool                   is_bytes      {false};   // output lexeme type follows the input
    scilex::token_iterator it;                      // current position (views into source_obj's buffer)
    scilex::token_iterator end;                     // the end sentinel
    bool                   needs_advance {false};   // advance before the next read?
};

// Frees a scan_state: destroy the cursor (its iterators borrow source_obj's buffer)
// FIRST, then release the borrowed refs — the lifetime order REAL's binding uses too.
void free_scan_state(scan_state* state)
{
    PyObject* source_obj    = state->source_obj;
    PyObject* lexer_capsule = state->lexer_capsule;
    delete state;
    Py_XDECREF(source_obj);
    Py_XDECREF(lexer_capsule);
}

// Capsule destructor: releases the lexer reference and frees the cursor.
void scan_capsule_free(PyObject* capsule)
{
    auto* state = static_cast<scan_state*>(PyCapsule_GetPointer(capsule, SCAN_CAPSULE_NAME));
    if (state != nullptr) {
        free_scan_state(state);
    }
}

// Parses a rule's optional `in_mode` field into mode names. None/absent -> empty
// (the rule lives in the default mode only); otherwise a sequence of str. A bare
// str is rejected (it would silently char-split). Returns 0 on success, -1 (error
// set) otherwise. `obj` must stay alive for the call (its entries are borrowed).
int parse_in_mode(PyObject* obj, std::vector<std::string>* out, const char* param_name)
{
    if (obj == nullptr || obj == Py_None) {
        return 0;
    }
    if (PyUnicode_Check(obj)) {
        PyErr_Format(sciforge_module_error(), "%s must be a sequence of mode names, not a bare str", param_name);
        return -1;
    }
    const Py_ssize_t count = PySequence_Size(obj);
    if (count < 0) {
        PyErr_Clear();
        PyErr_Format(sciforge_module_error(), "%s must be a sequence of str", param_name);
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* name = PySequence_GetItem(obj, i); // new ref
        if (name == nullptr) {
            return -1;
        }
        Py_ssize_t  length = 0;
        const char* text   = PyUnicode_AsUTF8AndSize(name, &length);
        if (text == nullptr) {
            Py_DECREF(name);
            PyErr_Clear();
            PyErr_Format(sciforge_module_error(), "%s entries must be str", param_name);
            return -1;
        }
        out->emplace_back(text, static_cast<std::size_t>(length));
        Py_DECREF(name);
    }
    return 0;
}

// Parses a rule's optional `action` field. None/absent -> no action; otherwise a
// tuple ("push", mode) / ("set", mode) / ("pop",). Returns 0 on success, -1 (error
// set) otherwise. `obj` must stay alive for the call (its strings are borrowed).
int parse_action(PyObject* obj, std::optional<scilex::mode_action>* out)
{
    if (obj == nullptr || obj == Py_None) {
        return 0;
    }
    const char* op_text = nullptr;
    const char* target  = nullptr;
    if (PyTuple_Check(obj) == 0 || PyArg_ParseTuple(obj, "s|s", &op_text, &target) == 0) {
        PyErr_Clear();
        PyErr_SetString(sciforge_module_error(),
                        R"(action must be None or a tuple ("push", mode) / ("set", mode) / ("pop",))");
        return -1;
    }
    scilex::mode_action action;
    const std::string   operation {op_text};
    if (operation == "push" || operation == "set") {
        if (target == nullptr) {
            PyErr_Format(sciforge_module_error(), "'%s' action needs a target mode", op_text);
            return -1;
        }
        action.operation = operation == "push" ? scilex::mode_action::op::push
                                               : scilex::mode_action::op::set;
        action.target = target;
    }
    else if (operation == "pop") {
        if (target != nullptr) {
            PyErr_SetString(sciforge_module_error(), "'pop' action takes no extra argument");
            return -1;
        }
        action.operation = scilex::mode_action::op::pop;
    }
    else {
        PyErr_Format(sciforge_module_error(), "unknown action '%s' (expected push, pop or set)", op_text);
        return -1;
    }
    *out = action;
    return 0;
}

// _scilex.compile(rules, dfa_modes=()) -> capsule wrapping a scilex::lexer. Each rule
// is (kind:int, pattern:str, skip:bool) and may carry two optional fields: in_mode (a
// sequence of mode-name str) and action (a push/pop/set tuple). A bare 3-tuple stays
// valid (a default-mode rule with no transition). dfa_modes is a sequence of mode names
// to accelerate with a real::dfa fast path (best-effort: an un-DFA-able mode silently
// stays on Pike — see dfa_modes_active). insignificant_modes is NOT passed here: layout
// significance is applied Python-side in _scilex.layout, unchanged.
PyObject* scilex_compile(PyObject* /*self*/, PyObject* args)
{
    PyObject*   rules_obj     = nullptr;
    PyObject*   dfa_modes_obj  = nullptr; // optional sequence of mode names (borrowed)
    const char* errors_str     = nullptr; // optional "raise" (default) or "token"
    if (PyArg_ParseTuple(args, "O|Oz", &rules_obj, &dfa_modes_obj, &errors_str) == 0) {
        return nullptr;
    }
    std::vector<std::string> dfa_modes;
    if (parse_in_mode(dfa_modes_obj, &dfa_modes, "dfa_modes") < 0) {
        return nullptr; // not a sequence of names (error set)
    }
    scilex::error_policy errors = scilex::error_policy::raise;
    if (errors_str != nullptr) {
        const std::string name {errors_str};
        if (name == "token") {
            errors = scilex::error_policy::token;
        }
        else if (name != "raise") {
            PyErr_Format(PyExc_ValueError, "errors must be 'raise' or 'token', not '%s'", errors_str);
            return nullptr;
        }
    }
    const Py_ssize_t count = PySequence_Size(rules_obj);
    if (count < 0) {
        return nullptr; // not a sequence (error already set)
    }
    try {
        std::vector<scilex::rule> rules;
        rules.reserve(static_cast<std::size_t>(count));
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject* item = PySequence_GetItem(rules_obj, i); // new ref
            if (item == nullptr) {
                return nullptr;
            }
            int         kind        = 0;
            const char* pattern     = nullptr;
            int         skip        = 0;
            PyObject*   in_mode_obj = nullptr; // optional 4th field (borrowed)
            PyObject*   action_obj  = nullptr; // optional 5th field (borrowed)
            const int   parsed      = PyArg_ParseTuple(item, "isp|OO", &kind, &pattern, &skip,
                                                       &in_mode_obj, &action_obj);
            // pattern, in_mode_obj and action_obj all borrow from `item` — read them
            // (copying through) before releasing it.
            std::string                        pattern_copy {parsed != 0 && pattern != nullptr ? pattern : ""};
            std::vector<std::string>           in_mode;
            std::optional<scilex::mode_action> action;
            const bool                         extras_ok = parsed != 0
                                                           && parse_in_mode(in_mode_obj, &in_mode, "in_mode") == 0
                                                           && parse_action(action_obj, &action) == 0;
            Py_DECREF(item);
            if (parsed == 0 || !extras_ok) {
                return nullptr; // PyArg / parse_in_mode / parse_action set the error
            }
            try {
                scilex::rule built {kind, real::regex(pattern_copy), skip != 0};
                built.in_mode = std::move(in_mode);
                built.action  = action;
                rules.push_back(std::move(built));
            }
            catch (const real::regex_error& error) {
                // what() already reads "regex_error at <offset>: <cause>"; prefix the rule.
                PyErr_Format(sciforge_module_error(), "rule %zd: %s", i, error.what());
                return nullptr;
            }
        }
        auto* lexer = new scilex::lexer(std::move(rules), {},
                                        std::unordered_set<std::string>(dfa_modes.begin(), dfa_modes.end()),
                                        errors);
        PyObject* capsule = PyCapsule_New(lexer, CAPSULE_NAME, capsule_free);
        if (capsule == nullptr) {
            delete lexer;
            return nullptr;
        }
        return capsule;
    }
    catch (...) {
        return set_cpp_error();
    }
}

// _scilex.dfa_modes_active(handle) -> the mode names actually accelerated by a DFA fast
// path. A mode requested in compile()'s dfa_modes but rejected (an un-DFA-able assertion,
// or a failed build-time audit) is absent — it fell back to Pike. Dispatched via def<>:
// caster<scilex::lexer*> extracts the capsule, caster<std::vector<std::string>> builds the
// list (the Python wrapper wraps either a tuple or a list in list(), so it is unchanged).
std::vector<std::string> dfa_modes_active(scilex::lexer* lexer)
{
    return lexer->dfa_modes_active();
}

// _scilex.tokenize(handle, text, eof=False) -> list of (kind, lexeme, offset, line,
// column). With eof true a terminal end_of_input token is appended.
PyObject* scilex_tokenize(PyObject* /*self*/, PyObject* args)
{
    PyObject* capsule  = nullptr;
    PyObject* text_obj = nullptr;
    int       eof      = 0;
    if (PyArg_ParseTuple(args, "OO|p", &capsule, &text_obj, &eof) == 0) {
        return nullptr;
    }
    auto* lexer = static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
    if (lexer == nullptr) {
        return nullptr; // wrong capsule (error already set)
    }
    const char* data     = nullptr;
    Py_ssize_t  size     = 0;
    bool        is_bytes = false;
    if (read_source(text_obj, &data, &size, &is_bytes) < 0) {
        return nullptr; // not str/bytes (TypeError set)
    }
    // text_obj is a borrowed argument — alive for the whole call and immutable, so
    // its buffer is stable even while the GIL is released below.
    const std::string_view   text {data, static_cast<std::size_t>(size)};
    const scilex::eof_policy  policy {eof != 0 ? scilex::eof_policy::append : scilex::eof_policy::omit};
    PyObject*                result = nullptr;
    try {
        // Phase 1 — scan (GIL released past the threshold; the tokens' lexeme views
        // point into `text`, i.e. text_obj's stable buffer). Phase 2 — build the
        // Python list under the GIL.
        std::vector<scilex::token> tokens;
        if (size >= gil_release_collect_min_bytes) {
            const GilRelease unlocked;
            tokens = lexer->tokenize(text, policy);
        }
        else {
            tokens = lexer->tokenize(text, policy);
        }
        result = PyList_New(static_cast<Py_ssize_t>(tokens.size()));
        if (result != nullptr) {
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                PyObject* tuple {token_tuple(tokens[i], is_bytes, lexer->mode_name(tokens[i].mode_id))};
                if (tuple == nullptr) {
                    Py_CLEAR(result);
                    break;
                }
                PyList_SetItem(result, static_cast<Py_ssize_t>(i), tuple); // steals ref
            }
        }
    }
    catch (const scilex::lex_error& error) {
        set_positioned_error(error.what(), error.where());
        result = nullptr;
    }
    catch (...) {
        result = set_cpp_error();
    }
    return result;
}

// _scilex.scan_start(handle, text, eof=False) -> a lazy scan-cursor capsule.
// The first token is produced eagerly here (so an error at the first position is
// raised now); subsequent tokens come one at a time from scan_next.
PyObject* scilex_scan_start(PyObject* /*self*/, PyObject* args)
{
    PyObject* capsule  = nullptr;
    PyObject* text_obj = nullptr;
    int       eof      = 0;
    if (PyArg_ParseTuple(args, "OO|p", &capsule, &text_obj, &eof) == 0) {
        return nullptr;
    }
    auto* lexer = static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
    if (lexer == nullptr) {
        return nullptr; // wrong capsule (error already set)
    }
    const char* data     = nullptr;
    Py_ssize_t  size     = 0;
    bool        is_bytes = false;
    if (read_source(text_obj, &data, &size, &is_bytes) < 0) {
        return nullptr; // not str/bytes (TypeError set)
    }
    auto* state = new scan_state;
    // Keep refs to the lexer (its rules) and the source (its borrowed buffer backs
    // the iterator and every token's lexeme view); both must outlive the cursor.
    state->lexer_capsule = Py_NewRef(capsule);
    state->lexer         = lexer; // borrowed; the capsule ref above keeps it alive
    state->source_obj    = Py_NewRef(text_obj);
    state->is_bytes      = is_bytes;
    try {
        const scilex::eof_policy policy {eof != 0 ? scilex::eof_policy::append : scilex::eof_policy::omit};
        // Construct the iterator over the borrowed source — runs the first advance (may throw).
        state->it = scilex::token_iterator(*lexer, std::string_view(data, static_cast<std::size_t>(size)), policy);
    }
    catch (const scilex::lex_error& error) {
        free_scan_state(state);
        set_positioned_error(error.what(), error.where());
        return nullptr;
    }
    catch (...) {
        free_scan_state(state);
        return set_cpp_error();
    }
    PyObject* cursor = PyCapsule_New(state, SCAN_CAPSULE_NAME, scan_capsule_free);
    if (cursor == nullptr) {
        free_scan_state(state);
        return nullptr;
    }
    return cursor;
}

// _scilex.scan_next(cursor) -> the next (kind, lexeme, offset, line, column) tuple,
// or None once the scan is exhausted. Raises scilex.error (with position) if a
// position matches no rule — but only after every token before it has been yielded.
PyObject* scilex_scan_next(PyObject* /*self*/, PyObject* args)
{
    PyObject* capsule = nullptr;
    if (PyArg_ParseTuple(args, "O", &capsule) == 0) {
        return nullptr;
    }
    auto* state = static_cast<scan_state*>(PyCapsule_GetPointer(capsule, SCAN_CAPSULE_NAME));
    if (state == nullptr) {
        return nullptr; // wrong capsule (error already set)
    }
    if (state->it == state->end) {
        Py_RETURN_NONE;
    }
    if (state->needs_advance) {
        try {
            ++(state->it); // advance past the token returned last time (may throw)
        }
        catch (const scilex::lex_error& error) {
            state->it = state->end; // stop: the cursor is now exhausted
            set_positioned_error(error.what(), error.where());
            return nullptr;
        }
        catch (...) {
            state->it = state->end;
            return set_cpp_error();
        }
        if (state->it == state->end) {
            Py_RETURN_NONE;
        }
    }
    state->needs_advance = true;
    return token_tuple(*state->it, state->is_bytes, state->lexer->mode_name(state->it->mode_id));
}

// _scilex.layout(tokens) -> list. Rewrites an end_of_input-terminated sequence of
// (kind, lexeme, offset, line, column) tuples with synthetic newline/indent/dedent
// tokens inserted from each line's indentation; returns the same tuple shape.
PyObject* scilex_layout(PyObject* /*self*/, PyObject* args)
{
    PyObject* seq               = nullptr;
    PyObject* insignificant_obj = nullptr; // optional: an iterable of mode names
    if (PyArg_ParseTuple(args, "O|O", &seq, &insignificant_obj) == 0) {
        return nullptr;
    }
    const Py_ssize_t count = PySequence_Size(seq);
    if (count < 0) {
        return nullptr; // not a sequence (error already set)
    }
    std::vector<std::string> insignificant;
    if (parse_in_mode(insignificant_obj, &insignificant, "insignificant_modes") < 0) {
        return nullptr; // not a sequence of mode names (error set)
    }
    PyObject* result = nullptr;
    try {
        // Re-derive a self-consistent mode-id space from the tuples' mode names:
        // "default" is id 0 (so the synthetic NEWLINE/INDENT/DEDENT, which carry mode
        // id 0, stay significant), then each distinct name in first-seen order. The
        // significance policy is built from that name set and the insignificant set.
        std::map<std::string, std::size_t> mode_id;
        std::vector<std::string>           names;
        const auto intern = [&mode_id, &names](const std::string& name) -> std::size_t {
            const auto [position, inserted] {mode_id.emplace(name, names.size())};
            if (inserted) {
                names.push_back(name);
            }
            return position->second;
        };
        intern("default"); // id 0

        // Own every lexeme's bytes (reserved so addresses stay stable as the token
        // views below point into them); rebuild the scilex::token sequence.
        std::vector<std::string>   lexemes;
        std::vector<scilex::token> input;
        lexemes.reserve(static_cast<std::size_t>(count));
        input.reserve(static_cast<std::size_t>(count));
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject* item = PySequence_GetItem(seq, i); // new ref
            if (item == nullptr) {
                return nullptr;
            }
            int         kind   = 0;
            const char* lexeme = nullptr;
            Py_ssize_t  length = 0;
            Py_ssize_t  offset = 0;
            Py_ssize_t  line   = 0;
            Py_ssize_t  column = 0;
            const char* mode   = nullptr; // optional 6th field (the mode name)
            const int   parsed = PyArg_ParseTuple(item, "is#nnn|s", &kind, &lexeme, &length,
                                                   &offset, &line, &column, &mode);
            const std::string mode_name {parsed != 0 && mode != nullptr ? mode : "default"};
            if (parsed != 0) {
                lexemes.emplace_back(lexeme != nullptr ? lexeme : "", static_cast<std::size_t>(length));
            }
            Py_DECREF(item);
            if (parsed == 0) {
                return nullptr; // not a (kind, lexeme, offset, line, column[, mode]) tuple
            }
            const scilex::position where {static_cast<std::size_t>(offset),
                                          static_cast<std::size_t>(line),
                                          static_cast<std::size_t>(column)};
            input.push_back(scilex::token {kind, std::string_view(lexemes.back()), where, intern(mode_name)});
        }
        std::vector<bool> mode_significant(names.size(), true);
        for (std::size_t i = 0; i < names.size(); ++i) {
            mode_significant[i] = std::find(insignificant.begin(), insignificant.end(), names[i])
                                  == insignificant.end();
        }
        const std::vector<scilex::token> out {scilex::layout(input, mode_significant)};
        result = PyList_New(static_cast<Py_ssize_t>(out.size()));
        if (result != nullptr) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                // Layout is a str/indentation pass (its input lexemes parse via "s#"),
                // so its synthetic and re-emitted tokens are str lexemes.
                PyObject* tuple {token_tuple(out[i], /*is_bytes=*/false, names[out[i].mode_id])};
                if (tuple == nullptr) {
                    Py_CLEAR(result);
                    break;
                }
                PyList_SetItem(result, static_cast<Py_ssize_t>(i), tuple); // steals ref
            }
        }
    }
    catch (const scilex::layout_error& error) {
        set_positioned_error(error.what(), error.where());
        result = nullptr;
    }
    catch (...) {
        result = set_cpp_error();
    }
    return result;
}

// The REAL version this extension was COMPILED against (baked from real/version.hpp at build time).
// Consumers assert it is at least the pinned version so a stale build/install fails loudly rather than
// silently running old semantics (e.g. ASCII shorthands after REAL made them Unicode).
PyObject* scilex_real_version(PyObject* /*self*/, PyObject* /*unused*/)
{
    return PyUnicode_FromString(REAL_VERSION_STRING);
}

} // namespace

// The per-binding capsule caster (the documented pattern): extracts the owned lexer from
// its handle for def<>-dispatched functions. A wrong/null capsule becomes a cast_error,
// which the dispatch turns into a TypeError.
namespace sciforge::binding {
template <>
struct caster<scilex::lexer*> {
    static scilex::lexer* from_python(PyObject* obj)
    {
        auto* lexer = static_cast<scilex::lexer*>(PyCapsule_GetPointer(obj, CAPSULE_NAME));
        if (lexer == nullptr) {
            PyErr_Clear();
            throw cast_error("expected a scilex.lexer handle");
        }
        return lexer;
    }
};
} // namespace sciforge::binding

// The declarative module surface. compile and scan_start are owned-capsule factories and
// tokenize/scan_next/layout raise positioned errors (carrying .offset/.line/.column) that
// the generic dispatch cannot reproduce, so all five stay manual PyCFunctions registered
// through m.raw; only dfa_modes_active (a plain value return, no positioned error) is
// def<>-dispatched.
SCIFORGE_MODULE(_scilex, "scilex.error", m)
{
    m.raw("compile", scilex_compile, METH_VARARGS,
          "compile(rules, dfa_modes=(), errors='raise')\n"
          "Compile an ordered list of rules into a lexer handle.\n\n"
          "Args:\n"
          "    rules (sequence): Each rule is (kind:int, pattern:str, skip:bool) and may\n"
          "        carry two optional fields: in_mode (a sequence of mode-name str; empty\n"
          "        means the default mode) and action (None, or a transition tuple\n"
          "        (\"push\", mode) / (\"set\", mode) / (\"pop\",)).\n"
          "    dfa_modes (sequence): Mode names to accelerate with a DFA fast path. Best-\n"
          "        effort: an un-DFA-able mode silently stays on Pike (see dfa_modes_active).\n"
          "    errors (str): 'raise' (default, unchanged: raise at the first unlexable byte)\n"
          "        or 'token' (recover, emitting one ERROR-kind token per unlexable run).\n\n"
          "Returns:\n"
          "    capsule: An opaque compiled-lexer handle for tokenize()/scan_start().\n\n"
          "Raises:\n"
          "    error: If a pattern is an invalid regex, a transition targets an empty mode,\n"
          "        or dfa_modes names an unknown mode.\n"
          "    ValueError: If errors is not 'raise' or 'token'.");
    m.def<&dfa_modes_active>("dfa_modes_active",
                             "dfa_modes_active(handle) -> list[str]\n"
                             "The mode names actually accelerated by a DFA (a requested mode that fell back\n"
                             "to Pike — an un-DFA-able assertion or a failed audit — is absent).");
    m.raw("real_version", scilex_real_version, METH_NOARGS,
          "real_version() -> str\n"
          "The REAL version this compiled extension was built against (real/version.hpp), so a stale\n"
          "build or install can be detected by comparing it to the pinned real-regex version.");
    m.raw("tokenize", scilex_tokenize, METH_VARARGS,
          "tokenize(handle, text, eof=False)\n"
          "Eagerly tokenize text with a compiled-lexer handle.\n\n"
          "Args:\n"
          "    handle (capsule): A handle from compile().\n"
          "    text (str): The source to tokenize.\n"
          "    eof (bool): Append a terminal end_of_input token.\n\n"
          "Returns:\n"
          "    list: (kind, lexeme, offset, line, column, mode) tuples, skip matches omitted.\n\n"
          "Raises:\n"
          "    error: If some position is matched by no rule (carrying .offset/.line/.column).");
    m.raw("scan_start", scilex_scan_start, METH_VARARGS,
          "scan_start(handle, text, eof=False)\n"
          "Begin a lazy scan; returns a cursor capsule for scan_next().\n\n"
          "Args:\n"
          "    handle (capsule): A handle from compile().\n"
          "    text (str): The source to scan.\n"
          "    eof (bool): Yield a terminal end_of_input token.\n\n"
          "Returns:\n"
          "    capsule: A scan cursor; pass it to scan_next().\n\n"
          "Raises:\n"
          "    error: If the first position is matched by no rule.");
    m.raw("scan_next", scilex_scan_next, METH_VARARGS,
          "scan_next(cursor)\n"
          "Return the next token tuple from a scan cursor, or None at the end.\n\n"
          "Args:\n"
          "    cursor (capsule): A cursor from scan_start().\n\n"
          "Returns:\n"
          "    tuple | None: (kind, lexeme, offset, line, column, mode), or None when exhausted.\n\n"
          "Raises:\n"
          "    error: If some position is matched by no rule (after earlier tokens are yielded).");
    m.raw("layout", scilex_layout, METH_VARARGS,
          "layout(tokens, insignificant=())\n"
          "Insert NEWLINE/INDENT/DEDENT tokens from indentation (mode-aware).\n\n"
          "Args:\n"
          "    tokens (sequence): An end_of_input-terminated sequence of\n"
          "        (kind, lexeme, offset, line, column, mode) tuples.\n"
          "    insignificant (sequence): Mode names whose tokens carry no layout\n"
          "        structure (Layout Awareness Level A); empty is the positional pass.\n\n"
          "Returns:\n"
          "    list: The layout-aware tuples (still end_of_input-terminated).\n\n"
          "Raises:\n"
          "    error: On a dedent to an indentation no open block used (with .offset/.line/.column).");
}
