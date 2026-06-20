/*!
 * \file python.hpp
 * \brief A Python-like lexer grammar — the indentation / layout example.
 *
 * The point of this one is SciLex's distinctive feature: significant
 * indentation. The base rules tokenize a Python-ish surface (keywords,
 * identifiers, numbers, strings, operators, punctuation; whitespace and `#`
 * comments skipped), then \ref scilex::layout turns the flat stream into a
 * layout-aware one — inserting NEWLINE / INDENT / DEDENT purely from token
 * line/column positions. End to end: `tokenize(src, append)` → `layout(...)`.
 *
 * Honest scope: strings are single- and double-quoted with escapes. Triple-
 * quoted strings and f-strings are not covered — f-strings interpolate
 * expressions and so need context-sensitive (mode-based) lexing, which SciLex
 * does not have yet (it is the measured motivation for a future `modes`
 * capability). Single-mode maximal munch handles everything here.
 */
#ifndef SCILEX_EXAMPLE_PYTHON_HPP
#define SCILEX_EXAMPLE_PYTHON_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>
#include <scilex/layout.hpp>

namespace scilex::examples::python {

  //! \brief Python-like token kinds. Whitespace and comments are skip rules; a
  //!        single \ref keyword kind covers the reserved words (the lexeme says
  //!        which). The reserved layout kinds are printed by \ref kind_name too.
  enum kind
  {
    ws,
    comment,
    keyword,
    ident,
    number,
    str,
    op,
    colon,
    comma,
    dot,
    lparen,
    rparen,
    lbracket,
    rbracket,
    lbrace,
    rbrace,
  };

  //! \brief A printable name for each kind, including the layout / EOF tokens.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case keyword:  return "KEYWORD";
      case ident:    return "NAME";
      case number:   return "NUMBER";
      case str:      return "STRING";
      case op:       return "OP";
      case colon:    return ":";
      case comma:    return ",";
      case dot:      return ".";
      case lparen:   return "(";
      case rparen:   return ")";
      case lbracket: return "[";
      case rbracket: return "]";
      case lbrace:   return "{";
      case rbrace:   return "}";
      case scilex::newline:      return "NEWLINE";
      case scilex::indent:       return "INDENT";
      case scilex::dedent:       return "DEDENT";
      case scilex::end_of_input: return "EOF";
      default:                   return "?";
    }
  }

  //! \brief Builds the base lexer (before the layout pass). Keyword rules come
  //!        before \ref ident, so on an equal-length match (e.g. `if`) the
  //!        keyword wins; a longer identifier (`iffy`) still wins by munch.
  inline std::vector<scilex::rule> make_rules()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({comment, real::regex("#.*"), true}); // `.` stops at the newline
    for (const char* word : {"def", "return", "if", "elif", "else", "while", "for", "in",
                             "and", "or", "not", "None", "True", "False", "pass", "break",
                             "continue", "import", "as", "class", "lambda"}) {
      rules.push_back({keyword, real::regex(word), false});
    }
    rules.push_back({ident, real::regex("[A-Za-z_][A-Za-z0-9_]*"), false});
    rules.push_back({number, real::regex("[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?"), false});
    // single- and double-quoted strings, each with `\`-escapes
    rules.push_back({str, real::regex(R"re('(\\.|[^'\\])*'|"(\\.|[^"\\])*")re"), false});
    // multi-byte operators first so maximal munch takes them over the single-byte set
    rules.push_back({op, real::regex(R"re(:=|==|!=|<=|>=|->|//|\*\*|<<|>>|[-+*/%<>=&|^~])re"), false});
    rules.push_back({colon, real::regex(":"), false});
    rules.push_back({comma, real::regex(","), false});
    rules.push_back({dot, real::regex("\\."), false});
    rules.push_back({lparen, real::regex("\\("), false});
    rules.push_back({rparen, real::regex("\\)"), false});
    rules.push_back({lbracket, real::regex("\\["), false});
    rules.push_back({rbracket, real::regex("\\]"), false});
    rules.push_back({lbrace, real::regex("\\{"), false});
    rules.push_back({rbrace, real::regex("\\}"), false});
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A small function with nested blocks, so the layout pass must emit
  //!        matching INDENT / DEDENT pairs.
  inline constexpr std::string_view sample {
    R"py(def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
)py"};

  //! \brief Self-check (so `make example` gates): the sample must lex, and the
  //!        layout pass must emit at least one INDENT and exactly as many
  //!        DEDENTs (balanced nesting). \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex     {make_lexer()};
    const std::vector<scilex::token> flat    {lex.tokenize(sample, scilex::eof_policy::append)};
    const std::vector<scilex::token> laid    {scilex::layout(flat)};
    int                              indents {0};
    int                              dedents {0};
    for (const scilex::token& tok : laid) {
      if (tok.kind == scilex::indent) {
        ++indents;
      }
      else if (tok.kind == scilex::dedent) {
        ++dedents;
      }
    }
    return indents > 0 && indents == dedents;
  }
} // namespace scilex::examples::python

#endif // SCILEX_EXAMPLE_PYTHON_HPP
