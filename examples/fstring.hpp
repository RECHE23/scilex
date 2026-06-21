/*!
 * \file fstring.hpp
 * \brief Python-style f-strings — the contextual-lexing (modes) showcase.
 *
 * An f-string interleaves three lexical contexts that a flat rule list cannot
 * tell apart, because the SAME byte means different things in each:
 *
 *   - in **code**, `{` is an operator and `"` opens a string;
 *   - inside an **f-string body** (`fstr`), text is literal until `{` starts an
 *     interpolation or `"` closes the string;
 *   - inside an **interpolation** (`interp`), we are back to CODE until `}`.
 *
 * SciLex resolves this with a per-scan mode stack. The decisive design point is
 * that the interpolation is *real code*: every code rule is declared active in
 * BOTH `default` and `interp` (`in_mode = {"default", "interp"}`), so the
 * interpolation reuses the exact same rules — no duplicated grammar. And because
 * the opener is itself a code rule, an f-string can appear inside an
 * interpolation, nesting through the stack to any depth:
 *
 *     f"{ f"{x}" }"   ->   fstr · interp · fstr · interp · ... balanced pops
 *
 * The whole grammar is still just a rule list: the only additions over the flat
 * examples are `in_mode` (which modes a rule is active in) and an `action`
 * (push/pop the mode stack). The engine is unchanged.
 */
#ifndef SCILEX_EXAMPLE_FSTRING_HPP
#define SCILEX_EXAMPLE_FSTRING_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::fstring {

  //! \brief f-string token kinds (whitespace is a skip rule, never emitted).
  enum kind
  {
    ws,           //!< whitespace (skipped)
    ident,        //!< identifier / keyword (code)
    number,       //!< numeric literal (code)
    op,           //!< operator / punctuation run (code)
    fstr_open,    //!< the `f"` that opens an f-string (code) — pushes `fstr`
    text,         //!< a literal chunk of an f-string body, incl. `{{` / `}}`
    fstr_close,   //!< the `"` that closes an f-string — pops `fstr`
    interp_open,  //!< the `{` that opens an interpolation — pushes `interp`
    interp_close, //!< the `}` that closes an interpolation — pops `interp`
  };

  //! \brief A printable name for each kind (for the demo output).
  inline const char* kind_name(int k)
  {
    switch (k) {
      case ws:           return "WS";
      case ident:        return "IDENT";
      case number:       return "NUMBER";
      case op:           return "OP";
      case fstr_open:    return "FSTR_OPEN";
      case text:         return "TEXT";
      case fstr_close:   return "FSTR_CLOSE";
      case interp_open:  return "INTERP_OPEN";
      case interp_close: return "INTERP_CLOSE";
      default:           return "?";
    }
  }

  //! \brief A push/pop/set action targeting \p target (target ignored for pop).
  inline scilex::mode_action go(scilex::mode_action::op operation,
                                const char*             target = "")
  {
    return {.operation = operation, .target = target};
  }

  //! \brief A rule active in \p modes, optionally skipped, optionally with an
  //!        action — keeps each rule a single, uncrustify-stable line below.
  inline scilex::rule rule(int                                kind,
                           const char*                        pattern,
                           std::vector<std::string>           modes,
                           bool                               skip   = false,
                           std::optional<scilex::mode_action> action = std::nullopt)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = action;
    return r;
  }

  //! \brief Builds the f-string lexer: a code mode (`default`), an f-string body
  //!        mode (`fstr`), and an interpolation mode (`interp`).
  //!
  //! The code rules are active in `default` AND `interp` (one shared list — the
  //! interpolation is genuine code, not a copy). The opener is a code rule too,
  //! so f-strings nest through the stack. `fstr` and `interp` are entered and left
  //! by push/pop actions on the delimiter rules.
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;

    // The contexts where ordinary code is lexed: top level and interpolations.
    const std::vector<std::string> code {"default", "interp"};

    std::vector<scilex::rule> rules;
    // --- code (shared by `default` and `interp`, no duplication) -------------
    rules.push_back(rule(ws, R"re(\s+)re", code, /*skip=*/ true));
    rules.push_back(rule(ident, R"re([A-Za-z_][A-Za-z0-9_]*)re", code));
    rules.push_back(rule(number, R"re([0-9]+(\.[0-9]+)?)re", code));
    rules.push_back(rule(op, R"re([-+*/%=<>!&|^,.:;()]+)re", code));
    // `f"` opens an f-string body; active in code, so it nests inside interps.
    rules.push_back(rule(fstr_open, R"re(f")re", code, false, go(op_t::push, "fstr")));

    // --- f-string body (`fstr`) ---------------------------------------------
    // Literal text up to a delimiter; `{{` and `}}` are escaped braces (one TEXT
    // token each — and longer than a bare `{`, so maximal munch keeps them whole).
    rules.push_back(rule(text, R"re(\{\{|\}\}|[^{}"]+)re", {"fstr"}));
    rules.push_back(rule(interp_open, R"re(\{)re", {"fstr"}, false, go(op_t::push, "interp")));
    rules.push_back(rule(fstr_close, R"re(")re", {"fstr"}, false, go(op_t::pop)));

    // --- interpolation (`interp`) -------------------------------------------
    // The code rules above are already active here; only the closing `}` is special.
    rules.push_back(rule(interp_close, R"re(\})re", {"interp"}, false, go(op_t::pop)));
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A sample exercising every context: code, an interpolation with an
  //!        operator, a nested f-string, and `{{` / `}}` escapes.
  inline constexpr std::string_view sample {
    R"fstr(greeting = f"Hello, {name}!"
total = f"sum = {count + 1} items"
nested = f"outer {f"inner {x}"} done"
literal = f"{{braces}} and {value}")fstr"};

  //! \brief Self-check (so `make example` gates, not just builds). Asserts the
  //!        three distinctive mode invariants. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer lex {make_lexer()};

    // (1) f"a{x}b" yields the canonical flow, and the interpolated `x` is lexed
    //     by the SAME code IDENT rule as top-level code (no duplicated grammar).
    {
      const std::vector<scilex::token> toks  {lex.tokenize(R"fs(f"a{x}b")fs")};
      const std::vector<int>           kinds {fstr_open, text, interp_open, ident, interp_close, text, fstr_close};
      if (toks.size() != kinds.size()) {
        return false;
      }
      for (std::size_t i {0}; i < kinds.size(); ++i) {
        if (toks[i].kind != kinds[i]) {
          return false;
        }
      }
    }

    // (2) Nesting is the stack at work: an f-string inside an interpolation, the
    //     same `"` and `{` delimiters disambiguated only by the mode stack.
    {
      try {
        (void)lex.tokenize(R"fs(f"{f"{x}"}")fs");
      }
      catch (const scilex::lex_error&) {
        return false; // a balanced nest must lex cleanly
      }
    }

    // (3) An unterminated f-string reports the OPENING position (error #3).
    {
      try {
        (void)lex.tokenize(R"fs(f"abc)fs"); // never closed
        return false;                       // must throw
      }
      catch (const scilex::lex_error& error) {
        if (error.where().offset != 0) {
          return false; // the report must point at the `f"`
        }
      }
    }
    return true;
  }
} // namespace scilex::examples::fstring

#endif // SCILEX_EXAMPLE_FSTRING_HPP
