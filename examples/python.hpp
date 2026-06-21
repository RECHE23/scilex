/*!
 * \file python.hpp
 * \brief A Python-ish lexer — the modes + layout showcase (a realistic grammar).
 *
 * This is SciLex's two distinctive features in one real grammar: significant
 * indentation (\ref scilex::layout) and contextual lexing (modes). The f-string
 * is the case that needs both — `f"sum={a+b}"` interleaves three contexts where
 * the *same* byte means different things, which a flat rule list cannot separate.
 *
 * What this grammar covers
 *   - realistic code: a full keyword set, identifiers, operators (augmented and
 *     walrus/arrow/ellipsis), `@` decorators, `(` `)` `[` `]`, and `{` `}` as
 *     dict/set punctuation at the top level;
 *   - numbers: decimal/float/exponent, `_` separators, `0x`/`0o`/`0b` bases, and
 *     the `j` imaginary suffix;
 *   - strings: single/double quoted with escapes, and triple-quoted `"""…"""` /
 *     `'''…'''` (one token, spanning newlines, via a dotall lazy regex);
 *   - f-strings `f"…"` / `f'…'` with `{…}` interpolations — nested f-strings and
 *     nested dict/set literals inside an interpolation, tracked by the stack;
 *   - INDENT / DEDENT / NEWLINE via the layout pass.
 *
 * What it does not cover (a realistic *example of modes*, not a full Python lexer
 * — none of these is excluded in principle, they are just beyond this sample):
 *   - implicit line continuation inside `()` `[]` `{}` (the layout pass treats
 *     every newline as significant — the same Layout Awareness gap as YAML's
 *     multi-line flow; see include/scilex/layout.hpp);
 *   - triple-quoted / multi-line f-strings;
 *   - f-string format specs: the `:fmt` tail of `{value:fmt}` lexes as code
 *     (colon + tokens), not as a literal spec;
 *   - bracket depth for the end of an interpolation: only `{` `}` are tracked, so
 *     a literal `}` inside a `[]`/`()` of an interpolation would close it (a rare
 *     edge);
 *   - raw / byte string prefixes (`r"…"`, `b"…"`, `rf"…"`).
 *
 * The modes and the brace-depth insight
 *   Four modes: `default` (top-level code), `fstr_dq` / `fstr_sq` (an f-string
 *   body), and `interp` (an interpolation). The decisive design point is that an
 *   interpolation IS code: every code rule is active in both `default` and
 *   `interp` (`in_mode = {"default", "interp"}`), so the interpolation reuses the
 *   exact same rules with no duplication. And `{` pushes `interp` from inside any
 *   f-string body OR from inside an interpolation, while `}` pops — so the stack
 *   tracks brace depth, exactly like CPython:
 *
 *     f"{ {1:2} }"
 *       f"  push fstr_dq   { push interp   {(dict) push interp
 *       1:2   }(dict) pop   }(interp) pop  " pop
 *
 *   The interpolation ends at the `}` that pops back to the f-string body, not at
 *   the first `}` — a dict/set literal inside `{…}` just nests another level.
 *   The engine is unchanged: the only additions over a flat grammar are `in_mode`
 *   and a push/pop `action` on the delimiter rules.
 */
#ifndef SCILEX_EXAMPLE_PYTHON_HPP
#define SCILEX_EXAMPLE_PYTHON_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>
#include <scilex/layout.hpp>

namespace scilex::examples::python {

  //! \brief Python-ish token kinds. Whitespace and comments are skip rules; one
  //!        \ref keyword kind covers the reserved words (the lexeme says which).
  enum kind
  {
    ws,
    comment,
    keyword,
    ident,
    number,
    str,          //!< single- or double-quoted string
    tstr,         //!< triple-quoted string (one token, may span newlines)
    fstr_open,    //!< `f"` / `f'` — pushes an f-string body mode
    fstr_text,    //!< a literal chunk of an f-string body (incl. `{{` / `}}`)
    fstr_close,   //!< the `"` / `'` that closes an f-string — pops the body mode
    op,
    at,           //!< `@` (decorator, or matrix-mult operator)
    ellipsis,     //!< `...`
    colon,
    comma,
    dot,
    lparen,
    rparen,
    lbracket,
    rbracket,
    lbrace,       //!< top-level `{` (dict/set) — no mode change
    rbrace,       //!< top-level `}` (dict/set) — no mode change
    interp_open,  //!< `{` opening an interpolation — pushes `interp`
    interp_close, //!< `}` closing the innermost interpolation/brace — pops `interp`
  };

  //! \brief A printable name for each kind, including the layout / EOF tokens.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case keyword:      return "KEYWORD";
      case ident:        return "NAME";
      case number:       return "NUMBER";
      case str:          return "STRING";
      case tstr:         return "TSTRING";
      case fstr_open:    return "FSTR_OPEN";
      case fstr_text:    return "FSTR_TEXT";
      case fstr_close:   return "FSTR_CLOSE";
      case op:           return "OP";
      case at:           return "AT";
      case ellipsis:     return "ELLIPSIS";
      case colon:        return ":";
      case comma:        return ",";
      case dot:          return ".";
      case lparen:       return "(";
      case rparen:       return ")";
      case lbracket:     return "[";
      case rbracket:     return "]";
      case lbrace:       return "{";
      case rbrace:       return "}";
      case interp_open:  return "INTERP{";
      case interp_close: return "INTERP}";
      case scilex::newline:      return "NEWLINE";
      case scilex::indent:       return "INDENT";
      case scilex::dedent:       return "DEDENT";
      case scilex::end_of_input: return "EOF";
      default:                   return "?";
    }
  }

  //! \brief A push/pop/set action targeting \p target (target ignored for pop).
  inline scilex::mode_action go(scilex::mode_action::op operation,
                                const char*             target = "")
  {
    return {.operation = operation, .target = target};
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action — keeps
  //!        each rule a single, uncrustify-stable line in \ref make_rules.
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

  //! \brief Builds the modal Python lexer (see the file header for the design).
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;
    const std::vector<std::string> code   {"default", "interp"};            // interpolation is code
    const std::vector<std::string> bodies {"fstr_dq", "fstr_sq", "interp"}; // where `{` opens an interp

    std::vector<scilex::rule> rules;
    // --- trivia (shared by code and interpolations) --------------------------
    rules.push_back(rule(ws, R"re(\s+)re", code, /*skip=*/ true));
    rules.push_back(rule(comment, R"re(#[^\n]*)re", code, /*skip=*/ true));
    // --- triple-quoted strings (dotall + lazy: up to the closing triple) ------
    rules.push_back(rule(tstr, R"re((?s)""".*?"""|'''.*?''')re", code));
    // --- keywords (full set; before ident so an exact keyword wins the tie) ---
    for (const char* word : {"False", "None", "True", "and", "as", "assert", "async", "await",
                             "break", "class", "continue", "def", "del", "elif", "else", "except",
                             "finally", "for", "from", "global", "if", "import", "in", "is",
                             "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
                             "while", "with", "yield"}) {
      rules.push_back(rule(keyword, word, code));
    }
    rules.push_back(rule(ident, R"re([A-Za-z_][A-Za-z0-9_]*)re", code));
    // --- numbers: hex / oct / bin / decimal+float+imaginary / leading-dot -----
    rules.push_back(rule(number, R"re(0[xX][0-9a-fA-F_]+)re", code));
    rules.push_back(rule(number, R"re(0[oO][0-7_]+)re", code));
    rules.push_back(rule(number, R"re(0[bB][01_]+)re", code));
    rules.push_back(rule(number, R"re([0-9][0-9_]*(\.[0-9_]*)?([eE][+-]?[0-9_]+)?[jJ]?)re", code));
    rules.push_back(rule(number, R"re(\.[0-9_]+([eE][+-]?[0-9_]+)?[jJ]?)re", code));
    // --- plain strings (single / double quoted, with escapes) -----------------
    rules.push_back(rule(str, R"re("(\\.|[^"\\\n])*"|'(\\.|[^'\\\n])*')re", code));
    // --- f-string openers (code rules, so f-strings nest inside interpolations) -
    rules.push_back(rule(fstr_open, R"re([fF]")re", code, false, go(op_t::push, "fstr_dq")));
    rules.push_back(rule(fstr_open, R"re([fF]')re", code, false, go(op_t::push, "fstr_sq")));
    // --- operators (longest first: REAL alternation is leftmost) and @ --------
    rules.push_back(rule(op, R"re(//=|>>=|<<=|\*\*=|:=|==|!=|<=|>=|->|//|\*\*|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|@=|[-+*/%<>=&|^~])re", code));
    rules.push_back(rule(at, R"re(@)re", code));
    // --- punctuation (ellipsis before dot; a leading-dot number beats dot) -----
    rules.push_back(rule(ellipsis, R"re(\.\.\.)re", code));
    rules.push_back(rule(colon, R"re(:)re", code));
    rules.push_back(rule(comma, R"re(,)re", code));
    rules.push_back(rule(dot, R"re(\.)re", code));
    rules.push_back(rule(lparen, R"re(\()re", code));
    rules.push_back(rule(rparen, R"re(\))re", code));
    rules.push_back(rule(lbracket, R"re(\[)re", code));
    rules.push_back(rule(rbracket, R"re(\])re", code));
    // --- top-level braces: dict / set literals, no mode change (default only) --
    rules.push_back(rule(lbrace, R"re(\{)re", {"default"}));
    rules.push_back(rule(rbrace, R"re(\})re", {"default"}));
    // --- the core: `{` opens an interpolation from a body OR nests inside one;
    //     `}` pops the innermost. The stack therefore tracks brace depth. --------
    rules.push_back(rule(interp_open, R"re(\{)re", bodies, false, go(op_t::push, "interp")));
    rules.push_back(rule(interp_close, R"re(\})re", {"interp"}, false, go(op_t::pop)));
    // --- f-string bodies: literal text (incl. {{ }} escapes) up to `{` or close -
    rules.push_back(rule(fstr_text, R"re((\\.|\{\{|\}\}|[^{}"\\])+)re", {"fstr_dq"}));
    rules.push_back(rule(fstr_close, R"re(")re", {"fstr_dq"}, false, go(op_t::pop)));
    rules.push_back(rule(fstr_text, R"re((\\.|\{\{|\}\}|[^{}'\\])+)re", {"fstr_sq"}));
    rules.push_back(rule(fstr_close, R"re(')re", {"fstr_sq"}, false, go(op_t::pop)));
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A realistic snippet exercising every covered context: a decorator, a
  //!        multi-line docstring, every number base, f-strings with a nested dict
  //!        interpolation and `{{`/`}}` escapes, the walrus/arrow, and nesting.
  inline constexpr std::string_view sample {
    R"py(@memoize
def stats(values, base=0x1F):
    """Summary line.

    A multi-line docstring with a {brace} and 'quotes'.
    """
    total = 0
    for v in values:
        total += v
    count = len(values)
    avg = total / count if count else 0
    label = f"n={count} sum={total}"
    nested = f"point {{x}} = { {'k': total} }"
    flags = 0o755 | 0b1010
    z = 1_000 + 4.2j
    while (count := count - 1) > 0:
        pass
    return [label, nested, avg, ...]
)py"};

  //! \brief Self-check (so `make example` gates, not just builds). Exercises each
  //!        distinctive feature; returns true on success.
  inline bool self_check()
  {
    const scilex::lexer lex {make_lexer()};

    // (1) f"a{x}b": the canonical flow; the interpolated `x` is the SAME code rule.
    {
      const std::vector<scilex::token> toks {lex.tokenize(R"fs(f"a{x}b")fs")};
      const std::vector<int>           want {fstr_open, fstr_text, interp_open, ident,
                                             interp_close, fstr_text, fstr_close};
      if (toks.size() != want.size()) {
        return false;
      }
      for (std::size_t i {0}; i < want.size(); ++i) {
        if (toks[i].kind != want[i]) {
          return false;
        }
      }
    }
    // (2) Nested f-strings and (3) a nested dict inside an interpolation both lex
    //     cleanly — the stack tracks brace depth (same delimiters, different levels).
    for (const std::string_view nest : {R"fs(f"{f"{x}"}")fs", R"fs(f"{ {1:2} }")fs"}) {
      try {
        (void)lex.tokenize(nest);
      }
      catch (const scilex::lex_error&) {
        return false;
      }
    }
    // (4) A multi-line triple-quoted string is a single token.
    {
      const std::vector<scilex::token> toks {lex.tokenize("\"\"\"a\nb\"\"\"")};
      if (toks.size() != 1 || toks[0].kind != tstr) {
        return false;
      }
    }
    // (5) A decorator and the number bases (hex with `_`, imaginary).
    {
      const std::vector<scilex::token> toks {lex.tokenize("@deco 0xFF_00 4j")};
      const std::vector<int>           want {at, ident, number, number};
      if (toks.size() != want.size()) {
        return false;
      }
      for (std::size_t i {0}; i < want.size(); ++i) {
        if (toks[i].kind != want[i]) {
          return false;
        }
      }
    }
    // (6) The full sample lexes, and the layout pass is balanced (some INDENTs,
    //     as many DEDENTs) over a real multi-line, multi-mode source.
    {
      const std::vector<scilex::token> laid    {scilex::layout(lex.tokenize(sample, scilex::eof_policy::append))};
      int                              indents {0};
      int                              dedents {0};
      for (const scilex::token& tok : laid) {
        indents += static_cast<int>(tok.kind == scilex::indent);
        dedents += static_cast<int>(tok.kind == scilex::dedent);
      }
      if (indents == 0 || indents != dedents) {
        return false;
      }
    }
    // (7) An unterminated f-string reports the OPENING position (error #3).
    {
      try {
        (void)lex.tokenize(R"fs(f"abc)fs");
        return false;
      }
      catch (const scilex::lex_error& error) {
        if (error.where().offset != 0) {
          return false;
        }
      }
    }
    return true;
  }
} // namespace scilex::examples::python

#endif // SCILEX_EXAMPLE_PYTHON_HPP
