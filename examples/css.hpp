/*!
 * \file css.hpp
 * \brief A CSS-like lexer grammar — the dimensions / hash-colors example.
 *
 * CSS has a token shape the others don't: a number glued to a **unit**
 * (`12px`, `1.5em`, `50%`) is one DIMENSION / PERCENTAGE token (maximal munch
 * over the bare number), and a `#`-prefixed run (`#1a2b3c`, `#fff`) is one HASH
 * token (a hex colour or an id — the parser decides which). Plus at-rules
 * (`@media`), strings, selectors, and block comments (skipped).
 *
 * Honest scope: a representative slice — no attribute selectors (`[a=b]`), no
 * `!important`, no `url(...)` / nested-function grammar. Single-mode maximal
 * munch covers what is here.
 */
#ifndef SCILEX_EXAMPLE_CSS_HPP
#define SCILEX_EXAMPLE_CSS_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::css {

  //! \brief CSS-like token kinds (whitespace and comments are skip rules).
  enum kind
  {
    ws,
    comment,
    at_keyword,
    hash,
    str,
    dimension,
    percentage,
    number,
    ident,
    lbrace,
    rbrace,
    lparen,
    rparen,
    colon,
    semicolon,
    comma,
    delim,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case at_keyword: return "AT";
      case hash:       return "HASH";
      case str:        return "STRING";
      case dimension:  return "DIM";
      case percentage: return "PCT";
      case number:     return "NUMBER";
      case ident:      return "NAME";
      case lbrace:     return "{";
      case rbrace:     return "}";
      case lparen:     return "(";
      case rparen:     return ")";
      case colon:      return ":";
      case semicolon:  return ";";
      case comma:      return ",";
      case delim:      return "DELIM";
      default:         return "?";
    }
  }

  //! \brief Builds the CSS-like lexer. DIMENSION and PERCENTAGE precede the
  //!        bare NUMBER rule, so `12px` / `50%` munch whole rather than leaving
  //!        a dangling unit; HASH and at-rules precede IDENT.
  inline scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({comment, real::regex(R"re(/\*([^*]|\*+[^*/])*\*+/)re"), true});
    rules.push_back({at_keyword, real::regex("@[A-Za-z][A-Za-z-]*"), false});
    rules.push_back({hash, real::regex("#[A-Za-z0-9_-]+"), false});
    rules.push_back({str, real::regex(R"re("(\\.|[^"\\])*"|'(\\.|[^'\\])*')re"), false});
    // number + unit (munch over the bare number that follows)
    rules.push_back({dimension, real::regex(R"re(-?[0-9]+(\.[0-9]+)?(px|em|rem|ex|ch|vmin|vmax|vw|vh|cm|mm|in|pt|pc|deg|rad|ms|s|fr))re"), false});
    rules.push_back({percentage, real::regex(R"re(-?[0-9]+(\.[0-9]+)?%)re"), false});
    rules.push_back({number, real::regex("-?[0-9]+(\\.[0-9]+)?"), false});
    rules.push_back({ident, real::regex("-?[A-Za-z_][A-Za-z0-9_-]*"), false});
    rules.push_back({lbrace, real::regex("\\{"), false});
    rules.push_back({rbrace, real::regex("\\}"), false});
    rules.push_back({lparen, real::regex("\\("), false});
    rules.push_back({rparen, real::regex("\\)"), false});
    rules.push_back({colon, real::regex(":"), false});
    rules.push_back({semicolon, real::regex(";"), false});
    rules.push_back({comma, real::regex(","), false});
    rules.push_back({delim, real::regex(R"re([-.*>+~/])re"), false});
    return scilex::lexer(std::move(rules));
  }

  //! \brief A stylesheet exercising a hash colour, a dimension and a percentage.
  inline constexpr std::string_view sample {
    R"css(/* a rule */
.btn {
  color: #1a2b3c;
  width: 12px;
  opacity: 50%;
  margin: 0 auto;
}
@media (min-width: 600px) { body { font-size: 1.5em; } }
)css"};

  //! \brief Self-check (so `make example` gates): the distinctive invariants —
  //!        `#1a2b3c` is one HASH, `12px` one DIMENSION, `50%` one PERCENTAGE.
  //!        \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex  {make_lexer()};
    const std::vector<scilex::token> toks {lex.tokenize(sample)};
    bool                             hex  {false};
    bool                             dim  {false};
    bool                             pct  {false};
    for (const scilex::token& tok : toks) {
      hex = hex || (tok.kind == hash && tok.lexeme == "#1a2b3c");
      dim = dim || (tok.kind == dimension && tok.lexeme == "12px");
      pct = pct || (tok.kind == percentage && tok.lexeme == "50%");
    }
    return hex && dim && pct;
  }
} // namespace scilex::examples::css

#endif // SCILEX_EXAMPLE_CSS_HPP
