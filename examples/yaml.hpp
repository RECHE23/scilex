/*!
 * \file yaml.hpp
 * \brief A YAML-ish lexer — modes (block/flow) meeting the layout pass.
 *
 * The third modal profile, distinct from Python (deep nesting + rule sharing) and
 * XML (many shallow modes): YAML combines significant block indentation with flow
 * collections. `block` is the root mode; `flow` is the single pushed mode, entered
 * by `{` or `[` (from block OR a flow, so flow collections nest) and left by `}` /
 * `]`. Block indentation is recovered by the layout pass from token positions.
 *
 * Modes & Layout: the decoupling, and Level A
 *   layout() is positional; by default it is mode-blind. Layout Awareness Level A
 *   makes it opt-in mode-aware (each token carries its mode; a per-mode policy
 *   marks a mode significant|insignificant), and THIS grammar uses it: `flow` is
 *   insignificant, so a multi-line flow collection adds no INDENT/DEDENT — without
 *   touching the scan. Block scalars `|` / `>` are deeper (a reference indent in
 *   the frame) — Layout Awareness Level B, still to come. Two invariants hold: (1)
 *   no policy ⇒ byte-for-byte the positional pass, zero-cost; (2) the MODE is the
 *   single source of truth for the policy — no per-rule flag. See
 *   include/scilex/layout.hpp.
 *
 * What this grammar covers
 *   - mappings `key: value`, block sequences `- item`;
 *   - flow collections `{…}` / `[…]`, single- or multi-line (nesting via the
 *     stack; multi-line adds no spurious layout, via Level A);
 *   - comments `#…`, anchors `&a` / aliases `*a` / tags `!t` (simple);
 *   - strings `"…"` (with `\`-escapes) and `'…'` (with `''` escapes);
 *   - plain scalars (pragmatic — see below) and block indentation via layout().
 *
 * Not yet handled (Layout Awareness Level B):
 *   - block scalars `|` / `>` (they need a reference indent in the frame).
 *
 * YAML complexity beyond this sample (realism, not the model — none excluded):
 *   - fine scalar context-sensitivity (a `:` inside a value, multi-word scalars,
 *     number/bool/null typing — all lexed as plain scalars here);
 *   - multi-document `---` / `...`, merge keys `<<`, complex keys `?`.
 *
 * REAL notes (reusable): lookahead `(?=…)` is not supported by REAL, so the block
 * dash cannot be written `-(?=\s|$)`. Instead `-` is a 1-char rule listed before
 * the scalar, and the plain scalar may start with `-`: `-5` wins as a scalar by
 * maximal munch while a bare `-` (sequence marker) stays a dash — the documented
 * approximation. Likewise `:` is one token; telling a mapping `:` from a `:` in a
 * scalar (a URL, a time) is left to the parser (out of this sample).
 */
#ifndef SCILEX_EXAMPLE_YAML_HPP
#define SCILEX_EXAMPLE_YAML_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <scilex/scilex.hpp>
#include <scilex/layout.hpp>

namespace scilex::examples::yaml {

  //! \brief YAML-ish token kinds. Whitespace and comments are skip rules.
  enum kind
  {
    ws,
    comment,
    anchor,     //!< `&name`
    alias,      //!< `*name`
    tag,        //!< `!name`
    dq,         //!< double-quoted string
    sq,         //!< single-quoted string (with `''` escapes)
    scalar,     //!< a plain scalar (block or flow context)
    colon,      //!< `:`
    dash,       //!< `-` (a block sequence marker)
    comma,      //!< `,` (a flow separator)
    flow_open,  //!< `{` / `[` — pushes `flow`
    flow_close, //!< `}` / `]` — pops `flow`
  };

  //! \brief A printable name for each kind, including the layout / EOF tokens.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case anchor:     return "ANCHOR";
      case alias:      return "ALIAS";
      case tag:        return "TAG";
      case dq:         return "DQSTRING";
      case sq:         return "SQSTRING";
      case scalar:     return "SCALAR";
      case colon:      return ":";
      case dash:       return "-";
      case comma:      return ",";
      case flow_open:  return "FLOW_OPEN";
      case flow_close: return "FLOW_CLOSE";
      case scilex::newline:      return "NEWLINE";
      case scilex::indent:       return "INDENT";
      case scilex::dedent:       return "DEDENT";
      case scilex::end_of_input: return "EOF";
      default:                   return "?";
    }
  }

  //! \brief A push/pop action targeting \p target (target ignored for pop).
  inline scilex::mode_action go(scilex::mode_action::op operation,
                                const char*             target = "")
  {
    return {.operation = operation, .target = target};
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action.
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

  //! \brief Builds the YAML lexer (block is the root mode; flow is pushed).
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;
    const std::vector<std::string> root {};                  // block = the default (root) mode
    const std::vector<std::string> both {"default", "flow"}; // active in block AND flow
    const std::vector<std::string> flow {"flow"};

    std::vector<scilex::rule> rules;
    // --- shared by block and flow --------------------------------------------
    // Whitespace (newlines included — layout() recovers structure from positions).
    rules.push_back(rule(ws, R"re(\s+)re", both, /*skip=*/ true));
    rules.push_back(rule(flow_open, R"re([{[])re", both, false, go(op_t::push, "flow"))); // nests
    rules.push_back(rule(dq, R"re("(\\.|[^"\\])*")re", both));
    rules.push_back(rule(sq, R"re('([^']|'')*')re", both));
    rules.push_back(rule(colon, R"re(:)re", both));
    // --- block only (the root mode) ------------------------------------------
    rules.push_back(rule(comment, R"re(#.*)re", root, /*skip=*/ true));
    rules.push_back(rule(anchor, R"re(&[A-Za-z0-9_-]+)re", root));
    rules.push_back(rule(alias, R"re(\*[A-Za-z0-9_-]+)re", root));
    rules.push_back(rule(tag, R"re(![A-Za-z0-9_/-]*)re", root));
    rules.push_back(rule(dash, R"re(-)re", root));                            // before the scalar (see the dash note)
    rules.push_back(rule(scalar, R"re([^\s#&*!"'{\[:][^\s#:{\[]*)re", root)); // block plain scalar (approx)
    // --- flow only -----------------------------------------------------------
    rules.push_back(rule(flow_close, R"re([}\]])re", flow, false, go(op_t::pop)));
    rules.push_back(rule(comma, R"re(,)re", flow));
    rules.push_back(rule(scalar, R"re([^\s#&*!"'{}\[\]:,][^\s#:{}\[\],]*)re", flow)); // flow plain scalar (approx)
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    // "flow" is layout-insignificant (Layout Awareness Level A): a multi-line flow
    // collection adds no INDENT/DEDENT. Block indentation stays significant.
    return scilex::lexer(make_rules(), {"flow"});
  }

  //! \brief A small configuration document exercising every covered context.
  inline constexpr std::string_view sample {
    R"yaml(# application config
name: SciLex
version: 2026.6
debug: false
tags: [lexer, modes, yaml]
limits: {max: 100, min: 0}
anchored: &base value
alias_ref: *base
kind: !color red
quoted: "a \"q\" b"
single: 'it''s fine'
items:
  - first
  - second
nested:
  key: deep
)yaml"};

  //! \brief True iff \p toks has exactly the kinds \p want, in order.
  inline bool kinds_are(const std::vector<scilex::token>& toks,
                        const std::vector<int>&           want)
  {
    if (toks.size() != want.size()) {
      return false;
    }
    for (std::size_t i {0}; i < want.size(); ++i) {
      if (toks[i].kind != want[i]) {
        return false;
      }
    }
    return true;
  }

  //! \brief (indents, dedents) the layout pass emits for \p src.
  inline std::pair<int, int> indent_dedent(const scilex::lexer& lex,
                                           std::string_view     src)
  {
    int indents {0};
    int dedents {0};
    for (const scilex::token& tok : scilex::layout(lex.tokenize(src, scilex::eof_policy::append),
                                                   lex.mode_significant())) {
      indents += static_cast<int>(tok.kind == scilex::indent);
      dedents += static_cast<int>(tok.kind == scilex::dedent);
    }
    return {indents, dedents};
  }

  //! \brief Self-check of CAPABILITIES (so `make example` gates). The limit of the
  //!        decoupled model — spurious indent in multi-line flow — is a separate
  //!        characterization test (tests/test_modes.cpp), not asserted here.
  inline bool self_check()
  {
    const scilex::lexer lex {make_lexer()};

    // (1) a mapping entry.
    if (!kinds_are(lex.tokenize("key: value"), {scalar, colon, scalar})) {
      return false;
    }
    // (2) a block sequence lexes, and its layout is balanced.
    if (!kinds_are(lex.tokenize("- a\n- b\n"), {dash, scalar, dash, scalar})) {
      return false;
    }
    if (const auto [ind, ded] = indent_dedent(lex, "- a\n- b\n"); ind != ded) {
      return false;
    }
    // (3) single-line flow collections lex, with ZERO layout structure inside.
    if (!kinds_are(lex.tokenize("[1, 2, 3]"),
                   {flow_open, scalar, comma, scalar, comma, scalar, flow_close})) {
      return false;
    }
    if (!kinds_are(lex.tokenize("{a: 1, b: 2}"),
                   {flow_open, scalar, colon, scalar, comma, scalar, colon, scalar, flow_close})) {
      return false;
    }
    if (indent_dedent(lex, "[1, 2, 3]") != std::pair<int, int> {0, 0}) {
      return false; // single line -> no indent/dedent
    }
    // A MULTI-line flow collection also adds no layout structure (Layout Awareness
    // Level A: "flow" is insignificant), where the positional pass would not.
    if (indent_dedent(lex, "[\n  1,\n  2\n]") != std::pair<int, int> {0, 0}) {
      return false;
    }
    // (4) a comment is skipped.
    if (!kinds_are(lex.tokenize("v # tail\n"), {scalar})) {
      return false;
    }
    // (5) an anchor and an alias.
    if (!kinds_are(lex.tokenize("&a x"), {anchor, scalar})
        || !kinds_are(lex.tokenize("*a"), {alias})) {
      return false;
    }
    // (6) a single-quoted string with a '' escape is one token.
    {
      const std::vector<scilex::token> toks {lex.tokenize("'it''s'")};
      if (toks.size() != 1 || toks[0].kind != sq) {
        return false;
      }
    }
    // (7) the realistic sample lexes; its block layout is balanced with INDENTs.
    if (const auto [ind, ded] = indent_dedent(lex, sample); ind == 0 || ind != ded) {
      return false;
    }
    return true;
  }
} // namespace scilex::examples::yaml

#endif // SCILEX_EXAMPLE_YAML_HPP
