/*!
 * \file css.hpp
 * \brief A CSS lexer grammar — the dimensions / hash-colors example.
 *
 * CSS has a token shape the others don't: a number glued to a **unit**
 * (`12px`, `1.5em`, `50%`) is one DIMENSION / PERCENTAGE token (maximal munch
 * over the bare number), and a `#`-prefixed run (`#1a2b3c`, `#fff`) is one HASH
 * token (a hex colour or an id — the parser decides which). Plus at-rules
 * (`@media`), strings, selectors, and block comments (skipped).
 *
 * It reads CSS as tinycss2 tokenizes it (`make css-oracle` compares the two over
 * the style sheets of the documentation toolchain): identifiers with escapes and
 * custom properties (`--x`), functions (`calc(`), unquoted `url(…)`, numbers with
 * any unit and exponent (`.5em`, `1e3px`), unicode ranges (`U+0025-00FF`), the
 * attribute matchers (`~=`, `^=`…), and any other character as a delimiter, so no
 * style sheet stops it. Single-mode maximal munch: a block is its brackets, and
 * which `(` a `)` closes is the parser's concern.
 */
#ifndef SCILEX_EXAMPLE_CSS_HPP
#define SCILEX_EXAMPLE_CSS_HPP

#include <string>
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
    url,           //!< `url(…)` with an unquoted address (a quoted one is a function and a string)
    function,      //!< a name and its `(`: `calc(`, `rgb(`
    unicode_range, //!< `U+0025-00FF`, `U+4??`
    lbracket,
    rbracket,
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
      case url:        return "URL";
      case function:   return "FUNCTION";
      case unicode_range: return "URANGE";
      case lbracket:   return "[";
      case rbracket:   return "]";
      default:         return "?";
    }
  }

  //! \brief Builds the CSS-like lexer. DIMENSION and PERCENTAGE precede the
  //!        bare NUMBER rule, so `12px` / `50%` munch whole rather than leaving
  //!        a dangling unit; HASH and at-rules precede IDENT.
  inline std::vector<scilex::rule> make_rules()
  {
    // CSS Syntax's escape, name characters and identifier; a non-ASCII code point is a name character.
    const std::string         escape         {R"re(\\([0-9A-Fa-f]{1,6}[ \t\n\r\f]?|[^\n\r\f0-9A-Fa-f]))re"};
    const std::string         name_char      {R"re([A-Za-z0-9_\x{80}-\x{10FFFF}-]|)re" + escape};
    const std::string         name_start     {R"re([A-Za-z_\x{80}-\x{10FFFF}]|)re" + escape};
    const std::string         identifier     {"(--|-?(" + name_start + "))(" + name_char + ")*"};
    const std::string         number_pattern {R"re([+-]?([0-9]+(\.[0-9]+)?|\.[0-9]+)([eE][+-]?[0-9]+)?)re"};
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+", real::flags::ascii), true});
    rules.push_back({comment, real::regex(R"re(/\*([^*]|\*+[^*/])*\*+/)re"), true});
    rules.push_back({delim, real::regex("<!--|-->"), false});
    rules.push_back({unicode_range, real::regex(R"re([uU]\+[0-9A-Fa-f?]{1,6}(-[0-9A-Fa-f]{1,6})?)re"), false});
    rules.push_back({url, real::regex(R"re([uU][rR][lL]\([ \t\n\r\f]*([^"'()\\ \t\n\r\f\x{0}-\x{8}\x{B}\x{E}-\x{1F}\x{7F}]|)re" + escape +
                                      R"re()*[ \t\n\r\f]*\))re"),
                     false});
    rules.push_back({function, real::regex(identifier + R"re(\()re"), false});
    rules.push_back({at_keyword, real::regex("@" + identifier), false});
    rules.push_back({hash, real::regex("#(" + name_char + ")+"), false});
    rules.push_back({str, real::regex(R"re("([^"\\\n]|\\[\s\S])*"|'([^'\\\n]|\\[\s\S])*')re"), false});
    // A number before a dimension: `1e3` is a number, whose exponent a unit `e3` would also match.
    rules.push_back({number, real::regex(number_pattern), false});
    rules.push_back({percentage, real::regex(number_pattern + "%"), false});
    rules.push_back({dimension, real::regex(number_pattern + identifier), false});
    rules.push_back({ident, real::regex(identifier), false});
    rules.push_back({delim, real::regex(R"re(~=|\|=|\^=|\$=|\*=|\|\|)re"), false});
    rules.push_back({lbrace, real::regex("\\{"), false});
    rules.push_back({rbrace, real::regex("\\}"), false});
    rules.push_back({lparen, real::regex("\\("), false});
    rules.push_back({rparen, real::regex("\\)"), false});
    rules.push_back({lbracket, real::regex("\\["), false});
    rules.push_back({rbracket, real::regex("\\]"), false});
    rules.push_back({colon, real::regex(":"), false});
    rules.push_back({semicolon, real::regex(";"), false});
    rules.push_back({comma, real::regex(","), false});
    // Any other character, `!` and a lone `\` included: no style sheet stops the lexer.
    rules.push_back({delim, real::regex(R"re([\s\S])re"), false});
    return rules;
  }

  //! \brief make_lexer() under the error policy and column unit given, its modes and DFA unchanged.
  //! \param[in] errors  What to do at a byte no rule can lex (see \ref scilex::lexer).
  //! \param[in] columns The unit token columns are counted in (see \ref scilex::lexer).
  //! \return The lexer.
  inline scilex::lexer make_lexer_with(scilex::error_policy errors,
                                       scilex::column_unit  columns)
  {
    return scilex::lexer(make_rules(), {}, {"default"}, errors, columns);
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  //!
  //! This grammar is mono-mode, greedy and assertion-free, so "default" is opted into
  //! the DFA fast path: one DFA pass replaces the per-rule dispatch (~20×), with the
  //! Pike engine as the floor and an identical token stream (see \ref scilex::lexer).
  inline scilex::lexer make_lexer()
  {
    return make_lexer_with(scilex::error_policy::raise, scilex::column_unit::bytes);
  }

  //! \brief A stylesheet exercising a hash colour, a dimension and a percentage.
  inline constexpr std::string_view sample {
    R"css(/* a rule */
.btn {
  color: #1a2b3c;
  width: 12px;
  opacity: 50%;
  margin: 0 auto !important;
  background: url(img/bg.png) no-repeat;
  --gap: calc(1e1px + .5em);
}
a[href^="https"] { unicode-range: U+0025-00FF; }
@media (min-width: 600px) { body { font-size: 1.5em; } }
)css"};

  //! \brief Self-check (so `make example` gates): the distinctive invariants —
  //!        `#1a2b3c` is one HASH, `12px` one DIMENSION, `50%` one PERCENTAGE, an
  //!        unquoted `url(…)` one URL, `calc(` one FUNCTION, `U+0025-00FF` one
  //!        unicode range, and "default" is actually DFA-accelerated (not silently
  //!        fallen back to Pike).
  //!        \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex  {make_lexer()};
    const std::vector<scilex::token> toks {lex.tokenize(sample)};
    bool                             hex  {false};
    bool                             dim  {false};
    bool                             pct  {false};
    bool                             link {false};
    bool                             call {false};
    bool                             span {false};
    for (const scilex::token& tok : toks) {
      hex  = hex || (tok.kind == hash && tok.lexeme == "#1a2b3c");
      dim  = dim || (tok.kind == dimension && tok.lexeme == "12px");
      pct  = pct || (tok.kind == percentage && tok.lexeme == "50%");
      link = link || (tok.kind == url && tok.lexeme == "url(img/bg.png)");
      call = call || (tok.kind == function && tok.lexeme == "calc(");
      span = span || (tok.kind == unicode_range && tok.lexeme == "U+0025-00FF");
    }
    bool dfa_active {false};
    for (const std::string& mode : lex.dfa_modes_active()) {
      dfa_active = dfa_active || (mode == "default");
    }
    return hex && dim && pct && link && call && span && dfa_active;
  }
} // namespace scilex::examples::css

#endif // SCILEX_EXAMPLE_CSS_HPP
