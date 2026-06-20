/*!
 * \file math.hpp
 * \brief An arithmetic lexer grammar — the calculator front, tied to SciParse.
 *
 * This is the lexical front of an expression calculator: numbers (integer,
 * decimal, or scientific), the arithmetic operators (`+ - * / ^ %`), parentheses,
 * commas, and identifiers (function names like `sin`, constants like `pi`,
 * variables like `x`). It is the same token shape SciParse's `examples/calc.cpp`
 * consumes — the REAL → SciLex → SciParse stack, lexer end first. A scientific
 * number such as `2.5e-3` munches into one NUMBER (the `-` is its exponent sign,
 * not a minus operator).
 *
 * Honest scope: it lexes the calculator surface; turning the tokens into an
 * expression tree (precedence, associativity) is SciParse's job, not the lexer's.
 */
#ifndef SCILEX_EXAMPLE_MATH_HPP
#define SCILEX_EXAMPLE_MATH_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::math {

  //! \brief Arithmetic token kinds (whitespace is a skip rule).
  enum kind
  {
    ws,
    number,
    ident,
    plus,
    minus,
    star,
    slash,
    caret,
    percent,
    lparen,
    rparen,
    comma,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case number: return "NUMBER";
      case ident:  return "NAME";
      case plus:   return "+";
      case minus:  return "-";
      case star:   return "*";
      case slash:  return "/";
      case caret:  return "^";
      case percent: return "%";
      case lparen: return "(";
      case rparen: return ")";
      case comma:  return ",";
      default:     return "?";
    }
  }

  //! \brief Builds the calculator lexer. The NUMBER rule covers integer,
  //!        decimal and scientific forms, so an exponent sign stays inside the
  //!        number rather than splitting off as a minus operator.
  inline std::vector<scilex::rule> make_rules()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({number, real::regex("[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?"), false});
    rules.push_back({ident, real::regex("[A-Za-z_][A-Za-z0-9_]*"), false});
    rules.push_back({plus, real::regex("\\+"), false});
    rules.push_back({minus, real::regex("-"), false});
    rules.push_back({star, real::regex("\\*"), false});
    rules.push_back({slash, real::regex("/"), false});
    rules.push_back({caret, real::regex("\\^"), false});
    rules.push_back({percent, real::regex("%"), false});
    rules.push_back({lparen, real::regex("\\("), false});
    rules.push_back({rparen, real::regex("\\)"), false});
    rules.push_back({comma, real::regex(","), false});
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A calculator expression: functions, a scientific number, every
  //!        operator, and nested parentheses.
  inline constexpr std::string_view sample {
    R"math(sin(x) + 2.5e-3 * (a - 1) ^ 2 % 3)math"};

  //! \brief Self-check (so `make example` gates): the distinctive invariants —
  //!        a scientific number munches whole (one NUMBER carrying an `e`), the
  //!        operators are present, and the parentheses balance. \return `true`.
  inline bool self_check()
  {
    const scilex::lexer              lex        {make_lexer()};
    const std::vector<scilex::token> toks       {lex.tokenize(sample)};
    bool                             scientific {false};
    bool                             saw_plus   {false};
    bool                             saw_caret  {false};
    int                              opens      {0};
    int                              closes     {0};
    for (const scilex::token& tok : toks) {
      if (tok.kind == number
          && (tok.lexeme.find('e') != std::string_view::npos
              || tok.lexeme.find('E') != std::string_view::npos)) {
        scientific = true; // e.g. 2.5e-3 is one NUMBER, exponent sign included
      }
      saw_plus  = saw_plus || (tok.kind == plus);
      saw_caret = saw_caret || (tok.kind == caret);
      if (tok.kind == lparen) {
        ++opens;
      }
      else if (tok.kind == rparen) {
        ++closes;
      }
    }
    return scientific && saw_plus && saw_caret && opens > 0 && opens == closes;
  }
} // namespace scilex::examples::math

#endif // SCILEX_EXAMPLE_MATH_HPP
