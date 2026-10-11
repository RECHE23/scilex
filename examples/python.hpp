/*!
 * \file python.hpp
 * \brief A Python-ish lexer — the modes + layout showcase (a realistic grammar).
 *
 * This is SciLex's two distinctive features in one real grammar: significant
 * indentation (\ref scilex::layout) and contextual lexing (modes). The f-string
 * is the case that needs both — `f"sum={a+b}"` interleaves three contexts where
 * the *same* byte means different things, which a flat rule list cannot separate.
 *
 * What this grammar covers — every token CPython's own tokenizer gives, checked against it over the whole
 * standard library (`make python-oracle`)
 *   - realistic code: a full keyword set, identifiers, operators (augmented, walrus, arrow, `!`, `;`),
 *     `@` decorators, and `(` `)` `[` `]` `{` `}` as brackets / dict / set;
 *   - line continuation: code wrapped across lines inside `()` `[]` `{}`, or after a `\` that ends a line,
 *     adds no INDENT/DEDENT (Layout Awareness Level A — `bracket` and `joined` are insignificant);
 *   - numbers: decimal/float/exponent, `_` between two digits, `0x`/`0o`/`0b` bases, and the `j` imaginary
 *     suffix (`obj._x` is an attribute, not a number);
 *   - strings: single/double quoted with escapes (a backslash-newline continues the string), and
 *     triple-quoted `"""…"""` / `'''…'''` (one token, spanning newlines), each with an optional `r`, `b`,
 *     `u`, `br` or `rb` prefix; a byte-order mark before the first token;
 *   - f-strings and t-strings in every form — `f"…"`, `f'…'`, triple-quoted, raw (`rf`, `fr`) — with `{…}`
 *     interpolations, a `!r` conversion and a `:spec` format spec (itself holding `{…}`), nested f-strings
 *     and nested dict/set literals inside an interpolation, tracked by the stack; `\N{name}` escapes;
 *   - INDENT / DEDENT / NEWLINE via the layout pass.
 *
 * What it does not cover: Unicode identifiers in the default (ASCII, DFA-friendly) variant — the
 * `python-unicode` one reads them as Python does (`XID_Start`, then `XID_Continue`).
 *
 * The modes and the brace-depth insight
 *   Thirteen modes: `default` (top-level code), eight f-string bodies (`fstr_` + `r` if raw + `t` if
 *   triple-quoted + `dq` / `sq`), `interp` (an f-string interpolation), `fspec` (its format spec),
 *   `bracket` (inside `()` `[]` `{}`), and `joined` (the rest of a logical line after a `\` that ends a
 *   physical one: entered and left by `set`, so a text may end in it). The decisive design point is that an
 *   interpolation and a bracket are both *code*: every code rule is active in `default`, `interp` and
 *   `bracket` (but the walrus, whose `:` opens the format spec at an interpolation's top), so they reuse the
 *   exact same rules with no duplication. `{` opens an interpolation from an
 *   f-string body (push `interp`) but a dict/set elsewhere (push `bracket`); a `}` closes whichever the stack
 *   top is. `bracket` is layout-insignificant, so code wrapped across lines in brackets is implicit line
 *   continuation:
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

#include <algorithm>
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

  //! \brief The body mode of one f-string style: `fstr_` then `r` if raw, `t` if triple-quoted, `dq` or `sq`.
  inline std::string fstring_mode(char quote,
                                  bool triple,
                                  bool raw)
  {
    return std::string {"fstr_"} + (raw ? "r" : "") + (triple ? "t" : "") + (quote == '"' ? "dq" : "sq");
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action — keeps
  //!        each rule a single, uncrustify-stable line in \ref make_rules.
  inline scilex::rule rule(int                                kind,
                           const char*                        pattern,
                           std::vector<std::string>           modes,
                           bool                               skip   = false,
                           std::optional<scilex::mode_action> action = std::nullopt,
                           real::flags                        fl     = real::flags::none)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern, fl), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = action;
    return r;
  }

  //! \brief Builds the rule list. \p unicode_identifiers swaps ONLY the identifier rule: the default
  //!        ASCII class, or Python's own identifier (`XID_Start` or `_`, then `XID_Continue`) that recognizes
  //!        `café` and `変数` — the faithful Python 3 behaviour. The Unicode form is a match-time code-point
  //!        predicate, so that rule stays on the general engine beside the mode's DFA (visible via
  //!        \ref scilex::lexer::pike_rules) — the identifier-Unicode vs DFA-speed trade-off.
  inline std::vector<scilex::rule> build_rules(bool unicode_identifiers)
  {
    using op_t = scilex::mode_action::op;
    // Code runs at the top level, inside an interpolation, and inside a bracket
    // (the layout-insignificant continuation mode — see make_lexer).
    const std::vector<std::string> code      {"default", "interp", "bracket", "joined"};
    const std::vector<std::string> multiline {"default", "interp", "bracket"};           // a line break is blank here
    std::vector<std::string>       bodies    {"fspec"};                                  // a `{` here opens an interpolation
    const std::vector<std::string> brackets  {"default", "interp", "bracket", "joined"}; // ( [ { open a bracket here

    std::vector<scilex::rule> rules;
    // --- trivia (shared by code and interpolations) --------------------------
    // ascii-pinned so `\s` is a byte class (DFA-representable): Python whitespace is ASCII, so the token
    // stream is unchanged and the default (ASCII-identifier) grammar becomes DFA-eligible.
    rules.push_back(rule(ws, R"re(\s+)re", multiline, /*skip=*/ true, std::nullopt, real::flags::ascii));
    // A byte-order mark opens a UTF-8 source file; the tokenizer reads past it.
    rules.push_back(rule(ws, "\xEF\xBB\xBF", {"default"}, true, std::nullopt, real::flags::bytes));
    // --- explicit line joining: a `\` that ends a line continues the logical line. From the top level it
    //     enters `joined` (layout-insignificant), whose line break returns to `default`; elsewhere the
    //     line is already continued and the pair is only skipped. ----------------------------------------
    rules.push_back(rule(ws, R"re(\\\r?\n)re", {"default"}, true, go(op_t::set, "joined")));
    rules.push_back(rule(ws, R"re(\\\r?\n)re", {"interp", "bracket", "joined"}, true));
    rules.push_back(rule(ws, R"re([ \t\f]+)re", {"joined"}, true));
    rules.push_back(rule(ws, R"re(\r?\n)re", {"joined"}, true, go(op_t::set, "default")));
    rules.push_back(rule(comment, R"re(#[^\n]*)re", code, /*skip=*/ true));
    // --- triple-quoted strings (dotall + lazy: up to the closing triple) ------
    // Up to the FIRST closing triple quote, greedily: each unit of the body is a character other than the
    // quote or a backslash, or a backslash and the character it escapes (an escaped quote does not close),
    // after at most two quotes. Ascii-pinned, so `[^"]` is a byte class and the mode stays DFA-eligible;
    // `[^"]` spans line breaks.
    rules.push_back(rule(tstr, R"re((?:[rRuUbB]|[bB][rR]|[rR][bB])?(?:"""(?:"{0,2}(?:[^"\\]|\\[\s\S]))*"""|'''(?:'{0,2}(?:[^'\\]|\\[\s\S]))*'''))re", code,
                         false, std::nullopt, real::flags::ascii));
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
                         unicode_identifiers ? R"re([\p{XID_Start}_][\p{XID_Continue}]*)re" : R"re([A-Za-z_][A-Za-z0-9_]*)re",
                         code));
    // --- numbers: hex / oct / bin / decimal+float+imaginary / leading-dot. An underscore only separates two
    //     digits, as Python's grammar has it: `1_000` is one number, `obj._x` an attribute. ------------------
    rules.push_back(rule(number, R"re(0[xX](?:_?[0-9a-fA-F])+)re", code));
    rules.push_back(rule(number, R"re(0[oO](?:_?[0-7])+)re", code));
    rules.push_back(rule(number, R"re(0[bB](?:_?[01])+)re", code));
    rules.push_back(rule(number, R"re([0-9](?:_?[0-9])*(?:\.(?:[0-9](?:_?[0-9])*)?)?(?:[eE][+-]?[0-9](?:_?[0-9])*)?[jJ]?)re", code));
    rules.push_back(rule(number, R"re(\.[0-9](?:_?[0-9])*(?:[eE][+-]?[0-9](?:_?[0-9])*)?[jJ]?)re", code));
    // --- plain strings (single / double quoted, with escapes) -----------------
    // A backslash escapes the next character, a line break included (the string continues on the next line).
    rules.push_back(rule(str, R"re((?:[rRuUbB]|[bB][rR]|[rR][bB])?(?:"(\\(?:.|\r?\n)|[^"\\\n])*"|'(\\(?:.|\r?\n)|[^'\\\n])*'))re", code));
    // --- f-strings, one body mode per style (quote, triple, raw); a t-string (3.14) lexes as one. The openers are
    //     code rules, so f-strings nest inside interpolations; a triple opener is the longer match, so it wins
    //     over the single. A body is text up to a `{` (an interpolation), a `}` or its closing quote:
    //     - an escape takes the next character, a line break included; in a raw body a backslash is text, but
    //       it still keeps the quote after it (`rf"\""`), and a `\` before a `{` leaves the `{` to open;
    //     - a triple-quoted body spans lines and holds one or two of its quotes, as text of their own before a
    //       brace (the closing triple is the longer match). ---------------------------------------------------
    std::vector<scilex::rule> fstrings;
    for (const bool raw : {false, true}) {
      for (const bool triple : {false, true}) {
        for (const char quote : {'"', '\''}) {
          const std::string mode   {fstring_mode(quote, triple, raw)};
          const std::string q      {quote};
          const std::string close  {triple ? q + q + q : q};
          const std::string plain  {"[^{}" + q + "\\\\" + (triple ? "" : "\\n") + "]"};
          const std::string escape {raw ? R"(\\[^{}])" : R"(\\(?:N(?:\{[^{}\r\n]*\})?|[^N\r\n]|\r?\n))"}; // `\N{name}` holds braces
          const std::string quotes {triple ? q + "{0,2}" : ""};                                           // a triple body's unit may follow one or two quotes
          const std::string text   {R"((?:\{\{|\}\}|)" + quotes + "(?:" + escape + "|" + plain + "))+" + (raw ? R"(\\?|\\)" : "")};
          const std::string prefix {raw ? "(?:[fFtT][rR]|[rR][fFtT])" : "[fFtT]"};
          rules.push_back(rule(fstr_open, (prefix + close).c_str(), code, false, go(op_t::push, mode.c_str())));
          fstrings.push_back(rule(fstr_text, text.c_str(), {mode}));
          if (triple) {
            fstrings.push_back(rule(fstr_text, (q + q + "?").c_str(), {mode}));
          }
          fstrings.push_back(rule(fstr_close, close.c_str(), {mode}, false, go(op_t::pop)));
          bodies.push_back(mode);
        }
      }
    }
    // --- operators (longest first: REAL alternation is leftmost) and @ --------
    rules.push_back(rule(op, R"re(//=|>>=|<<=|\*\*=|==|!=|<=|>=|->|//|\*\*|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|@=|[-+*/%<>=&|^~!;])re", code));
    // The walrus, but not at the top of an interpolation, where `:` opens the format spec (`f"{x:=10}"` pads x
    // to ten; the walrus needs parentheses there, which open a bracket).
    rules.push_back(rule(op, R"re(:=)re", {"default", "bracket", "joined"}));
    rules.push_back(rule(at, R"re(@)re", code));
    // --- punctuation (ellipsis before dot; a leading-dot number beats dot) -----
    rules.push_back(rule(ellipsis, R"re(\.\.\.)re", code));
    // A `:` at the top of an interpolation opens its format spec (`f"{x:>10}"`), which runs as text to the `}`
    // that closes the interpolation; first, so it wins the tie with the plain colon.
    rules.push_back(rule(colon, R"re(:)re", {"interp"}, false, go(op_t::set, "fspec")));
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
    rules.push_back(rule(interp_close, R"re(\})re", {"interp", "fspec"}, false, go(op_t::pop)));
    rules.insert(rules.end(), std::make_move_iterator(fstrings.begin()), std::make_move_iterator(fstrings.end()));
    // A format spec is text up to its interpolation's `}`, nested `{…}` interpolations aside.
    rules.push_back(rule(fstr_text, R"re([^{}\r\n]+)re", {"fspec"}));
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

  //! \brief The modes that take a DFA: all but `joined`, which holds the code rules again for the rare
  //!        continued line, and whose DFA would cost as much to build as `default`'s.
  inline const std::vector<std::string> dfa_modes {"default", "interp", "bracket", "fspec", "fstr_dq", "fstr_sq",
                                                   "fstr_tdq", "fstr_tsq", "fstr_rdq", "fstr_rsq", "fstr_rtdq",
                                                   "fstr_rtsq"};

  //! \brief The layout-insignificant modes (Layout Awareness Level A): code inside ( ) [ ] { }, after a `\` that
  //!        ends a line, or inside an f-string — its interpolations and its triple-quoted text included —
  //!        continues its line, with no NEWLINE or INDENT of its own.
  inline const std::vector<std::string> insignificant_modes {"bracket", "joined", "interp", "fspec", "fstr_tdq",
                                                             "fstr_tsq", "fstr_rtdq", "fstr_rtsq"};

  //! \brief make_lexer() under the error policy and column unit given, its modes and DFA unchanged.
  //! \param[in] errors  What to do at a byte no rule can lex (see \ref scilex::lexer).
  //! \param[in] columns The unit token columns are counted in (see \ref scilex::lexer).
  //! \return The lexer.
  inline scilex::lexer make_lexer_with(scilex::error_policy errors,
                                       scilex::column_unit  columns)
  {
    return scilex::lexer(make_rules(), insignificant_modes, dfa_modes, errors, columns, scilex::dfa_policy::requested);
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return make_lexer_with(scilex::error_policy::raise, scilex::column_unit::bytes);
  }

  //! \brief make_lexer_unicode() under the error policy and column unit given, its modes and DFA unchanged.
  //! \param[in] errors  What to do at a byte no rule can lex (see \ref scilex::lexer).
  //! \param[in] columns The unit token columns are counted in (see \ref scilex::lexer).
  //! \return The lexer.
  inline scilex::lexer make_lexer_unicode_with(scilex::error_policy errors,
                                               scilex::column_unit  columns)
  {
    return scilex::lexer(make_rules_unicode(), insignificant_modes, dfa_modes, errors, columns,
                         scilex::dfa_policy::requested);
  }

  //! \brief The Unicode-identifier variant lexer — same grammar, Unicode identifiers.
  inline scilex::lexer make_lexer_unicode()
  {
    return make_lexer_unicode_with(scilex::error_policy::raise, scilex::column_unit::bytes);
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

    // (0) Every rule of every DFA mode is on the DFA: a rule the DFA cannot reproduce (an alternation whose first
    //     match is not its longest, say) would stay on Pike and cost the mode most of its speed, with the same
    //     tokens.
    for (const std::string& mode : dfa_modes) {
      if (!lex.pike_rules(mode).empty()) {
        return false;
      }
    }
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
    // (6c) Explicit line joining: a `\` that ends a line continues the logical line, which adds no
    //      INDENT and one NEWLINE, even when the text ends inside the joined line.
    struct joined_case
    {
      std::string_view text;
      int              indents;
      int              newlines;
    };
    for (const joined_case& joined : {joined_case {"if x:\n    y = 1 + \\\n  2\n    z\n", 1, 3},
                                      joined_case {"y = 1 + \\\n        2", 0, 1}}) {
      const std::vector<scilex::token> laid {
        scilex::layout(lex.tokenize(joined.text, scilex::eof_policy::append), lex.mode_significant())};
      int indents                           {0};
      int newlines                          {0};
      for (const scilex::token& tok : laid) {
        indents  += static_cast<int>(tok.kind == scilex::indent);
        newlines += static_cast<int>(tok.kind == scilex::newline);
      }
      if (indents != joined.indents || newlines != joined.newlines) {
        return false;
      }
    }
    // (6d) String prefixes: a raw, byte, raw-byte or unicode string is one token, and rf"…" an f-string.
    {
      const std::vector<scilex::token> toks {lex.tokenize(R"py(r"\d" b'x' Rb"y" u'z' rb'''q''' rf"{a}")py")};
      const std::vector<int>           want {str, str, str, str, tstr, fstr_open, interp_open, ident, interp_close,
                                             fstr_close};
      if (toks.size() != want.size()) {
        return false;
      }
      for (std::size_t i {0}; i < want.size(); ++i) {
        if (toks[i].kind != want[i]) {
          return false;
        }
      }
    }
    // (6e) What CPython's tokenizer reads differently from a naive grammar: an underscore between digits only
    //      (`obj._x` is an attribute), `!` and `;`, a format spec after a top-level `:` (`:=` included), a raw
    //      f-string's `\` before `{`, a quote inside a triple-quoted f-string, `\N{…}`, a backslash-newline
    //      inside a string, a byte-order mark, a t-string.
    struct kinds_case
    {
      std::string_view text;
      std::vector<int> kinds;
    };
    for (const kinds_case& expected : {
      kinds_case {"self._x = 1_000 + .5j", {ident, dot, ident, op, number, op, number}},
      kinds_case {R"py(f"{x!r:>{w}}"; y)py", {fstr_open, interp_open, ident, op, ident, colon, fstr_text, interp_open, ident,
                                              interp_close, interp_close, fstr_close, op, ident}},
      kinds_case {R"py(f"{x:=10}")py", {fstr_open, interp_open, ident, colon, fstr_text, interp_close, fstr_close}},
      kinds_case {R"py(fr'\{y}')py", {fstr_open, fstr_text, interp_open, ident, interp_close, fstr_close}},
      kinds_case {"f\"\"\"a\"{b}\"\n\"\"\"", {fstr_open, fstr_text, fstr_text, interp_open, ident, interp_close, fstr_text,
                                              fstr_close}},
      kinds_case {"f'\\N{DASH}' \"a\\\nb\"", {fstr_open, fstr_text, fstr_close, str}},
      kinds_case {"\xEF\xBB\xBFx", {ident}},
      kinds_case {R"py(r"""a\"""b""")py", {tstr}}, // an escaped quote does not close a raw triple-quoted string
      kinds_case {R"py(t"{z}")py", {fstr_open, interp_open, ident, interp_close, fstr_close}},
    }) {
      const std::vector<scilex::token> toks {lex.tokenize(expected.text)};
      if (toks.size() != expected.kinds.size()) {
        return false;
      }
      for (std::size_t i {0}; i < toks.size(); ++i) {
        if (toks[i].kind != expected.kinds[i]) {
          return false;
        }
      }
    }
    // (6f) A triple-quoted f-string spanning lines continues its logical line: one NEWLINE for two lines.
    {
      const std::vector<scilex::token> laid {
        scilex::layout(lex.tokenize("x = f'''a\n{b}\n'''\ny\n", scilex::eof_policy::append), lex.mode_significant())};
      int newlines                          {0};
      for (const scilex::token& tok : laid) {
        newlines += static_cast<int>(tok.kind == scilex::newline);
      }
      if (newlines != 2) {
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
    // The trade-off, as tested behaviour: the Unicode identifier is a code-point predicate, so its rule
    // stays on Pike beside the mode's DFA — pike_rules names it.
    {
      const std::vector<scilex::rule> rules      {make_rules_unicode()};
      std::size_t                     ident_rule {rules.size()};
      for (std::size_t i {0}; i < rules.size(); ++i) {
        if (rules[i].kind == ident && ident_rule == rules.size()) {
          ident_rule = i;
        }
      }
      const scilex::lexer            hybrid  {rules, {"bracket"}};
      const std::vector<std::size_t> on_pike {hybrid.pike_rules("default")};
      if (std::ranges::find(on_pike, ident_rule) == on_pike.end()) {
        return false; // the code-point predicate must stay on Pike
      }
    }
    return true;
  }
} // namespace scilex::examples::python

#endif // SCILEX_EXAMPLE_PYTHON_HPP
