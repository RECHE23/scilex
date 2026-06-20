/*!
 * \file json.hpp
 * \brief A JSON lexer grammar — the canonical "real data format" example.
 *
 * Tokenizes JSON (RFC 8259) over the SciLex API: structural punctuation,
 * strings with escapes, numbers, and the `true` / `false` / `null` literals —
 * a real, widely-used format recognized with a handful of REAL patterns and
 * maximal munch, no engine change, just a rule list.
 *
 * Lexical grain only: the string rule recognizes a `"`-delimited run of
 * `\`-escapes and ordinary bytes; checking that an escape is one of JSON's
 * (e.g. `\u` followed by four hex digits) is a parser/semantic concern, not the
 * lexer's — the string is still recognized as a single token.
 *
 * The grammar lives in a named namespace so it is one reusable source of truth:
 * the `tokens` demo, and later the benchmarks, the fuzzer and the CLI, all
 * consume this single definition.
 */
#ifndef SCILEX_EXAMPLE_JSON_HPP
#define SCILEX_EXAMPLE_JSON_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::json {

  //! \brief JSON token kinds (whitespace is a skip rule, so it is never emitted).
  enum kind
  {
    ws,
    lbrace,
    rbrace,
    lbracket,
    rbracket,
    colon,
    comma,
    str,
    number,
    kw_true,
    kw_false,
    kw_null,
  };

  //! \brief A printable name for each kind (for the demo output).
  inline const char* kind_name(int k)
  {
    switch (k) {
      case lbrace:   return "{";
      case rbrace:   return "}";
      case lbracket: return "[";
      case rbracket: return "]";
      case colon:    return ":";
      case comma:    return ",";
      case str:      return "STRING";
      case number:   return "NUMBER";
      case kw_true:  return "true";
      case kw_false: return "false";
      case kw_null:  return "null";
      default:       return "?";
    }
  }

  //! \brief Builds the JSON lexer: an ordered rule list, whitespace skipped.
  //!
  //! Order is only a tie-break; here every rule has a distinct first byte (or a
  //! longer match), so maximal munch alone resolves each token.
  inline scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({lbrace, real::regex("\\{"), false});
    rules.push_back({rbrace, real::regex("\\}"), false});
    rules.push_back({lbracket, real::regex("\\["), false});
    rules.push_back({rbracket, real::regex("\\]"), false});
    rules.push_back({colon, real::regex(":"), false});
    rules.push_back({comma, real::regex(","), false});
    // "  then (escape: \ + any byte) or (any byte but " and \)  then  "
    rules.push_back({str, real::regex(R"re("(\\.|[^"\\])*")re"), false});
    // optional sign, integer part, optional fraction, optional exponent
    rules.push_back({number, real::regex("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?"), false});
    rules.push_back({kw_true, real::regex("true"), false});
    rules.push_back({kw_false, real::regex("false"), false});
    rules.push_back({kw_null, real::regex("null"), false});
    return scilex::lexer(std::move(rules));
  }

  //! \brief A document exercising every token kind, including a string whose
  //!        escapes (`\t`, `\"`, `é`) must munch into a single STRING token.
  inline constexpr std::string_view sample {
    R"json({
  "name": "SciLex",
  "version": 2026.6,
  "universal": true,
  "modes": null,
  "tokens": ["string", "number", -42, 1e3],
  "nested": {"escaped": "a\t\"b\"é"}
})json"};

  //! \brief Self-check (so `make example` gates, not just builds): the sample
  //!        must tokenize, be brace-delimited, and munch the escaped string into
  //!        one token. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex  {make_lexer()};
    const std::vector<scilex::token> toks {lex.tokenize(sample)};
    if (toks.empty() || toks.front().kind != lbrace || toks.back().kind != rbrace) {
      return false;
    }
    for (const scilex::token& tok : toks) {
      if (tok.kind == str && tok.lexeme.find("é") != std::string_view::npos) {
        return true; // the whole "a\t\"b\"é" came back as one STRING
      }
    }
    return false;
  }
} // namespace scilex::examples::json

#endif // SCILEX_EXAMPLE_JSON_HPP
