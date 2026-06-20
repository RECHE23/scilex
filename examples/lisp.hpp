/*!
 * \file lisp.hpp
 * \brief A Lisp-like lexer grammar — universality at the minimal extreme.
 *
 * The counterpoint to C++: a whole language lexed by a handful of rules.
 * Lisp's surface is parentheses, a quote prefix, string and number literals,
 * `;` line comments, and otherwise *atoms* — a symbol is any maximal run of
 * characters that are not whitespace, a parenthesis, a quote, or a comment
 * start. Operators are just symbols (`+`, `<`, `set!`), so there is no operator
 * table at all; that minimalism is the point this example demonstrates.
 *
 * Honest scope: a single quote prefix (`'`) is recognized; quasiquote /
 * unquote (`` ` ``, `,`, `,@`), the full numeric tower (ratios, radixes),
 * block comments (`#| … |#`) and reader macros are out of this small sample.
 */
#ifndef SCILEX_EXAMPLE_LISP_HPP
#define SCILEX_EXAMPLE_LISP_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::lisp {

  //! \brief Lisp-like token kinds (whitespace and comments are skip rules).
  enum kind
  {
    ws,
    comment,
    lparen,
    rparen,
    str,
    number,
    quote,
    symbol,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case lparen: return "(";
      case rparen: return ")";
      case str:    return "STRING";
      case number: return "NUMBER";
      case quote:  return "QUOTE";
      case symbol: return "SYMBOL";
      default:     return "?";
    }
  }

  //! \brief Builds the Lisp-like lexer. NUMBER precedes SYMBOL so a numeric
  //!        atom is a NUMBER on an equal-length match; SYMBOL is the catch-all
  //!        atom — any run of non-delimiter bytes — so operators need no rules.
  inline std::vector<scilex::rule> make_rules()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({comment, real::regex(";.*"), true});
    rules.push_back({lparen, real::regex("\\("), false});
    rules.push_back({rparen, real::regex("\\)"), false});
    rules.push_back({str, real::regex(R"re("(\\.|[^"\\])*")re"), false});
    rules.push_back({number, real::regex("[+-]?[0-9]+(\\.[0-9]+)?"), false});
    rules.push_back({quote, real::regex("'"), false});
    rules.push_back({symbol, real::regex(R"re([^\s()";']+)re"), false});
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A recursive function: nested, balanced parentheses with symbol and
  //!        number atoms.
  inline constexpr std::string_view sample {
    R"lisp(; a tiny program
(defun fib (n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))
)lisp"};

  //! \brief Self-check (so `make example` gates): the distinctive invariants —
  //!        the parentheses balance (the homoiconic structure) and both atom
  //!        kinds appear (a SYMBOL and a NUMBER). \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex     {make_lexer()};
    const std::vector<scilex::token> toks    {lex.tokenize(sample)};
    int                              opens   {0};
    int                              closes  {0};
    bool                             has_sym {false};
    bool                             has_num {false};
    for (const scilex::token& tok : toks) {
      switch (tok.kind) {
        case lparen: ++opens;          break;
        case rparen: ++closes;         break;
        case symbol: has_sym = true;   break;
        case number: has_num = true;   break;
        default:                       break;
      }
    }
    return opens > 0 && opens == closes && has_sym && has_num;
  }
} // namespace scilex::examples::lisp

#endif // SCILEX_EXAMPLE_LISP_HPP
