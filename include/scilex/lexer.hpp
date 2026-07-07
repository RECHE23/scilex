/*!
 * \file lexer.hpp
 * \brief The lexer: maximal-munch tokenization over a set of REAL patterns.
 *
 * SciLex is a thin layer over REAL. Each \ref scilex::rule pairs a token kind
 * with a `real::regex`; the lexer scans the source left to right, and at each
 * position picks the rule with the **longest** anchored match (maximal munch),
 * breaking ties by rule order (earlier rules have priority). Because REAL is a
 * linear-time engine, tokenization is linear and ReDoS-safe by construction —
 * no token rule can make the scanner backtrack catastrophically.
 *
 * Two ways to consume tokens: \ref scilex::lexer::tokenize materializes them
 * all into a vector, while \ref scilex::lexer::scan returns a lazy range that
 * produces one token at a time (the parser-friendly access pattern — no token
 * vector is allocated).
 */
#ifndef SCILEX_LEXER_HPP
#define SCILEX_LEXER_HPP

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <real/dfa.hpp>
#include <real/real.hpp>

#include "token.hpp"

namespace scilex {

  class token_iterator;
  class token_range;

  /*!
   * \brief Whether tokenization appends a synthetic end-of-input token.
   *
   * \ref eof_policy::append yields one final token of kind \ref end_of_input at the end
   * position once the input is exhausted — the parser-friendly mode, so a
   * cursor always has a current token to match against.
   */
  enum class eof_policy
  {
    omit,   //!< Stop at the last real token (default).
    append, //!< Append one \ref end_of_input token at the end position.
  };

  /*!
   * \brief A mode transition, fired when its rule wins, acting on the scan's mode
   *        stack: enter a nested mode, leave the current one, or replace it.
   */
  struct mode_action
  {
    //! \brief The kind of transition.
    enum class op
    {
      push, //!< Enter \ref target, remembering the mode below it (a nested context).
      pop,  //!< Leave the current mode, returning to the one beneath it.
      set,  //!< Replace the current mode with \ref target (stack depth unchanged).
    };

    op          operation;   //!< Which transition to perform.
    std::string target {};   //!< The mode push/set enters; ignored (and omittable) for pop.

    //! \brief The interned id of \ref target, resolved once when the lexer is built
    //!        (see \ref scilex::lexer::build_dispatch) so the per-token transition is
    //!        a field read, not a name→id map lookup. Internal cache: a caller leaves
    //!        it at 0 and sets only \ref target; pop leaves it unused.
    std::size_t target_id {0};
  };

  /*!
   * \brief A token rule: a kind, the pattern that recognizes it, whether matches
   *        are discarded (whitespace, comments), and — for contextual lexing — the
   *        modes it is active in and an optional mode transition it fires when it wins.
   *
   * \ref in_mode empty means the rule is active in the implicit "default" mode only,
   * so a plain `{kind, pattern, skip}` rule keeps working unchanged.
   *
   * The pattern is a fully-formed `real::regex`, so the grammar author owns its flags.
   *
   * ## Unicode identifiers vs DFA speed — the grammar author's choice
   *
   * This is a real trade-off, not a footnote. `\w+` (or `[^\W\d]\w*`) with the default flags reads
   * **Unicode identifiers** — `café`, `変数` — the faithful behaviour for a language like Python 3.
   * But a Unicode `\w \d \s \b` compiles to a match-time **code-point predicate**, which no DFA can
   * represent, so a mode that requests DFA acceleration (`dfa_modes`) and contains one is **transparently
   * demoted** to the general Pike engine (same tokens; the demotion is visible via
   * `lexer::dfa_modes_active`). Concretely: the general engine lexes at roughly **6–9.5 MB/s**, while a
   * DFA-accelerated mode runs **3–27× that** — so the Unicode identifier costs the DFA fast path.
   *
   * If your identifiers are ASCII by specification (JSON, SQL, C), pin `(?a)` inline in the pattern
   * (or pass `real::flags::ascii`) to keep `\w \d \s \b` ASCII and small, DFA-representable, and fast —
   * this is what the `examples/` grammars do. If you want Unicode identifiers, write `\w+` and accept
   * the general-engine floor. The two spellings tokenize the same ASCII input identically; they differ
   * only on non-ASCII input and on whether the mode can be a DFA.
   */
  struct rule
  {
    int                        kind;            //!< Kind assigned to tokens this rule produces.
    real::regex                pattern;         //!< The recognizer (a linear-time REAL regex; its flags are the author's — see above).
    bool                       skip    {false}; //!< If true, matches are consumed but not emitted.
    std::vector<std::string>   in_mode {};      //!< Modes this rule is active in; empty ⇒ {"default"}.
    std::optional<mode_action> action  {};      //!< Mode transition fired when this rule wins.
  };

  /*!
   * \brief Thrown when no rule matches at a position (a lexical error).
   *
   * Carries the \ref position of the offending byte so a caller can report it.
   */
  class lex_error : public std::runtime_error
  {
  public:

    /*!
     * \brief Builds the error.
     * \param[in] message Human-readable cause.
     * \param[in] where   Position of the byte that no rule could match.
     */
    lex_error(const std::string& message,
              position           where)
      : std::runtime_error(message),
        where_(where)
    {}

    /*!
     * \brief Returns the position of the unmatched byte.
     */
    [[nodiscard]] position where() const noexcept
    {
      return where_;
    }

  private:

    position where_; //!< Where tokenization failed.
  };

  /*!
   * \brief One entry on the per-scan mode stack: the active mode and where it was
   *        entered (the entry position feeds the unterminated/diagnostic messages).
   */
  struct frame
  {
    std::size_t mode_id;   //!< Id of the active mode.
    position    entry_pos; //!< Where this mode was entered.
  };

  /*!
   * \brief Applies rule \p r's mode transition (if any) to \p stack — the per-scan
   *        mode-stack mutation, kept pure so the lexer and the fuzz oracle share it
   *        verbatim.
   *
   * Depends only on \p r's action (its pre-resolved \ref mode_action::target_id), the
   * token start \p start, and the \p stack; it mutates only \p stack. push enters the
   * target (remembering \p start), pop leaves the current mode, set replaces it in
   * place. The target id is resolved once at build time, so this hot per-token pivot
   * does no name→id map lookup.
   *
   * \throws lex_error On a pop while the stack is at its root (nothing to leave).
   */
  inline void apply_transition(const rule&         r,
                               position            start,
                               std::vector<frame>& stack)
  {
    if (!r.action) {
      return;
    }
    if (r.action->operation == mode_action::op::push) {
      stack.push_back(frame {.mode_id = r.action->target_id, .entry_pos = start});
    }
    else if (r.action->operation == mode_action::op::pop) {
      if (stack.size() == 1) {
        throw lex_error("cannot pop the mode stack: already at the root mode", start);
      }
      stack.pop_back();
    }
    else { // set: replace the active mode in place (depth unchanged)
      stack.back().mode_id = r.action->target_id;
    }
  }

  /*!
   * \brief What a lexer does when it reaches a byte that no rule in the active mode can begin.
   *
   * The default preserves the historical behaviour exactly; \ref token is opt-in recovery.
   */
  enum class error_policy
  {
    raise, //!< Throw a \ref lex_error at the first unmatched byte (the default).
    //! Recover: emit the maximal unmatched byte run as one \ref scilex::error token and resume. The
    //! cost of an error run is the grammar's no-match cost: a first-byte pre-filter skips positions no
    //! rule can begin (usually O(1) per byte), so an unanchored, greedy rule that scans far before
    //! failing is what makes recovery expensive on a long run — prefer a definite leading byte.
    token,
  };

  /*!
   * \brief The unit a token's \c position::column is counted in.
   *
   * The default \c bytes is the historical behaviour, bit-for-bit. \c codepoints counts Unicode
   * scalar values (each valid UTF-8 codepoint is one column), and \c utf16 counts UTF-16 code units
   * (a BMP codepoint is 1, an astral codepoint 2) — the unit an LSP client expects. A malformed byte
   * (an orphan continuation, an overlong or out-of-range sequence) counts as one unit in every mode, so
   * the column stays defined on the error runs \ref error_policy::token emits. The chosen unit is not
   * carried on \ref position (one field, not self-describing) — the lexer declares it via
   * \ref lexer::columns, a named trade-off rather than a silent default.
   */
  enum class column_unit
  {
    bytes,      //!< One column per byte (the default; column == byte offset within the line + 1).
    codepoints, //!< One column per Unicode scalar value (a valid UTF-8 codepoint).
    utf16,      //!< One column per UTF-16 code unit (BMP = 1, astral = 2) — the LSP unit.
  };

  /*!
   * \brief A lexer built from an ordered list of rules.
   *
   * Order matters only as a tie-breaker between rules whose matches have equal
   * length (the first such rule wins). Put more specific rules (keywords)
   * before their general counterparts (identifiers).
   */
  class lexer
  {
  public:

    /*!
     * \brief Builds a lexer from \p rules (taken by value, then moved in).
     * \param[in] rules The ordered token rules.
     * \param[in] insignificant_modes Modes whose tokens carry no layout structure
     *            (Layout Awareness Level A — see \ref scilex::layout). Each name
     *            must be a mode the rules use; empty (the default) leaves every mode
     *            significant, so \ref mode_significant has no effect.
     * \param[in] dfa_modes Modes to accelerate with a \c real::dfa fast path (one
     *            DFA pass replaces the per-rule Pike dispatch). Each name must be a
     *            mode the rules use. Opt-in is best-effort: a mode whose rules cannot
     *            be a DFA (a zero-width assertion) or whose DFA fails the build-time
     *            audit (a lazy quantifier) silently stays on Pike — see
     *            \ref dfa_modes_active. The token stream is identical either way.
     * \param[in] errors What to do at a byte no rule can lex: \ref error_policy::raise (the default —
     *            throw) or \ref error_policy::token (recover, emitting an \ref scilex::error token). The
     *            recovery path never throws per byte; the token stream under \c raise is unchanged.
     * \param[in] columns The unit each token's \c position::column is counted in:
     *            \ref column_unit::bytes (the default, unchanged), \ref column_unit::codepoints, or
     *            \ref column_unit::utf16. The unit is not stored on the position — read it back with
     *            \ref columns().
     * \throws std::invalid_argument If a transition rule is malformed (empty
     *         pattern or target), or \p insignificant_modes / \p dfa_modes names an
     *         unknown mode.
     */
    explicit lexer(std::vector<rule>               rules,
                   std::vector<std::string>        insignificant_modes        = {},
                   std::vector<std::string>        dfa_modes                  = {},
                   error_policy                    errors                     = error_policy::raise,
                   column_unit                     columns                    = column_unit::bytes)
      : rules_(std::move(rules)),
        errors_(errors),
        columns_(columns)
    {
      build_dispatch();
      build_significance(insignificant_modes);
      build_dfa_modes(dfa_modes);
    }

    //! \brief The unit this lexer counts \c position::column in (positions do not carry it, so a
    //!        consumer that needs to interpret a column reads the unit here).
    [[nodiscard]] column_unit columns() const noexcept
    {
      return columns_;
    }

    /*!
     * \brief Tokenizes \p source into the sequence of non-skipped tokens.
     *
     * At each position every rule is matched anchored; the longest match wins
     * (ties broken by rule order). A zero-length winning match (a nullable rule
     * with no longer match here) cannot advance the scan, so it is reported as a
     * \ref lex_error rather than allowed to stall.
     *
     * \param[in] source The text to tokenize (must outlive the returned tokens;
     *            each token's lexeme views into it).
     * \param[in] policy Whether to append a terminal \ref end_of_input token.
     * \return The tokens in source order, skip-rule matches omitted.
     * \throws lex_error If some position is matched by no rule, or only by a
     *         zero-length match.
     */
    [[nodiscard]] std::vector<token> tokenize(std::string_view source,
                                              eof_policy       policy = eof_policy::omit) const
    {
      std::vector<token> out;
      position           cursor {0, 1, 1};
      std::vector<frame> stack {frame {.mode_id = 0, .entry_pos = cursor}}; // start in "default"
      token              next   {};
      while (scan_next(source, cursor, stack, next)) {
        out.push_back(next);
      }
      if (policy == eof_policy::append) {
        // The cursor now sits at the end position (past any trailing trivia).
        out.push_back(token {end_of_input, source.substr(cursor.offset), cursor});
      }
      return out;
    }

    /*!
     * \brief Returns a lazy range over the non-skipped tokens of \p source.
     *
     * Each `++` produces the next token on demand; nothing but the current
     * token is held. Usable in a range-for. Errors surface as \ref lex_error
     * thrown while advancing.
     *
     * \param[in] source The text to scan (must outlive the iteration; each
     *            token's lexeme views into it).
     * \param[in] policy Whether to yield a terminal \ref end_of_input token.
     * \return A \ref token_range whose iterators yield \ref token values.
     * \throws lex_error (while iterating) if some position matches no rule.
     */
    [[nodiscard]] token_range scan(std::string_view source,
                                   eof_policy       policy = eof_policy::omit) const&;

    //! \brief Deleted: the range would point into a temporary lexer.
    token_range scan(std::string_view source,
                     eof_policy       policy = eof_policy::omit) const&& = delete;

    //! \brief The per-mode-id layout-significance policy (see \ref scilex::layout).
    //!        Index by a token's `mode_id`; `false` marks an insignificant mode.
    //!        Empty unless the lexer was built with `insignificant_modes`.
    [[nodiscard]] const std::vector<bool>& mode_significant() const noexcept
    {
      return mode_significant_;
    }

    //! \brief The name of mode \p id (0 is "default"), for labelling tokens.
    [[nodiscard]] const std::string& mode_name(std::size_t id) const noexcept
    {
      return mode_names_[id];
    }

    //! \brief The modes actually accelerated by a DFA fast path.
    //!
    //! A mode passed in `dfa_modes` but rejected — its rules need an assertion no DFA
    //! can represent (\c real::dfa_error), or its DFA failed the build-time audit (a
    //! lazy quantifier) — is **absent** here: it fell back to the Pike path, lexing the
    //! same tokens. So `dfa_modes` is an optimizer, not a guarantee; the rejected set is
    //! `dfa_modes − dfa_modes_active()`.
    [[nodiscard]] std::vector<std::string> dfa_modes_active() const
    {
      std::vector<std::string> active;
      for (std::size_t m {0}; m < per_mode_dfa_.size(); ++m) {
        if (per_mode_dfa_[m]) {
          active.push_back(mode_names_[m]);
        }
      }
      return active;
    }

  private:

    friend class token_iterator;

    //! \brief Formats a position as "line:column" for diagnostics.
    static std::string position_label(position where)
    {
      return std::to_string(where.line) + ":" + std::to_string(where.column);
    }

    /*!
     * \brief Advances \p cursor to and past the next non-skipped token in the active
     *        mode, applying the winning rule's mode transition (if any).
     *
     * Skips over skip-rule matches, then fills \p out with the next emitted token,
     * advancing \p cursor (offset, line, byte column) and the mode \p stack past it.
     * The single source of scanning truth shared by \ref tokenize and the lazy
     * \ref token_iterator.
     *
     * \param[in]     source The text being scanned.
     * \param[in,out] cursor Current position; advanced past the consumed bytes.
     * \param[in,out] stack  The mode stack (its top is the active mode); a winning
     *                rule's transition mutates it. Never empty.
     * \param[out]    out    Receives the next non-skipped token on success.
     * \return `true` if a token was produced, `false` at end of input.
     * \throws lex_error If a position matches no rule in the active mode (#1), a rule
     *         pops at the stack root (#2), input ends inside a pushed mode (#3), or the
     *         winning match is zero-length and so cannot advance the scan (#4).
     */
    bool scan_next(std::string_view    source,
                   position&           cursor,
                   std::vector<frame>& stack,
                   token&              out) const
    {
      while (cursor.offset < source.size()) {
        const std::string_view rest {source.substr(cursor.offset)};
        const std::size_t      mode {stack.back().mode_id};
        const munch_result     m    {munch_at(mode, rest, static_cast<unsigned char>(source[cursor.offset]))};

        if (!m.have) {
          if (errors_ == error_policy::raise) {
            throw lex_error("no rule matches in mode '" + mode_names_[mode] + "' (entered at "
                            + position_label(stack.back().entry_pos) + ")", cursor); // #1
          }
          // Recovery (error_policy::token): accumulate the maximal run of bytes that no rule in this
          // mode can begin into ONE reserved-kind error token, then resume — no throw, no transition
          // (the run stays in its mode). The run ends at the first position where a rule matches (>0);
          // may_start is an O(1) first-byte pre-filter that skips the bulk of the noise without a full
          // match attempt. The lexeme is the exact offending bytes.
          const position err_start {cursor};
          advance(source, cursor, 1); // the byte at err_start is unmatched by definition
          while (cursor.offset < source.size()
                 && !starts_a_match(mode, source, cursor.offset)) {
            advance(source, cursor, 1);
          }
          out = token {scilex::error, source.substr(err_start.offset, cursor.offset - err_start.offset),
                       err_start, mode};
          return true;
        }
        if (m.len == 0) {
          // A rule won with a zero-length match (a nullable rule and no longer match at this
          // position). Advancing by 0 would spin forever, so report it as a lexical error — fatal
          // under either policy (recovery cannot make progress here). The shared advance point, so
          // both the Pike and DFA paths are covered.
          throw lex_error("zero-length match in mode '" + mode_names_[mode]
                          + "' (rule never advances)", cursor); // #4
        }

        const std::size_t best_idx {m.idx};
        const std::size_t best_len {m.len};
        const position    start    {cursor};
        advance(source, cursor, best_len);

        apply_transition(rules_[best_idx], start, stack); // advances, then transitions (#2 on a bad pop)

        if (!rules_[best_idx].skip) {
          // Tag the token with the mode it was lexed in (captured before the
          // transition above) — Layout Awareness reads it; the scan is untouched.
          out = token {rules_[best_idx].kind, source.substr(start.offset, best_len), start, mode};
          return true;
        }
        // Skip rule: keep scanning for the next emitted token (possibly in a new mode).
      }
      if (stack.size() > 1) {
        if (errors_ == error_policy::raise) {
          throw lex_error("unterminated mode '" + mode_names_[stack.back().mode_id] + "' (entered at "
                          + position_label(stack.back().entry_pos) + ")", stack.back().entry_pos); // #3
        }
        // Recovery (error_policy::token): a mode was still pushed at end of input. Emit one zero-width
        // error token positioned at the EOF (the partial tokens already emitted stay), then unwind to
        // the root so the next call reports a clean end of input.
        out = token {scilex::error, source.substr(cursor.offset, 0), cursor, stack.back().mode_id};
        stack.resize(1);
        return true;
      }
      return false;
    }

    //! \brief Interns a mode name to its id, assigning the next id on first sight.
    std::size_t intern_mode(const std::string& name)
    {
      const auto [it, inserted] {mode_id_.emplace(name, mode_names_.size())};
      if (inserted) {
        mode_names_.push_back(name);
      }
      return it->second;
    }

    //! \brief Adds rule \p idx to mode \p m's dispatch via REAL's exact first-byte
    //!        API — the same 3-way split (nullable → general; one fixed byte → its
    //!        bucket; otherwise → every bucket the set admits) as the mono-mode build.
    void add_to_mode(std::size_t m,
                     std::size_t idx)
    {
      const real::regex& pattern {rules_[idx].pattern};
      dispatch&          target  {per_mode_[m]};
      if (!pattern.has_first_byte_set()) {
        target.general.push_back(idx);
      }
      else if (const std::optional<unsigned char> byte {pattern.unique_first_byte()}) {
        target.first_byte_index[*byte].push_back(idx);
      }
      else {
        for (int candidate {0}; candidate < 256; ++candidate) {
          if (pattern.may_start_with(static_cast<unsigned char>(candidate))) {
            target.first_byte_index[static_cast<unsigned char>(candidate)].push_back(idx);
          }
        }
      }
    }

    //! \brief Whether mode \p m has no active rule (so nothing can match in it).
    [[nodiscard]] bool mode_is_empty(std::size_t m) const
    {
      const dispatch& d {per_mode_[m]};
      return d.general.empty()
             && std::all_of(d.first_byte_index.begin(), d.first_byte_index.end(),
                            [](const std::vector<std::size_t>& bucket) { return bucket.empty(); });
    }

    //! \brief Fail-fast transition checks: a transition rule must consume input, and
    //!        a push/set target must be a defined, non-empty mode.
    //! \throws std::invalid_argument on a violation.
    void validate_transitions() const
    {
      for (const rule& candidate : rules_) {
        if (!candidate.action) {
          continue;
        }
        if (candidate.pattern.pattern().empty()) {
          throw std::invalid_argument("a transition rule must consume input (empty pattern)");
        }
        if (candidate.action->operation != mode_action::op::pop
            && mode_is_empty(mode_id_.at(candidate.action->target))) {
          throw std::invalid_argument("a transition targets the empty mode '"
                                      + candidate.action->target + "' (no rule is active in it)");
        }
      }
    }

    /*!
     * \brief Builds the per-mode first-byte dispatch from \ref rules_ (once, at
     *        construction). "default" is mode 0; every name in a rule's \ref
     *        rule::in_mode and every push/set target is interned to an id. For each
     *        mode, the rules active in it are bucketed by REAL's exact first-byte API,
     *        keeping \ref rules_ order so the index tie-break in \ref scan_next is the
     *        rule priority. Finishes with \ref validate_transitions.
     */
    void build_dispatch()
    {
      intern_mode("default"); // mode 0, always present
      for (const rule& candidate : rules_) {
        for (const std::string& name : candidate.in_mode) {
          intern_mode(name);
        }
        if (const std::optional<mode_action> action {candidate.action};
            action.has_value() && action->operation != mode_action::op::pop) {
          intern_mode(action->target);
        }
      }
      per_mode_.resize(mode_names_.size());

      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        if (rules_[idx].in_mode.empty()) {
          add_to_mode(0, idx); // an undeclared rule is active in "default" only
        }
        else {
          for (const std::string& name : rules_[idx].in_mode) {
            add_to_mode(mode_id_.at(name), idx);
          }
        }
      }
      validate_transitions();

      // Pre-resolve each transition's target mode id once, now that every mode is
      // interned and validated, so the per-token apply_transition reads a field instead
      // of a name→id map lookup. The target string stays for diagnostics; pop has none.
      for (rule& candidate : rules_) {
        if (candidate.action && candidate.action->operation != mode_action::op::pop) {
          candidate.action->target_id = mode_id_.at(candidate.action->target);
        }
      }
    }

    //! \brief Builds the layout-significance policy from the insignificant-mode
    //!        names (validated against the interned modes). With none, the policy
    //!        stays empty — every mode significant, so \ref scilex::layout is the
    //!        positional pass (invariant 1).
    void build_significance(const std::vector<std::string>& insignificant_modes)
    {
      if (insignificant_modes.empty()) {
        return;
      }
      mode_significant_.assign(mode_names_.size(), true);
      for (const std::string& name : insignificant_modes) {
        const auto found {mode_id_.find(name)};
        if (found == mode_id_.end()) {
          throw std::invalid_argument("insignificant_modes names an unknown mode: " + name);
        }
        mode_significant_[found->second] = false;
      }
    }

    //! \brief An adopted per-mode DFA: the automaton plus its local→global rule map.
    struct mode_dfa
    {
      real::dfa                dfa;       //!< Recognizes the mode's rules in one pass.
      std::vector<std::size_t> to_global; //!< DFA local rule index -> global rules_ index.
    };

    //! \brief A munch decision: whether a rule matched, which (global index), how many
    //!        bytes — the small value scan_next's Pike branch and the audit share.
    struct munch_result
    {
      bool        have {false};
      std::size_t idx  {0};
      std::size_t len  {0};
    };

    //! \brief The winning munch in \p mode at the start of \p rest (\p lead is rest's first byte),
    //!        dispatching to the mode's DFA when it has one, else the Pike + first-byte munch. The
    //!        single match primitive both the forward scan and the error-recovery probe call, so the
    //!        two never diverge on which rule wins.
    munch_result munch_at(std::size_t      mode,
                          std::string_view rest,
                          unsigned char    lead) const
    {
      if (per_mode_dfa_[mode]) {
        if (const std::optional<real::dfa_match> matched {per_mode_dfa_[mode]->dfa.match(rest)}) {
          return munch_result {.have = true, .idx = per_mode_dfa_[mode]->to_global[matched->rule_index],
                               .len = matched->length};
        }
        return munch_result {};
      }
      return pike_munch_in_mode(mode, rest, lead);
    }

    //! \brief O(1) pre-filter for error recovery: can a fixed-lead rule in \p mode begin with \p byte?
    //!        A false is conclusive (no rule can match, so the byte is error text); a true still needs a
    //!        full \ref munch_at to confirm a real match. This is what skips the bulk of noise cheaply.
    //!
    //! Only the first-byte buckets are consulted, not the mode's general (nullable) rules: a nullable
    //! rule matches the empty string at every position, so a mode that had one could never reach the
    //! no-match case (#1) that starts a recovery run — it would report a zero-length error (#4) at the
    //! very first byte instead. So during recovery the active mode provably has no general rule.
    bool may_start(std::size_t   mode,
                   unsigned char byte) const
    {
      return !per_mode_[mode].first_byte_index[byte].empty();
    }

    //! \brief Does a rule in \p mode match at \p offset in \p source? The error-recovery loop's stop
    //!        test — the smallest such offset ends an error run. Since recovery never runs in a mode
    //!        with a nullable rule (see \ref may_start), any match here has positive length, so
    //!        `have` alone is the stop condition (a zero-length win is impossible in this context).
    bool starts_a_match(std::size_t      mode,
                        std::string_view source,
                        std::size_t      offset) const
    {
      const unsigned char lead {static_cast<unsigned char>(source[offset])};
      if (!may_start(mode, lead)) {
        return false;
      }
      return munch_at(mode, source.substr(offset), lead).have;
    }

    //! \brief Advances \p cursor by \p n bytes of \p source, maintaining the 1-based line/column
    //!        tracker (a newline resets the column). The shared advance point for a matched token, a
    //!        recovery step, and the error-run scan.
    void advance(std::string_view source,
                 position&        cursor,
                 std::size_t      n) const
    {
      for (std::size_t i {0}; i < n; ++i) {
        if (source[cursor.offset] == '\n') {
          ++cursor.line;
          cursor.column = 1;
        }
        else {
          cursor.column += column_step(source, cursor.offset, columns_);
        }
        ++cursor.offset;
      }
    }

    //! \brief The length (1–4) of a valid UTF-8 codepoint starting at \p off in \p s, or 0 when the
    //!        byte there is not a valid lead — a continuation byte, a truncated/over­long/surrogate/
    //!        out-of-range sequence, or an invalid lead. The column stepper's UTF-8 validator.
    static std::size_t valid_utf8_len(std::string_view s,
                                      std::size_t      off)
    {
      const unsigned char b0  {static_cast<unsigned char>(s[off])};
      std::size_t         len {0};
      unsigned int        cp  {0};
      if (b0 < 0x80U) {
        return 1; // ASCII
      }
      if ((b0 & 0xE0U) == 0xC0U) {
        len = 2;
        cp  = b0 & 0x1FU;
      }
      else if ((b0 & 0xF0U) == 0xE0U) {
        len = 3;
        cp  = b0 & 0x0FU;
      }
      else if ((b0 & 0xF8U) == 0xF0U) {
        len = 4;
        cp  = b0 & 0x07U;
      }
      else {
        return 0; // a continuation byte (0x80–0xBF) or an invalid lead (0xF8–0xFF)
      }
      if (off + len > s.size()) {
        return 0; // truncated
      }
      for (std::size_t i {1}; i < len; ++i) {
        const unsigned char bi {static_cast<unsigned char>(s[off + i])};
        if ((bi & 0xC0U) != 0x80U) {
          return 0; // a missing continuation
        }
        cp = (cp << 6U) | (bi & 0x3FU);
      }
      static constexpr unsigned int min_for_len[5] {0, 0, 0x80U, 0x800U, 0x10000U};
      if (cp < min_for_len[len] || (cp >= 0xD800U && cp <= 0xDFFFU) || cp > 0x10FFFFU) {
        return 0; // overlong, a UTF-16 surrogate, or beyond U+10FFFF
      }
      return len;
    }

    //! \brief How much the column advances when the byte at \p off in \p source is consumed, under
    //!        \p unit. \c bytes is always 1 (so the byte mode is the historical column == byte offset).
    //!        \c codepoints counts one per valid codepoint (its lead scores 1, its continuations 0);
    //!        \c utf16 scores 2 for an astral (4-byte) codepoint, 1 otherwise. A malformed byte —
    //!        including an orphan continuation — scores 1 in every unit, so the column stays defined
    //!        across the error runs recovery emits.
    static std::size_t column_step(std::string_view        source,
                                   std::size_t             off,
                                   scilex::column_unit     unit)
    {
      if (unit == scilex::column_unit::bytes) {
        return 1;
      }
      const unsigned char byte {static_cast<unsigned char>(source[off])};
      if ((byte & 0xC0U) == 0x80U) { // a continuation byte
        // Score 0 only if it belongs to a valid codepoint whose lead is 1–3 bytes back; an orphan
        // continuation is malformed and scores 1. (A codepoint never spans a newline, so this
        // fixed look-back cannot cross a line boundary in a way that matters.)
        for (std::size_t back {1}; back <= 3 && back <= off; ++back) {
          if (valid_utf8_len(source, off - back) > back) {
            return 0;
          }
        }
        return 1;
      }
      if (unit == scilex::column_unit::utf16) {
        return valid_utf8_len(source, off) == 4 ? 2 : 1; // an astral codepoint is a surrogate pair
      }
      return 1; // codepoints: an ASCII byte or a lead (its continuations already scored 0)
    }

    //! \brief The per-rule Pike + first-byte-dispatch munch in \p mode at the start of
    //!        \p rest (\p lead is rest's first byte). Zero allocation; shared by
    //!        scan_next's Pike branch and the DFA audit. A zero-length match is reported
    //!        as a candidate (it wins only when nothing matches >0); scan_next's shared
    //!        guard turns such a win into a lexical error.
    munch_result pike_munch_in_mode(std::size_t      mode,
                                    std::string_view rest,
                                    unsigned char    lead) const
    {
      std::size_t best_len {0};
      std::size_t best_idx {0};
      bool        have     {false};
      const auto  consider {[&](std::size_t idx) {
                              // idx comes from this mode's first-byte dispatch, populated
                              // in build_dispatch() from rules_ indices, so it is always in
                              // range. The analyzer cannot prove that cross-vector invariant
                              // once this munch is a standalone shared method; a bounds guard
                              // would be an unreachable branch the 100%-4D gate rejects, so the
                              // proven false positive is suppressed here (see REPORT note).
                              // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
                              const auto matched {rules_[idx].pattern.match(rest)};
                              // A zero-length match participates (it can only win when no rule
                              // matches >0 here); the shared guard in scan_next turns that win
                              // into a lexical error rather than a stalled scan. Maximal munch
                              // still prefers any longer non-empty match.
                              if (matched
                                  && (!have || matched.end() > best_len
                                      || (matched.end() == best_len && idx < best_idx))) {
                                best_len = matched.end();
                                best_idx = idx;
                                have     = true;
                              }
                            }};
      const dispatch& active {per_mode_[mode]};
      for (const std::size_t idx : active.first_byte_index[lead]) {
        consider(idx);
      }
      for (const std::size_t idx : active.general) {
        consider(idx);
      }
      return {.have = have, .idx = best_idx, .len = best_len};
    }

    //! \brief Whether rule \p idx is active in mode \p mode (mirrors \ref build_dispatch,
    //!        an empty in_mode is the default mode only; otherwise the listed modes).
    [[nodiscard]] bool rule_active_in_mode(std::size_t idx,
                                           std::size_t mode) const
    {
      const std::vector<std::string>& modes {rules_[idx].in_mode};
      if (modes.empty()) {
        return mode == 0;
      }
      for (const std::string& name : modes) {
        if (mode_id_.at(name) == mode) {
          return true;
        }
      }
      return false;
    }

    //! \brief The bounded, deterministic probe inputs for the audit: every active rule's
    //!        possible first bytes ∪ structural bytes, as singletons and short repeats
    //!        (repeats expose lazy delimiters and quantifier boundaries — the hard cases),
    //!        then fixed-seed random strings. At most 512 inputs, each ≤ 48 bytes.
    std::vector<std::string> audit_probes(const std::vector<std::size_t>& to_global) const
    {
      std::array<bool, 256>      seen {};
      std::vector<unsigned char> alpha;
      const auto                 add {[&](unsigned char b) {
                                        if (!seen[b]) {
                                          seen[b] = true;
                                          alpha.push_back(b);
                                        }
                                      }};
      for (const std::size_t g : to_global) {
        for (int b {0}; b < 256; ++b) {
          if (rules_[g].pattern.may_start_with(static_cast<unsigned char>(b))) {
            add(static_cast<unsigned char>(b));
          }
        }
      }
      for (const char structural : std::string_view {" \t\n\"'/*-+=<>()[]{};.:,aAz09_"}) {
        add(static_cast<unsigned char>(structural));
      }

      // alpha is always non-empty (the structural bytes above are unconditional), so
      // the probe count is O(alphabet) + a fixed random batch — deterministic, bounded,
      // and free of cap branches. Singletons + short repeats expose lazy delimiters and
      // quantifier boundaries (the hard cases); the random batch broadens coverage.
      std::vector<std::string> probes;
      for (const unsigned char b : alpha) {
        for (const std::size_t n : std::array<std::size_t, 5> {1, 2, 3, 6, 8}) {
          probes.emplace_back(n, static_cast<char>(b));
        }
      }
      // Fixed seed by design: this RNG only generates local probe strings for the
      // build-time DFA equivalence audit, which must be reproducible. No security
      // role (no tokens, crypto or identifiers) — a constant seed is correct here.
      // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
      std::mt19937                               rng   {0x5C11EFU}; // fixed seed: the audit is reproducible
      std::uniform_int_distribution<std::size_t> len_d {1, 48};
      std::uniform_int_distribution<std::size_t> sym_d {0, alpha.size() - 1};
      for (int batch {0}; batch < 256; ++batch) {
        std::string       input;
        const std::size_t len {len_d(rng)};
        for (std::size_t i {0}; i < len; ++i) {
          input.push_back(static_cast<char>(alpha[sym_d(rng)]));
        }
        probes.push_back(std::move(input));
      }
      return probes;
    }

    //! \brief The candidate DFA must reproduce the Pike munch on every probe: catches
    //!        divergences the bytecode cannot reveal — chiefly a lazy quantifier, whose
    //!        match() is the shortest span while the DFA takes the longest.
    [[nodiscard]] bool audit_passes(const real::dfa&                candidate,
                                    const std::vector<std::size_t>& to_global,
                                    std::size_t                     mode) const
    {
      const std::vector<std::string> probes {audit_probes(to_global)};
      for (const std::string& probe : probes) {
        const std::string_view               rest    {probe}; // probes always have length >= 1
        const std::optional<real::dfa_match> hit     {candidate.match(rest)};
        const munch_result                   pike    {pike_munch_in_mode(mode, rest, static_cast<unsigned char>(rest[0]))};
        std::size_t                          dfa_idx {0};
        std::size_t                          dfa_len {0};
        if (hit.has_value()) {
          dfa_idx = to_global[hit->rule_index];
          dfa_len = hit->length;
        }
        // One comparison — the tuple's element-wise short-circuit lives in <tuple>, not
        // here — so any divergence (chiefly a lazy rule's shortest-vs-longest) rejects.
        if (std::tuple {hit.has_value(), dfa_idx, dfa_len} != std::tuple {pike.have, pike.idx, pike.len}) {
          return false;
        }
      }
      return true;
    }

    //! \brief Builds the \ref mode_dfa for one mode, or \c std::nullopt if the mode
    //!        cannot take the DFA fast path. Two non-error reasons return nullopt:
    //!        the rules contain an un-DFA-able assertion (\c real::dfa throws
    //!        \c real::dfa_error — caught here, turned into nullopt), or the DFA
    //!        fails the build-time equivalence audit (a lazy quantifier). Both leave
    //!        the mode on Pike. Returning the outcome instead of throwing past the
    //!        caller keeps the fast-path decision explicit at the call site.
    //! \param[in] to_global The mode's active rules, ascending global index (priority).
    //! \param[in] mode      The mode id (for the audit).
    std::optional<mode_dfa> try_build_mode_dfa(std::vector<std::size_t> to_global,
                                               std::size_t              mode)
    {
      std::vector<real::detail::program_view> programs;
      programs.reserve(to_global.size());
      for (const std::size_t g : to_global) {
        programs.push_back(rules_[g].pattern.raw_program());
      }
      try {
        real::dfa candidate {std::span<const real::detail::program_view>(programs)};
        if (!audit_passes(candidate, to_global, mode)) {
          return std::nullopt; // a divergence (e.g. a lazy rule) → keep this mode on Pike
        }
        return mode_dfa {.dfa = std::move(candidate), .to_global = std::move(to_global)};
      }
      catch (const real::dfa_error&) {
        return std::nullopt; // un-DFA-able assertion ($, \b, multiline ^/$): keep on Pike
      }
    }

    //! \brief Opts the named \p dfa_modes into the DFA fast path (called once, after
    //!        \ref build_dispatch). For each, builds a \c real::dfa from the mode's
    //!        active rules in ascending global index (= priority); an un-DFA-able
    //!        assertion (\c real::dfa_error) or a failed \ref audit_passes leaves the
    //!        mode on Pike (nullptr). Best-effort — see \ref dfa_modes_active.
    //! \param[in] dfa_modes The opted-in mode names. The build-time equivalence audit
    //!            always runs; its outcome is observable via \ref dfa_modes_active.
    //! \throws std::invalid_argument If \p dfa_modes names an unknown mode.
    void build_dfa_modes(const std::vector<std::string>& dfa_modes)
    {
      per_mode_dfa_.assign(mode_names_.size(), nullptr);
      for (const std::string& name : dfa_modes) {
        const auto found {mode_id_.find(name)};
        if (found == mode_id_.end()) {
          throw std::invalid_argument("dfa_modes names an unknown mode: " + name);
        }
        const std::size_t        mode {found->second};
        std::vector<std::size_t> to_global;
        for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
          if (rule_active_in_mode(idx, mode)) {
            to_global.push_back(idx);
          }
        }
        if (auto built {try_build_mode_dfa(std::move(to_global), mode)}) {
          per_mode_dfa_[mode] = std::make_shared<const mode_dfa>(std::move(*built));
        }
      }
    }

    //! \brief Per-mode dispatch index: the first-byte buckets scoped to one mode.
    struct dispatch
    {
      std::array<std::vector<std::size_t>, 256> first_byte_index; //!< Rule indices by leading byte.
      std::vector<std::size_t>                  general;          //!< Nullable rules (tried everywhere).
    };

    std::vector<rule>                            rules_;            //!< The ordered token rules.
    error_policy                                 errors_;           //!< What to do at an unmatched byte.
    scilex::column_unit                          columns_;          //!< The unit position::column is counted in.
    std::vector<std::string>                     mode_names_;       //!< Mode id -> name ("default" is id 0).
    std::map<std::string, std::size_t>           mode_id_;          //!< Mode name -> id.
    std::vector<dispatch>                        per_mode_;         //!< Dispatch index, one per mode (by id).
    std::vector<std::shared_ptr<const mode_dfa>> per_mode_dfa_;     //!< Per-mode DFA fast path (nullptr = Pike).
    std::vector<bool>                            mode_significant_; //!< Layout policy (empty = all significant).
  };

  /*!
   * \brief Forward (single-pass) iterator yielding one token at a time.
   *
   * A default-constructed iterator is the end sentinel. Each increment runs the
   * lexer just far enough to produce the next non-skipped token; a \ref
   * lex_error thrown by the lexer propagates out of the increment.
   */
  class token_iterator
  {
  public:

    using iterator_category = std::input_iterator_tag;     //!< Single-pass.
    using value_type        = token;                       //!< Yielded element.
    using difference_type   = std::ptrdiff_t;              //!< Required typedef.
    using pointer           = const token*;                //!< Pointer to current.
    using reference         = const token&;                //!< Reference to current.

    //! \brief Constructs the end sentinel.
    token_iterator() = default;

    /*!
     * \brief Constructs a begin iterator over \p source for \p owner.
     * \param[in] owner  The lexer providing the rules.
     * \param[in] source The text to scan.
     * \param[in] policy Whether to yield a terminal \ref end_of_input token.
     */
    token_iterator(const lexer&     owner,
                   std::string_view source,
                   eof_policy       policy)
      : owner_(&owner),
        source_(source),
        policy_(policy),
        done_(false)
    {
      advance();
    }

    //! \brief Returns the current token.
    reference operator*() const
    {
      return current_;
    }

    //! \brief Member access to the current token.
    pointer operator->() const
    {
      return &current_;
    }

    //! \brief Advances to the next token. \return `*this`.
    token_iterator& operator++()
    {
      advance();
      return *this;
    }

    //! \brief Post-increment (single-pass: no useful copy is returned).
    void operator++(int)
    {
      advance();
    }

    /*!
     * \brief Equality: both exhausted, or both at the same offset.
     * \param[in] other Another iterator.
     * \return `true` if the two denote the same position/end.
     */
    [[nodiscard]] bool operator==(const token_iterator& other) const
    {
      return done_ == other.done_ && (done_ || cursor_.offset == other.cursor_.offset);
    }

    /*!
     * \brief Inequality.
     * \param[in] other Another iterator.
     * \return `true` if the two differ.
     */
    [[nodiscard]] bool operator!=(const token_iterator& other) const
    {
      return !(*this == other);
    }

  private:

    const lexer*       owner_  {nullptr};                                               //!< Rules provider (not owned).
    std::string_view   source_;                                                         //!< Text being scanned.
    position           cursor_ {0, 1, 1};                                               //!< Current scan position.
    std::vector<frame> stack_  {frame {.mode_id = 0, .entry_pos = position {0, 1, 1}}}; //!< Mode stack (top = active).
    token              current_  {};                                                    //!< The current token.
    eof_policy         policy_   {eof_policy::omit};                                    //!< End-of-input policy.
    bool               eof_done_ {false};                                               //!< End-of-input token already yielded.
    bool               done_     {true};                                                //!< True once exhausted (end sentinel).

    //! \brief Produces the next token, or marks the iterator exhausted.
    void advance()
    {
      if (done_) {
        return;
      }
      if (owner_->scan_next(source_, cursor_, stack_, current_)) {
        return;
      }
      // Input exhausted: yield one end-of-input token if requested, else stop.
      if (policy_ == eof_policy::append && !eof_done_) {
        current_  = token {end_of_input, source_.substr(cursor_.offset), cursor_};
        eof_done_ = true;
        return;
      }
      done_ = true;
    }
  };

  /*!
   * \brief A lazy range of tokens, returned by \ref lexer::scan.
   *
   * Holds the lexer and source by reference/view; usable directly in range-for.
   */
  class token_range
  {
  public:

    /*!
     * \brief Builds the range.
     * \param[in] owner  The lexer providing the rules.
     * \param[in] source The text to scan.
     * \param[in] policy Whether to yield a terminal \ref end_of_input token.
     */
    token_range(const lexer&     owner,
                std::string_view source,
                eof_policy       policy)
      : owner_(&owner),
        source_(source),
        policy_(policy)
    {}

    //! \brief Begin iterator (produces the first token). \return The iterator.
    [[nodiscard]] token_iterator begin() const
    {
      return token_iterator(*owner_, source_, policy_);
    }

    //! \brief End sentinel. \return A default-constructed iterator.
    [[nodiscard]] token_iterator end() const
    {
      return token_iterator();
    }

  private:

    const lexer*     owner_  {nullptr};          //!< Rules provider (not owned).
    std::string_view source_;                    //!< Text being scanned.
    eof_policy       policy_ {eof_policy::omit}; //!< End-of-input policy.
  };

  inline token_range lexer::scan(std::string_view source,
                                 eof_policy       policy) const&
  {
    return token_range(*this, source, policy);
  }
} // namespace scilex

#endif // SCILEX_LEXER_HPP
