/*!
 * \file reference.hpp
 * \brief The fuzzing oracle: an independent reference tokenizer (the SciLex
 *        *spec*, brute-force) plus the property checks comparing it to the real
 *        lexer.
 *
 * The reference encodes SciLex's lexing **specification** directly, with **no
 * first-byte dispatch** — at each position it tries *every rule active in the
 * current mode*. The real \ref scilex::lexer reaches the same answer through the
 * per-mode first-byte dispatch (the most likely bug site). Comparing the two is
 * a non-circular equivalence oracle: any `reference != lexer` is a real bug, not
 * an artifact (the same idea as REAL's hints-disabled equivalence test, which
 * found real engine bugs).
 *
 * The reference reuses **exactly one** lexer internal: the pure free
 * \ref scilex::apply_transition. The mode-stack transition *is* the spec, so
 * sharing it verbatim (rather than cloning it) keeps the oracle non-circular on
 * rule selection while guaranteeing both sides agree on push/pop/set. The
 * reference re-derives its **own** mode name↔id map from the rules — the id
 * *numbers* never reach the token stream, only name-membership (fixed by the
 * rules) does — and keeps its own mode stack.
 *
 * The encoded spec (verified against `lexer.hpp` this session): at each
 * position, among the rules **active in the current mode** (empty `in_mode` ⇒
 * the default mode; otherwise the listed modes), try each anchored; the
 * **longest** match wins; on an **equal length** the **earliest** rule (lowest
 * index) wins; an **empty** match (length 0) never wins; if nothing matches,
 * throw a positioned #1 error at the byte; after a match `apply_transition` may
 * push/pop/set the mode (a pop at the root is #2); a skip rule advances the
 * cursor (and still transitions) but emits no token; line/column advance over
 * the consumed bytes (a `\n` resets the column); if the input ends inside a
 * pushed mode, throw #3 at the opening position.
 */
#ifndef SCILEX_FUZZ_REFERENCE_HPP
#define SCILEX_FUZZ_REFERENCE_HPP

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>
#include <scilex/layout.hpp>

namespace scilex::fuzz {

  //! \brief The mode name↔id mapping, re-derived independently from the rules.
  //!
  //! The id *numbers* are arbitrary (they never reach the token stream); only
  //! name-membership matters, and that is fixed by the rules. So the reference
  //! interns its own ids — "default" is 0, then every `in_mode` name and every
  //! non-pop action target, in first-seen order — and stays internally
  //! consistent: \ref names is the inverse of \ref ids.
  struct mode_map
  {
    std::vector<std::string>           names; //!< id → name.
    std::map<std::string, std::size_t> ids;   //!< name → id (fed to apply_transition).
  };

  //! \brief Re-derives the mode map from \p rules (independent of the lexer).
  inline mode_map derive_modes(const std::vector<scilex::rule>& rules)
  {
    mode_map map;
    auto     intern = [&map](const std::string& name) {
                        if (map.ids.try_emplace(name, map.names.size()).second) {
                          map.names.push_back(name);
                        }
                      };
    intern("default"); // the root mode is always id 0
    for (const scilex::rule& candidate : rules) {
      for (const std::string& name : candidate.in_mode) {
        intern(name);
      }
      if (candidate.action && candidate.action->operation != scilex::mode_action::op::pop) {
        intern(candidate.action->target);
      }
    }
    return map;
  }

  //! \brief Is \p rule active in the mode named \p mode_name?
  //!
  //! Empty `in_mode` ⇒ the rule lives in the default mode only; otherwise it is
  //! active exactly in the modes it lists. Mirrors the lexer's per-mode rule
  //! assignment, but computed independently and by name.
  inline bool rule_active_in(const scilex::rule& rule,
                             const std::string&  mode_name)
  {
    if (rule.in_mode.empty()) {
      return mode_name == "default";
    }
    for (const std::string& name : rule.in_mode) {
      if (name == mode_name) {
        return true;
      }
    }
    return false;
  }

  //! \brief Tokenizes \p source by the SciLex spec, brute-force (no dispatch).
  //!
  //! Mode-aware: keeps its own mode stack, considers only the rules active in the
  //! current mode, and drives push/pop/set through the shared
  //! \ref scilex::apply_transition (so the oracle and the lexer cannot diverge on
  //! the transition itself — only on rule selection, which is the point).
  //!
  //! \param[in]  rules        The grammar's rule list.
  //! \param[in]  source       The text to tokenize.
  //! \param[in]  emit_skipped When true, emit skip-rule matches too (used by the
  //!             coverage check); otherwise they advance the cursor silently.
  //! \param[out] modes_out    When non-null, receives the mode name each emitted
  //!             token was lexed in (1:1 with the returned tokens) — lets the
  //!             munch re-check stay mode-correct.
  //! \return The tokens in source order.
  //! \throws scilex::lex_error (positioned) on #1 (no active rule), #2 (pop at
  //!         the root, via apply_transition), or #3 (input ends inside a mode).
  inline std::vector<scilex::token> reference_tokenize(const std::vector<scilex::rule>& rules,
                                                       std::string_view                 source,
                                                       bool                             emit_skipped = false,
                                                       std::vector<std::string>*        modes_out    = nullptr)
  {
    const mode_map             modes  {derive_modes(rules)};
    std::vector<scilex::token> out;
    scilex::position           cursor {0, 1, 1};
    std::vector<scilex::frame> stack  {scilex::frame {.mode_id = 0, .entry_pos = cursor}};
    while (cursor.offset < source.size()) {
      const std::string_view rest      {source.substr(cursor.offset)};
      const std::string&     mode_name {modes.names[stack.back().mode_id]};
      std::size_t            best_len  {0};
      const scilex::rule*    best      {nullptr};
      for (const scilex::rule& candidate : rules) {
        if (!rule_active_in(candidate, mode_name)) {
          continue; // brute-force, but only over the rules active in this mode
        }
        const auto        matched {candidate.pattern.match(rest)};
        const std::size_t len     {matched ? static_cast<std::size_t>(matched.end()) : 0};
        // Longest wins; updating only on a STRICTLY longer match keeps the
        // earliest rule on a tie (the spec's order tie-break). Empty (len 0) never wins.
        if (len > 0 && len > best_len) {
          best_len = len;
          best     = &candidate;
        }
      }
      if (best == nullptr) {
        throw scilex::lex_error("no rule matches in the current mode", cursor); // #1, at the byte
      }
      const scilex::position start {cursor};
      for (std::size_t i {0}; i < best_len; ++i) {
        if (source[cursor.offset + i] == '\n') {
          ++cursor.line;
          cursor.column = 1;
        }
        else {
          ++cursor.column;
        }
      }
      cursor.offset += best_len;
      if (emit_skipped || !best->skip) {
        out.push_back(scilex::token {best->kind, source.substr(start.offset, best_len), start});
        if (modes_out != nullptr) {
          modes_out->push_back(mode_name); // the mode this token was lexed in
        }
      }
      // The transition is part of the spec: share the lexer's pure pivot verbatim
      // (a pop at the root throws #2 here, at start, exactly as the lexer does).
      scilex::apply_transition(*best, start, stack, modes.ids);
    }
    if (stack.size() > 1) {                                                 // input ran out inside a pushed mode
      throw scilex::lex_error("unterminated mode", stack.back().entry_pos); // #3, at the opening
    }
    return out;
  }

  //! \brief Two token streams are equal iff every kind, lexeme, and position match.
  inline bool tokens_equal(const std::vector<scilex::token>& a,
                           const std::vector<scilex::token>& b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i {0}; i < a.size(); ++i) {
      if (a[i].kind != b[i].kind || a[i].lexeme != b[i].lexeme
          || a[i].start.offset != b[i].start.offset || a[i].start.line != b[i].start.line
          || a[i].start.column != b[i].start.column) {
        return false;
      }
    }
    return true;
  }

  //! \brief Outcome of an oracle run: ok, or the first invariant that failed.
  struct result
  {
    bool        ok        {true};
    const char* invariant {nullptr}; //!< Which invariant failed (nullptr when ok).
  };

  //! \brief Runs every applicable property invariant for one (grammar, input).
  //!
  //! Invariants (see the fiche): 3 total coverage, 4 maximal munch (independent),
  //! 5 lazy == eager, 6 layout balance (when \p has_layout), 7 positioned error,
  //! 8 determinism. (1 no-crash / 2 termination are the harness's job — sanitizers
  //! and a bound.) The reference-vs-lexer equivalence is the spine of 3-4.
  //!
  //! \param[in] rules      The grammar's rule list (for the independent reference).
  //! \param[in] lex        The real lexer (same grammar) — the system under test.
  //! \param[in] input      The text to tokenize.
  //! \param[in] has_layout Whether to run the indentation-layout balance check.
  inline result check(const std::vector<scilex::rule>& rules,
                      const scilex::lexer&             lex,
                      std::string_view                 input,
                      bool                             has_layout)
  {
    // --- reference vs lexer: they must agree (the dispatch / munch gate) ---
    bool                       reference_threw {false};
    std::size_t                reference_pos   {0};
    std::vector<scilex::token> reference;
    std::vector<std::string>   reference_modes; // the mode each reference token was lexed in
    try {
      reference = reference_tokenize(rules, input, false, &reference_modes);
    }
    catch (const scilex::lex_error& error) {
      reference_threw = true;
      reference_pos   = error.where().offset;
    }

    bool                       lexer_threw {false};
    std::size_t                lexer_pos   {0};
    std::vector<scilex::token> eager;
    try {
      eager = lex.tokenize(input);
    }
    catch (const scilex::lex_error& error) {
      lexer_threw = true;
      lexer_pos   = error.where().offset;
    }

    if (reference_threw != lexer_threw) {
      return {false, "equivalence: reference and lexer disagree on whether the input lexes"};
    }
    if (lexer_threw) {
      if (reference_pos != lexer_pos) {
        return {false, "error position: reference and lexer disagree"};
      }
      if (lexer_pos >= input.size()) { // #7 positioned error within bounds
        return {false, "error position out of bounds"};
      }
      return {true, nullptr};              // both refuse the same in-bounds position — agreement
    }
    if (!tokens_equal(reference, eager)) { // #4 munch + the dispatch decision
      return {false, "maximal munch: reference != lexer (dispatch divergence)"};
    }

    // --- #5 lazy scan == eager tokenize (the shared scan_next, both paths) ---
    std::vector<scilex::token> lazy;
    for (const scilex::token& tok : lex.scan(input)) {
      lazy.push_back(tok);
    }
    if (!tokens_equal(lazy, eager)) {
      return {false, "lazy scan != eager tokenize"};
    }

    // --- #8 determinism ---
    if (!tokens_equal(lex.tokenize(input), eager)) {
      return {false, "tokenize is not deterministic"};
    }

    // --- #3 total coverage: every byte consumed exactly once, contiguously ---
    const std::vector<scilex::token> all     {reference_tokenize(rules, input, true)};
    std::size_t                      covered {0};
    for (const scilex::token& tok : all) {
      if (tok.start.offset != covered) {
        return {false, "coverage: a gap or overlap between consumed spans"};
      }
      covered += tok.lexeme.size();
    }
    if (covered != input.size()) {
      return {false, "coverage: the input was not fully consumed"};
    }

    // --- #4 independent per-token munch re-check (mode-aware) ---
    // reference == eager (checked above), so reference_modes aligns 1:1 with eager.
    for (std::size_t i {0}; i < eager.size(); ++i) {
      const scilex::token&   tok       {eager[i]};
      const std::string&     mode_name {reference_modes[i]};
      const std::string_view at        {input.substr(tok.start.offset)};
      std::size_t            longest   {0};
      bool                   kind_fits {false};
      for (const scilex::rule& candidate : rules) {
        if (!rule_active_in(candidate, mode_name)) {
          continue; // only the rules the lexer could have used in this mode
        }
        const auto        matched {candidate.pattern.match(at)};
        const std::size_t len     {matched ? static_cast<std::size_t>(matched.end()) : 0};
        if (len > longest) {
          longest = len;
        }
        if (candidate.kind == tok.kind && len == tok.lexeme.size() && len > 0) {
          kind_fits = true; // (a) a rule of this kind matches this lexeme anchored
        }
      }
      if (!kind_fits) {
        return {false, "munch: the chosen kind's rule does not match the lexeme anchored"};
      }
      if (longest != tok.lexeme.size()) { // (b) no active rule matches a longer prefix here
        return {false, "munch: a rule matches a longer prefix than the chosen token"};
      }
    }

    // --- #6 layout balance (indentation languages only) ---
    if (has_layout) {
      const std::vector<scilex::token> flat {lex.tokenize(input, scilex::eof_policy::append)};
      std::vector<scilex::token>       laid;
      try {
        // Policy-aware: the lexer's significance policy, whose ids match its tokens.
        laid = scilex::layout(flat, lex.mode_significant());
      }
      catch (const scilex::layout_error&) {
        return {true, nullptr}; // inconsistent indentation → a positioned error is correct
      }
      long depth {0};
      for (const scilex::token& tok : laid) {
        if (tok.kind == scilex::indent) {
          ++depth;
        }
        else if (tok.kind == scilex::dedent) {
          --depth;
        }
        if (depth < 0) {
          return {false, "layout: a DEDENT drove the depth below zero"};
        }
      }
      if (depth != 0) {
        return {false, "layout: unbalanced INDENT/DEDENT (depth ends non-zero)"};
      }
    }
    return {true, nullptr};
  }
} // namespace scilex::fuzz

#endif // SCILEX_FUZZ_REFERENCE_HPP
