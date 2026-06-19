// SciLex — Python extension module (CPython Limited API, abi3).
//
// Exposes the generic maximal-munch lexer:
//   _scilex.compile(rules) -> capsule        # rules: [(kind:int, pattern:str, skip:bool), …]
//   _scilex.tokenize(handle, text, eof) -> list  # eager [(kind, lexeme, offset, line, column), …]
//   _scilex.scan_start(handle, text, eof) -> capsule  # a lazy scan cursor
//   _scilex.scan_next(cursor) -> tuple | None    # the next token, or None at the end
//   _scilex.layout(tokens) -> list           # insert NEWLINE/INDENT/DEDENT from indentation
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

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

PyObject* error_type = nullptr; // scilex.error

constexpr const char* CAPSULE_NAME      = "scilex.lexer";
constexpr const char* SCAN_CAPSULE_NAME = "scilex.scan";

// Raises scilex.error carrying the byte position of the offending input, as the
// instance attributes .offset/.line/.column (the wrapper turns them into .position).
void set_positioned_error(const char* message, scilex::position where)
{
    PyObject* exc = PyObject_CallFunction(error_type, "s", message);
    if (exc == nullptr) {
        PyErr_SetString(error_type, message); // fallback: message only, no position
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
    PyErr_SetObject(error_type, exc);
    Py_DECREF(exc);
}

// Builds the (kind, lexeme, offset, line, column) tuple for one token (lexeme
// decoded from UTF-8 bytes back to str). Returns a new reference, or null on error.
PyObject* token_tuple(const scilex::token& token)
{
    // A synthetic token (newline/indent/dedent) carries a default, null-data view;
    // "s#" maps a null pointer to None, so use "" to keep an empty lexeme an empty str.
    const char* data = token.lexeme.data() != nullptr ? token.lexeme.data() : "";
    return Py_BuildValue("(is#nnn)", token.kind, data,
                         static_cast<Py_ssize_t>(token.lexeme.size()),
                         static_cast<Py_ssize_t>(token.start.offset),
                         static_cast<Py_ssize_t>(token.start.line),
                         static_cast<Py_ssize_t>(token.start.column));
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
    std::string            source;                  // owned UTF-8 copy (tokens view into it)
    scilex::token_iterator it;                      // current position (views into source)
    scilex::token_iterator end;                     // the end sentinel
    bool                   needs_advance {false};   // advance before the next read?
};

// Capsule destructor: releases the lexer reference and frees the cursor.
void scan_capsule_free(PyObject* capsule)
{
    auto* state = static_cast<scan_state*>(PyCapsule_GetPointer(capsule, SCAN_CAPSULE_NAME));
    if (state != nullptr) {
        Py_XDECREF(state->lexer_capsule);
        delete state;
    }
}

// _scilex.compile(rules) -> capsule wrapping a scilex::lexer built from
// (kind:int, pattern:str, skip:bool) triples.
PyObject* scilex_compile(PyObject* /*self*/, PyObject* args)
{
    PyObject* rules_obj = nullptr;
    if (PyArg_ParseTuple(args, "O", &rules_obj) == 0) {
        return nullptr;
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
            int         kind    = 0;
            const char* pattern = nullptr;
            int         skip    = 0;
            const int   parsed  = PyArg_ParseTuple(item, "isp", &kind, &pattern, &skip);
            // `pattern` borrows the item's buffer — copy before releasing it.
            std::string pattern_copy {parsed != 0 && pattern != nullptr ? pattern : ""};
            Py_DECREF(item);
            if (parsed == 0) {
                return nullptr; // not an (int, str, bool) triple
            }
            rules.push_back(scilex::rule {kind, real::regex(pattern_copy), skip != 0});
        }
        auto* lexer = new scilex::lexer(std::move(rules));
        PyObject* capsule = PyCapsule_New(lexer, CAPSULE_NAME, capsule_free);
        if (capsule == nullptr) {
            delete lexer;
            return nullptr;
        }
        return capsule;
    }
    catch (const std::exception& error) {
        PyErr_SetString(error_type, error.what());
        return nullptr;
    }
}

// _scilex.tokenize(handle, text, eof=False) -> list of (kind, lexeme, offset, line,
// column). With eof true a terminal end_of_input token is appended.
PyObject* scilex_tokenize(PyObject* /*self*/, PyObject* args)
{
    PyObject* capsule  = nullptr;
    PyObject* text_obj = nullptr;
    int       eof      = 0;
    if (PyArg_ParseTuple(args, "OU|p", &capsule, &text_obj, &eof) == 0) {
        return nullptr;
    }
    auto* lexer = static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
    if (lexer == nullptr) {
        return nullptr; // wrong capsule (error already set)
    }
    PyObject* utf8 = PyUnicode_AsUTF8String(text_obj); // new ref (bytes)
    if (utf8 == nullptr) {
        return nullptr;
    }
    char*      data = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(utf8, &data, &size) < 0) {
        Py_DECREF(utf8);
        return nullptr;
    }
    PyObject* result = nullptr;
    try {
        const std::string      text {data, static_cast<std::size_t>(size)};
        const scilex::eof_policy policy {eof != 0 ? scilex::eof_policy::append : scilex::eof_policy::omit};
        const std::vector<scilex::token> tokens {lexer->tokenize(text, policy)};
        result = PyList_New(static_cast<Py_ssize_t>(tokens.size()));
        if (result != nullptr) {
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                PyObject* tuple {token_tuple(tokens[i])};
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
    catch (const std::exception& error) {
        PyErr_SetString(error_type, error.what());
        result = nullptr;
    }
    Py_DECREF(utf8);
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
    if (PyArg_ParseTuple(args, "OU|p", &capsule, &text_obj, &eof) == 0) {
        return nullptr;
    }
    auto* lexer = static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
    if (lexer == nullptr) {
        return nullptr; // wrong capsule (error already set)
    }
    PyObject* utf8 = PyUnicode_AsUTF8String(text_obj); // new ref (bytes)
    if (utf8 == nullptr) {
        return nullptr;
    }
    char*      data = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(utf8, &data, &size) < 0) {
        Py_DECREF(utf8);
        return nullptr;
    }
    auto* state = new scan_state;
    try {
        state->lexer_capsule = Py_NewRef(capsule);
        state->source.assign(data, static_cast<std::size_t>(size));
        const scilex::eof_policy policy {eof != 0 ? scilex::eof_policy::append : scilex::eof_policy::omit};
        // Construct the iterator over the owned source — runs the first advance (may throw).
        state->it = scilex::token_iterator(*lexer, std::string_view(state->source), policy);
    }
    catch (const scilex::lex_error& error) {
        Py_DECREF(utf8);
        Py_XDECREF(state->lexer_capsule);
        delete state;
        set_positioned_error(error.what(), error.where());
        return nullptr;
    }
    catch (const std::exception& error) {
        Py_DECREF(utf8);
        Py_XDECREF(state->lexer_capsule);
        delete state;
        PyErr_SetString(error_type, error.what());
        return nullptr;
    }
    Py_DECREF(utf8);
    PyObject* cursor = PyCapsule_New(state, SCAN_CAPSULE_NAME, scan_capsule_free);
    if (cursor == nullptr) {
        Py_XDECREF(state->lexer_capsule);
        delete state;
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
        catch (const std::exception& error) {
            state->it = state->end;
            PyErr_SetString(error_type, error.what());
            return nullptr;
        }
        if (state->it == state->end) {
            Py_RETURN_NONE;
        }
    }
    state->needs_advance = true;
    return token_tuple(*state->it);
}

// _scilex.layout(tokens) -> list. Rewrites an end_of_input-terminated sequence of
// (kind, lexeme, offset, line, column) tuples with synthetic newline/indent/dedent
// tokens inserted from each line's indentation; returns the same tuple shape.
PyObject* scilex_layout(PyObject* /*self*/, PyObject* args)
{
    PyObject* seq = nullptr;
    if (PyArg_ParseTuple(args, "O", &seq) == 0) {
        return nullptr;
    }
    const Py_ssize_t count = PySequence_Size(seq);
    if (count < 0) {
        return nullptr; // not a sequence (error already set)
    }
    PyObject* result = nullptr;
    try {
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
            const int   parsed = PyArg_ParseTuple(item, "is#nnn", &kind, &lexeme, &length,
                                                   &offset, &line, &column);
            if (parsed != 0) {
                lexemes.emplace_back(lexeme != nullptr ? lexeme : "", static_cast<std::size_t>(length));
            }
            Py_DECREF(item);
            if (parsed == 0) {
                return nullptr; // not a (kind, lexeme, offset, line, column) tuple
            }
            const scilex::position where {static_cast<std::size_t>(offset),
                                          static_cast<std::size_t>(line),
                                          static_cast<std::size_t>(column)};
            input.push_back(scilex::token {kind, std::string_view(lexemes.back()), where});
        }
        const std::vector<scilex::token> out {scilex::layout(input)};
        result = PyList_New(static_cast<Py_ssize_t>(out.size()));
        if (result != nullptr) {
            for (std::size_t i = 0; i < out.size(); ++i) {
                PyObject* tuple {token_tuple(out[i])};
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
    catch (const std::exception& error) {
        PyErr_SetString(error_type, error.what());
        result = nullptr;
    }
    return result;
}

PyMethodDef module_methods[] = {
    {"compile", scilex_compile, METH_VARARGS,
     "compile(rules)\n"
     "Compile an ordered list of (kind, pattern, skip) rules into a lexer handle.\n\n"
     "Args:\n"
     "    rules (sequence): Triples (kind:int, pattern:str, skip:bool).\n\n"
     "Returns:\n"
     "    capsule: An opaque compiled-lexer handle for tokenize()/scan_start().\n\n"
     "Raises:\n"
     "    error: If a pattern is an invalid regex."},
    {"tokenize", scilex_tokenize, METH_VARARGS,
     "tokenize(handle, text, eof=False)\n"
     "Eagerly tokenize text with a compiled-lexer handle.\n\n"
     "Args:\n"
     "    handle (capsule): A handle from compile().\n"
     "    text (str): The source to tokenize.\n"
     "    eof (bool): Append a terminal end_of_input token.\n\n"
     "Returns:\n"
     "    list: (kind, lexeme, offset, line, column) tuples, skip matches omitted.\n\n"
     "Raises:\n"
     "    error: If some position is matched by no rule (carrying .offset/.line/.column)."},
    {"scan_start", scilex_scan_start, METH_VARARGS,
     "scan_start(handle, text, eof=False)\n"
     "Begin a lazy scan; returns a cursor capsule for scan_next().\n\n"
     "Args:\n"
     "    handle (capsule): A handle from compile().\n"
     "    text (str): The source to scan.\n"
     "    eof (bool): Yield a terminal end_of_input token.\n\n"
     "Returns:\n"
     "    capsule: A scan cursor; pass it to scan_next().\n\n"
     "Raises:\n"
     "    error: If the first position is matched by no rule."},
    {"scan_next", scilex_scan_next, METH_VARARGS,
     "scan_next(cursor)\n"
     "Return the next token tuple from a scan cursor, or None at the end.\n\n"
     "Args:\n"
     "    cursor (capsule): A cursor from scan_start().\n\n"
     "Returns:\n"
     "    tuple | None: (kind, lexeme, offset, line, column), or None when exhausted.\n\n"
     "Raises:\n"
     "    error: If some position is matched by no rule (after earlier tokens are yielded)."},
    {"layout", scilex_layout, METH_VARARGS,
     "layout(tokens)\n"
     "Insert NEWLINE/INDENT/DEDENT tokens from indentation.\n\n"
     "Args:\n"
     "    tokens (sequence): An end_of_input-terminated sequence of\n"
     "        (kind, lexeme, offset, line, column) tuples.\n\n"
     "Returns:\n"
     "    list: The layout-aware tuples (still end_of_input-terminated).\n\n"
     "Raises:\n"
     "    error: On a dedent to an indentation no open block used (with .offset/.line/.column)."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "_scilex",
    "SciLex generic maximal-munch lexer C++ core (built on REAL).", -1,
    module_methods, nullptr, nullptr, nullptr, nullptr,
};

} // namespace

PyMODINIT_FUNC PyInit__scilex() // PyMODINIT_FUNC already implies extern "C"
{
    PyObject* module = PyModule_Create(&module_def);
    if (module == nullptr) {
        return nullptr;
    }
    error_type = PyErr_NewException("scilex.error", nullptr, nullptr);
    if (error_type == nullptr || PyModule_AddObject(module, "error", Py_NewRef(error_type)) < 0) {
        Py_DECREF(module);
        return nullptr;
    }
    return module;
}
