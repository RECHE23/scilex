// SciLex — Python extension module (CPython Limited API, abi3).
//
// Exposes the generic maximal-munch lexer:
//   _scilex.compile(rules) -> capsule       # rules: [(kind:int, pattern:str, skip:bool), …]
//   _scilex.tokenize(handle, text) -> list   # [(kind, lexeme, offset, line, column), …]
//   _scilex.error                            # raised on a bad pattern or unlexable input
//
// A compiled lexer (held in a PyCapsule) owns its REAL regexes, so the rules are
// compiled once and reused. Tokenization runs on UTF-8 bytes — SciLex's model —
// so offset/column are byte indices (0- and 1-based) and lexemes are decoded
// back to str.

#define PY_SSIZE_T_CLEAN
#define Py_LIMITED_API 0x030A0000
#include <Python.h>

#include <scilex/scilex.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

PyObject* error_type = nullptr; // scilex.error

constexpr const char* CAPSULE_NAME = "scilex.lexer";

// Capsule destructor: frees the lexer when the Python handle is collected.
void capsule_free(PyObject* capsule)
{
    delete static_cast<scilex::lexer*>(PyCapsule_GetPointer(capsule, CAPSULE_NAME));
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

// _scilex.tokenize(handle, text) -> list of (kind, lexeme, offset, line, column).
PyObject* scilex_tokenize(PyObject* /*self*/, PyObject* args)
{
    PyObject* capsule  = nullptr;
    PyObject* text_obj = nullptr;
    if (PyArg_ParseTuple(args, "OU", &capsule, &text_obj) == 0) {
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
    char*     data = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(utf8, &data, &size) < 0) {
        Py_DECREF(utf8);
        return nullptr;
    }
    PyObject* result = nullptr;
    try {
        const std::string                text {data, static_cast<std::size_t>(size)};
        const std::vector<scilex::token> tokens {lexer->tokenize(text)};
        result = PyList_New(static_cast<Py_ssize_t>(tokens.size()));
        if (result != nullptr) {
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const scilex::token& token {tokens[i]};
                PyObject*            tuple {Py_BuildValue(
                    "(is#nnn)", token.kind, token.lexeme.data(),
                    static_cast<Py_ssize_t>(token.lexeme.size()),
                    static_cast<Py_ssize_t>(token.start.offset),
                    static_cast<Py_ssize_t>(token.start.line),
                    static_cast<Py_ssize_t>(token.start.column))};
                if (tuple == nullptr) {
                    Py_CLEAR(result);
                    break;
                }
                PyList_SetItem(result, static_cast<Py_ssize_t>(i), tuple); // steals ref
            }
        }
    }
    catch (const std::exception& error) {
        PyErr_SetString(error_type, error.what());
        result = nullptr;
    }
    Py_DECREF(utf8);
    return result;
}

PyMethodDef module_methods[] = {
    {"compile", scilex_compile, METH_VARARGS,
     "compile(rules)\n"
     "Compile an ordered list of (kind, pattern, skip) rules into a lexer handle.\n\n"
     "Args:\n"
     "    rules (sequence): Triples (kind:int, pattern:str, skip:bool).\n\n"
     "Returns:\n"
     "    capsule: An opaque compiled-lexer handle for tokenize().\n\n"
     "Raises:\n"
     "    error: If a pattern is an invalid regex."},
    {"tokenize", scilex_tokenize, METH_VARARGS,
     "tokenize(handle, text)\n"
     "Tokenize text with a compiled-lexer handle.\n\n"
     "Args:\n"
     "    handle (capsule): A handle from compile().\n"
     "    text (str): The source to tokenize.\n\n"
     "Returns:\n"
     "    list: (kind, lexeme, offset, line, column) tuples, skip matches omitted.\n\n"
     "Raises:\n"
     "    error: If some position is matched by no rule."},
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
