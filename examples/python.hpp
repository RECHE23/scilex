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
 *     walrus/arrow/ellipsis), `@` decorators, and `(` `)` `[` `]` `{` `}` as
 *     brackets / dict / set;
 *   - implicit line continuation: code wrapped across lines inside `()` `[]` `{}`
 *     adds no INDENT/DEDENT (Layout Awareness Level A — `bracket` is insignificant);
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
 *   - triple-quoted / multi-line f-strings;
 *   - f-string format specs: the `:fmt` tail of `{value:fmt}` lexes as code
 *     (colon + tokens), not as a literal spec;
 *   - raw / byte string prefixes (`r"…"`, `b"…"`, `rf"…"`).
 *
 * The modes and the brace-depth insight
 *   Five modes: `default` (top-level code), `fstr_dq` / `fstr_sq` (an f-string
 *   body), `interp` (an f-string interpolation), and `bracket` (inside `()` `[]`
 *   `{}`). The decisive design point is that an interpolation and a bracket are both
 *   *code*: every code rule is active in `default`, `interp` and `bracket`, so they
 *   reuse the exact same rules with no duplication. `{` opens an interpolation from
 *   an f-string body (push `interp`) but a dict/set elsewhere (push `bracket`); a
 *   `}` closes whichever the stack top is. `bracket` is layout-insignificant, so
 *   code wrapped across lines in brackets is implicit line continuation:
 *
 *     f"{ {1:2} }"
 *       f"  push fstr_dq   {  push interp   {(dict) push bracket
 *       1:2   }(dict) pop bracket   }(interp) pop interp  " pop fstr_dq
 *
 *   The interpolation ends at the `}` that pops `interp`, not at the dict's `}`
 *   (which pops `bracket`) — the stack tells them apart. The engine is unchanged:
 *   the only additions over a flat grammar are `in_mode` and a push/pop `action`.
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
    lbrace,       //!< `{` (dict / set) — pushes `bracket`
    rbrace,       //!< `}` closing a dict / set — pops `bracket`
    interp_open,  //!< `{` opening an f-string interpolation — pushes `interp`
    interp_close, //!< `}` closing an interpolation — pops `interp`
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

  //! \brief Builds the rule list. \p unicode_identifiers swaps ONLY the identifier rule: the default
  //!        ASCII class, or a Unicode word identifier (`[^\W\d]\w*`, text mode) that recognizes `café`
  //!        and `変数` — the faithful Python 3 behaviour. The Unicode form is a match-time code-point
  //!        predicate, so a DFA-accelerated mode containing it demotes to the general engine (visible
  //!        via \ref scilex::lexer::dfa_modes_active) — the identifier-Unicode vs DFA-speed trade-off.
  inline std::vector<scilex::rule> build_rules(bool unicode_identifiers)
  {
    using op_t = scilex::mode_action::op;
    // Code runs at the top level, inside an interpolation, and inside a bracket
    // (the layout-insignificant continuation mode — see make_lexer).
    const std::vector<std::string> code     {"default", "interp", "bracket"};
    const std::vector<std::string> bodies   {"fstr_dq", "fstr_sq"};           // a `{` here opens an interpolation
    const std::vector<std::string> brackets {"default", "interp", "bracket"}; // ( [ { open a bracket here

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
    // ASCII by default (DFA-friendly); the Unicode form recognizes café / 変数 but is a code-point
    // predicate, so it leaves the DFA fast path — the author's identifier-Unicode vs speed choice.
    rules.push_back(rule(ident,
                         unicode_identifiers ? R"re([^\W\d]\w*)re" : R"re([A-Za-z_][A-Za-z0-9_]*)re",
                         code));
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
    // --- brackets: ( [ { open an insignificant "bracket" mode (Layout Awareness
    //     Level A: implicit line continuation), the closers pop it. Active in code
    //     (default/interp/bracket) so brackets nest; NOT in an f-string body, where
    //     ( [ are literal text and { opens an interpolation (below). --------------
    rules.push_back(rule(lparen, R"re(\()re", brackets, false, go(op_t::push, "bracket")));
    rules.push_back(rule(rparen, R"re(\))re", {"bracket"}, false, go(op_t::pop)));
    rules.push_back(rule(lbracket, R"re(\[)re", brackets, false, go(op_t::push, "bracket")));
    rules.push_back(rule(rbracket, R"re(\])re", {"bracket"}, false, go(op_t::pop)));
    rules.push_back(rule(lbrace, R"re(\{)re", brackets, false, go(op_t::push, "bracket")));    // dict / set
    rules.push_back(rule(rbrace, R"re(\})re", {"bracket"}, false, go(op_t::pop)));
    // --- f-string interpolation: a `{` in a string body opens it (push interp); a
    //     `}` while in interp closes it. A `}` while in a bracket pops the bracket
    //     (above) — the stack top tells an interpolation `}` from a dict `}`. -------
    rules.push_back(rule(interp_open, R"re(\{)re", bodies, false, go(op_t::push, "interp")));
    rules.push_back(rule(interp_close, R"re(\})re", {"interp"}, false, go(op_t::pop)));
    // --- f-string bodies: literal text (incl. {{ }} escapes) up to `{` or close -
    rules.push_back(rule(fstr_text, R"re((\\.|\{\{|\}\}|[^{}"\\])+)re", {"fstr_dq"}));
    rules.push_back(rule(fstr_close, R"re(")re", {"fstr_dq"}, false, go(op_t::pop)));
    rules.push_back(rule(fstr_text, R"re((\\.|\{\{|\}\}|[^{}'\\])+)re", {"fstr_sq"}));
    rules.push_back(rule(fstr_close, R"re(')re", {"fstr_sq"}, false, go(op_t::pop)));
    return rules;
  }

  //! \brief The standard (ASCII-identifier) rule list — the registry entry point.
  inline std::vector<scilex::rule> make_rules()
  {
    return build_rules(false);
  }

  //! \brief The Unicode-identifier variant's rule list: identical to \ref make_rules but for the
  //!        identifier rule, which recognizes Unicode word identifiers (café, 変数). See \ref build_rules
  //!        for the DFA trade-off.
  inline std::vector<scilex::rule> make_rules_unicode()
  {
    return build_rules(true);
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    // "bracket" is layout-insignificant (Layout Awareness Level A): code spanning
    // several lines inside ( ) [ ] { } is implicit line continuation — no INDENT.
    return scilex::lexer(make_rules(), {"bracket"});
  }

  //! \brief The Unicode-identifier variant lexer — same grammar, Unicode identifiers.
  inline scilex::lexer make_lexer_unicode()
  {
    return scilex::lexer(make_rules_unicode(), {"bracket"});
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
    // (6) The full sample lexes, and the (policy-aware) layout pass is balanced —
    //     some INDENTs, as many DEDENTs — over a real multi-line, multi-mode source.
    {
      const std::vector<scilex::token> laid {
        scilex::layout(lex.tokenize(sample, scilex::eof_policy::append), lex.mode_significant())};
      int indents                           {0};
      int dedents                           {0};
      for (const scilex::token& tok : laid) {
        indents += static_cast<int>(tok.kind == scilex::indent);
        dedents += static_cast<int>(tok.kind == scilex::dedent);
      }
      if (indents == 0 || indents != dedents) {
        return false;
      }
    }
    // (6b) Implicit line continuation (Layout Awareness Level A): a multi-line call
    //      inside ( ) adds NO INDENT/DEDENT — the bracket mode is insignificant.
    {
      const std::vector<scilex::token> laid {
        scilex::layout(lex.tokenize("foo(\n    x,\n    y,\n)\n", scilex::eof_policy::append),
                       lex.mode_significant())};
      for (const scilex::token& tok : laid) {
        if (tok.kind == scilex::indent || tok.kind == scilex::dedent) {
          return false;
        }
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

  //! \brief Self-check for the Unicode-identifier variant: the distinctive invariant is that a Unicode
  //!        word identifier is one token (café, a CJK word, a 4-byte astral letter), the ASCII grammar
  //!        is NOT, and the mode holding the code-point predicate is not DFA-accelerated (the trade-off
  //!        as tested behaviour, not a doc promise).
  inline bool self_check_unicode()
  {
    const scilex::lexer lex {make_lexer_unicode()};
    // café is one identifier (é, U+00E9, is a word char in text mode) — the faithful Python 3 read.
    {
      const std::string                cafe {"caf\xC3\xA9"};
      const std::vector<scilex::token> toks {lex.tokenize(cafe)};
      if (toks.size() != 1 || toks[0].kind != ident || toks[0].lexeme != cafe) {
        return false;
      }
    }
    // A CJK identifier (変数) and a 4-byte astral letter (U+20000, a CJK-Extension-B ideograph) are
    // each a single identifier — the multi-byte cases the column policy also cares about.
    {
      const std::string                cjk         {"\xE5\xA4\x89\xE6\x95\xB0"};
      const std::string                astral      {"\xF0\xA0\x80\x80"};
      const std::vector<scilex::token> cjk_toks    {lex.tokenize(cjk)};
      const std::vector<scilex::token> astral_toks {lex.tokenize(astral)};
      if (cjk_toks.size() != 1 || cjk_toks[0].kind != ident) {
        return false;
      }
      if (astral_toks.size() != 1 || astral_toks[0].kind != ident) {
        return false;
      }
    }
    // The distinctive contrast: the ASCII grammar does NOT read café as one identifier (é is not ASCII,
    // so the default grammar stops at 'caf' and then cannot lex the rest).
    {
      const scilex::lexer ascii {make_lexer()};
      const std::string   cafe  {"caf\xC3\xA9"};
      try {
        const std::vector<scilex::token> toks {ascii.tokenize(cafe)};
        if (toks.size() == 1 && toks[0].kind == ident) {
          return false; // the ASCII grammar must not match café as one identifier
        }
      }
      catch (const scilex::lex_error&) {
        // expected: the ASCII grammar cannot lex the non-ASCII byte
      }
    }
    // The trade-off, as tested behaviour: the Unicode identifier is a code-point predicate, so a mode
    // holding it leaves the DFA fast path — dfa_modes_active omits the requested mode.
    {
      const scilex::lexer dfa_requested {make_rules_unicode(), {"bracket"}, {"default"}};
      for (const std::string& active : dfa_requested.dfa_modes_active()) {
        if (active == "default") {
          return false; // the code-point predicate must demote the mode
        }
      }
    }
    return true;
  }
} // namespace scilex::examples::python

#endif // SCILEX_EXAMPLE_PYTHON_HPP
