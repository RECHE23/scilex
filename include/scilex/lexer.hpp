/*!
 * \file lexer.hpp
 * \brief The lexer: maximal-munch tokenization over a set of REAL patterns.
 *
 * SciLex is a thin layer over REAL. Each \ref scilex::rule pairs a token kind
 * with a `real::regex`; the lexer scans the source left to right, and at each
 * position picks the rule with the **longest** anchored match (maximal munch),
 * breaking ties by rule order (earlier rules have priority). Because REAL is a
 * linear-time engine, every match is linear in what it scans and ReDoS-safe by
 * construction — no token rule can make the scanner backtrack catastrophically.
 * The scan as a whole is linear wherever a mode's rules run on its DFA (each walk
 * is memoized over the source), and quadratic in the worst case only through a rule
 * left on Pike that scans far past the token that wins at every position (see the
 * spec's complexity section).
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
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <numeric>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
    //!        so the per-token transition is
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
   * But a Unicode `\w` expands into more UTF-8 byte transitions than a DFA is built from, and `\b` is a
   * zero-width assertion no DFA represents, so a rule holding either **stays on the general Pike
   * engine** while the mode's other rules take the DFA (same tokens). That rule is tried at every
   * position its first byte allows, so it keeps a share of the per-rule cost. The narrower Unicode
   * `\d` and `\s` expand and stay on the DFA. Concretely: the general engine lexes at roughly
   * **6–9.5 MB/s**, while a fully DFA-accelerated mode runs **3–27× that** — so the Unicode
   * identifier costs part of the DFA fast path.
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
   * \brief The deepest the mode stack may grow. A push beyond it is a \ref lex_error under either
   *        error policy: each frame costs memory, and without a bound an input of openers alone (16 MiB
   *        of `(` under the python grammar) grew the stack past a gigabyte. 65 536 frames is ~2 MiB,
   *        far beyond any nesting a real grammar reaches.
   */
  inline constexpr std::size_t max_mode_depth {65536};

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
   * \throws lex_error On a pop while the stack is at its root (nothing to leave), or a push that would
   *         take it past \ref max_mode_depth.
   */
  inline void apply_transition(const rule&         r,
                               position            start,
                               std::vector<frame>& stack)
  {
    if (!r.action) {
      return;
    }
    if (r.action->operation == mode_action::op::push) {
      if (stack.size() >= max_mode_depth) {
        throw lex_error("cannot push another mode: the mode stack is " + std::to_string(max_mode_depth)
                        + " deep (scilex::max_mode_depth)", start);
      }
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
   * \brief Which modes a lexer tries to accelerate with a \c real::dfa.
   *
   * The token stream is the same either way: a mode keeps its DFA only where the lexer has decided
   * that the DFA reproduces the per-rule munch on every input (see \ref lexer::dfa_modes_active).
   */
  enum class dfa_policy : std::uint8_t
  {
    //! Every mode (the default). In each, the rules a DFA reproduces go on one and the others stay on
    //! the per-rule path; the cost is the DFA construction.
    automatic,
    //! Only the modes named in `dfa_modes`; an empty list keeps every mode on the per-rule path.
    requested,
  };

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
   *
   * \par Thread safety
   * A lexer is immutable once built: its DFAs are built in the constructor, and every
   * \ref tokenize call and every \ref scan range keeps its own mode stack and walk
   * memos. One `const` lexer may therefore be shared by any number of threads, each
   * tokenizing its own source. A \ref token_iterator is a cursor: drive each one from
   * a single thread. Rules left on Pike (\ref pike_rules) call `real::regex`, whose
   * lazy-DFA cache is shared per regex behind a lock in REAL 2026.9.7, so a grammar
   * with such rules may not scale with the thread count the way a DFA mode does.
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
     *            DFA pass replaces the per-rule Pike dispatch) under \ref dfa_policy::requested;
     *            under the default \ref dfa_policy::automatic every mode is tried and this list
     *            adds nothing. Each name must be a mode the rules use. The attempt is best-effort: a rule that cannot
     *            be a DFA (a zero-width assertion) or whose DFA would change an answer
     *            (its `match()` is not its longest match, such as `as|assert`
     *            or a lazy delimiter) silently stays on Pike beside the mode's DFA — see
     *            \ref dfa_modes_active. The token stream is identical either way: the
     *            decision is exact, not sampled.
     * \param[in] errors What to do at a byte no rule can lex: \ref error_policy::raise (the default —
     *            throw) or \ref error_policy::token (recover, emitting an \ref scilex::error token). The
     *            recovery path never throws per byte; the token stream under \c raise is unchanged.
     * \param[in] columns The unit each token's \c position::column is counted in:
     *            \ref column_unit::bytes (the default, unchanged), \ref column_unit::codepoints, or
     *            \ref column_unit::utf16. The unit is not stored on the position — read it back with
     *            \ref columns().
     * \param[in] dfa Which modes are tried for DFA acceleration (\ref dfa_policy); the default
     *            tries all of them.
     * \throws std::invalid_argument If a transition rule is malformed (empty
     *         pattern or target), or \p insignificant_modes / \p dfa_modes names an
     *         unknown mode.
     */
    explicit lexer(std::vector<rule>               rules,
                   std::vector<std::string>        insignificant_modes        = {},
                   std::vector<std::string>        dfa_modes                  = {},
                   error_policy                    errors                     = error_policy::raise,
                   column_unit                     columns                    = column_unit::bytes,
                   dfa_policy                      dfa                        = dfa_policy::automatic)
      : rules_(std::move(rules)),
        errors_(errors),
        columns_(columns)
    {
      build_dispatch();
      build_significance(insignificant_modes);
      build_dfa_modes(dfa_modes, dfa);
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
      munch_memos        memos  {memos_for(source)};
      while (scan_next(source, cursor, stack, next, memos)) {
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
    //! A mode is listed when at least one of its rules runs on the DFA. A rule the DFA cannot take — it
    //! needs an assertion no DFA represents (\c real::dfa_error), or its `match()` is not its longest
    //! match — stays on Pike beside the DFA; a mode where no rule can go on one is **absent** here.
    //! The tokens are the same either way: acceleration is an optimizer, not a guarantee.
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

    /*!
     * \brief The rules of \p mode that run on the per-rule Pike path, by index into the rules the
     *        lexer was built from: every rule of a mode with no DFA, and in an accelerated mode the
     *        rules its DFA cannot take (see \ref dfa_modes_active).
     * \param[in] mode A mode the rules use.
     * \return The indices, ascending.
     * \throws std::invalid_argument If \p mode is not a mode the rules use.
     */
    [[nodiscard]] std::vector<std::size_t> pike_rules(const std::string& mode) const
    {
      const auto found {mode_id_.find(mode)};
      if (found == mode_id_.end()) {
        throw std::invalid_argument("pike_rules names an unknown mode: " + mode);
      }
      const mode_dfa* const    hybrid {per_mode_dfa_[found->second].get()};
      std::vector<std::size_t> on_pike;
      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        if (rule_active_in_mode(idx, found->second) && (hybrid == nullptr || hybrid->on_pike[idx])) {
          on_pike.push_back(idx);
        }
      }
      return on_pike;
    }

  private:

    friend class token_iterator;

    //! \brief Per mode, what the munches over one source have proved (\c real::dfa_munch_memo); one
    //!        object per tokenization or iterator, made by \ref memos_for. An unarmed memo holds
    //!        nothing, so making one per mode up front costs nothing a scan would notice.
    using munch_memos = std::vector<real::dfa_munch_memo>;

    //! \brief The memos for one scan of \p source: one per mode, each unarmed.
    //! \param[in] source The text about to be scanned.
    //! \return The memos, indexed by mode id.
    [[nodiscard]] munch_memos memos_for(std::string_view source) const
    {
      return munch_memos(mode_names_.size(), real::dfa_munch_memo {source.size()});
    }

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
     * \param[in,out] memos  What earlier munches over \p source proved, per mode (see
     *                \ref munch_memos); the same object for every call over one source.
     * \return `true` if a token was produced, `false` at end of input.
     * \throws lex_error If a position matches no rule in the active mode (#1), a rule
     *         pops at the stack root or pushes past \ref max_mode_depth (#2), input ends inside a
     *         pushed mode (#3), or the
     *         winning match is zero-length and so cannot advance the scan (#4).
     */
    bool scan_next(std::string_view    source,
                   position&           cursor,
                   std::vector<frame>& stack,
                   token&              out,
                   munch_memos&        memos) const
    {
      while (cursor.offset < source.size()) {
        const std::size_t  mode {stack.back().mode_id};
        const munch_result m    {munch_at(mode, source, cursor.offset, memos)};

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
                 && !starts_a_match(mode, source, cursor.offset, memos)) {
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
      real::dfa                  dfa;           //!< Recognizes the mode's DFA rules in one pass.
      std::vector<std::size_t>   to_global;     //!< DFA local rule index -> global rules_ index.
      std::vector<bool>          on_pike;       //!< By global index: a rule of this mode the DFA cannot take.
      bool                       any_on_pike;   //!< Whether any rule of the mode stays on Pike.
      std::optional<std::size_t> empty_winner;  //!< The lowest-index DFA rule that matches the empty string.
    };

    //! \brief A munch decision: whether a rule matched, which (global index), how many
    //!        bytes — the small value scan_next's Pike and DFA branches share.
    struct munch_result
    {
      bool        have {false};
      std::size_t idx  {0};
      std::size_t len  {0};
    };

    //! \brief The winning munch in \p mode at \p offset of \p source (\p memos: see \ref munch_memos),
    //!        dispatching to the mode's DFA when it has one, else the Pike + first-byte munch. The
    //!        single match primitive both the forward scan and the error-recovery probe call, so the
    //!        two never diverge on which rule wins.
    munch_result munch_at(std::size_t      mode,
                          std::string_view source,
                          std::size_t      offset,
                          munch_memos&     memos) const
    {
      const mode_dfa* const hybrid {per_mode_dfa_[mode].get()};
      if (hybrid == nullptr) {
        return pike_munch_in_mode(mode, source.substr(offset), static_cast<unsigned char>(source[offset]), nullptr);
      }
      // The DFA's walk is memoized over the whole source (real::dfa_munch_memo): a state a walk proved
      // leads to no accept stops every later walk that reaches it, so the DFA's share of a
      // tokenization is linear in the source rather than quadratic.
      real::dfa_munch_memo& memo {memos[mode]};
      // The Pike munch over the whole mode, assembled from its parts: the longest match wins and the
      // lowest index breaks a tie. The DFA answers for its rules' non-empty matches (each rule's
      // match() is its longest, so the DFA's longest is theirs); a DFA rule's empty match, which the
      // DFA never reports, competes through empty_winner; the rules the DFA cannot take run on Pike.
      munch_result best {};
      if (const std::optional<real::dfa_match> matched {hybrid->dfa.match(source, offset, memo)}) {
        best = munch_result {.have = true, .idx = hybrid->to_global[matched->rule_index], .len = matched->length};
      }
      else if (hybrid->empty_winner) {
        best = munch_result {.have = true, .idx = *hybrid->empty_winner, .len = 0};
      }
      if (hybrid->any_on_pike) {
        const munch_result rest_of_mode {pike_munch_in_mode(mode, source.substr(offset),
                                                            static_cast<unsigned char>(source[offset]),
                                                            &hybrid->on_pike)};
        if (rest_of_mode.have
            && (!best.have || rest_of_mode.len > best.len
                || (rest_of_mode.len == best.len && rest_of_mode.idx < best.idx))) {
          best = rest_of_mode;
        }
      }
      return best;
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
                        std::size_t      offset,
                        munch_memos&     memos) const
    {
      if (!may_start(mode, static_cast<unsigned char>(source[offset]))) {
        return false;
      }
      return munch_at(mode, source, offset, memos).have;
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
    //!        scan_next's Pike branch. A zero-length match is reported
    //!        as a candidate (it wins only when nothing matches >0); scan_next's shared
    //!        guard turns such a win into a lexical error.
    munch_result pike_munch_in_mode(std::size_t              mode,
                                    std::string_view         rest,
                                    unsigned char            lead,
                                    const std::vector<bool>* only) const
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
                              if (only != nullptr && !(*only)[idx]) {
                                return; // this rule is answered by the mode's DFA
                              }
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

    //! \brief The mode's hybrid munch, or nothing when no rule of it can go on a DFA.
    //!
    //! A rule joins the DFA when \c real::dfa_faithful decides that its `match()` is its longest match
    //! on every input -- exactly, not sampled. The Pike munch picks the longest per-rule `match()` and
    //! the DFA the longest match of any of its rules, so over such rules the two agree on the length and
    //! on the earliest rule. Which rules pass is not a syntactic property: `as|assert` fails (its
    //! `match()` stops at "as") and the lazy `x*?y` passes; a rule no DFA represents (a `\b`, a `$`, a
    //! lookaround, a class too wide) raises \c real::dfa_error and fails too. Every failing rule stays
    //! on Pike beside the DFA, and \ref munch_at merges the two, so one such rule no longer sends its
    //! whole mode back to Pike. An exhausted decision budget counts as a failure.
    //! \param[in] to_global The mode's active rules, in ascending global index (= priority).
    //! \return The DFA over the passing rules with the bookkeeping \ref munch_at merges by, or
    //!         `std::nullopt` when no rule passes or their union outgrows the DFA's state cap.
    std::optional<mode_dfa> try_build_mode_dfa(const std::vector<std::size_t>& to_global)
    {
      std::vector<std::size_t> on_dfa;
      std::vector<bool>        on_pike(rules_.size(), false);
      bool                     any_on_pike {false};
      for (const std::size_t g : to_global) {
        bool faithful {false};
        try {
          faithful = real::dfa_faithful(rules_[g].pattern).outcome == real::dfa_fidelity_outcome::faithful;
        }
        catch (const real::dfa_error&) {
          faithful = false; // not DFA-able
        }
        if (faithful) {
          on_dfa.push_back(g);
        }
        else {
          on_pike[g]  = true;
          any_on_pike = true;
        }
      }
      if (on_dfa.empty()) {
        return std::nullopt;
      }
      std::vector<real::regex> patterns;
      patterns.reserve(on_dfa.size());
      std::optional<std::size_t> empty_winner;
      for (const std::size_t g : on_dfa) {
        patterns.push_back(rules_[g].pattern);
        if (!empty_winner && rules_[g].pattern.match(std::string_view {}).matched()) {
          empty_winner = g; // on_dfa ascends, so the first is the lowest index
        }
      }
      try {
        return mode_dfa {.dfa          = real::dfa {std::span<const real::regex>(patterns)},
                         .to_global    = std::move(on_dfa),
                         .on_pike      = std::move(on_pike),
                         .any_on_pike  = any_on_pike,
                         .empty_winner = empty_winner};
      }
      catch (const real::dfa_error&) {
        return std::nullopt; // the rules pass one by one, but their union outgrows the state cap
      }
    }

    //! \brief Tries modes for the DFA fast path (called once, after \ref build_dispatch):
    //!        every mode under \ref dfa_policy::automatic, the named \p dfa_modes under
    //!        \ref dfa_policy::requested. For each, \ref try_build_mode_dfa puts the rules a DFA
    //!        reproduces on one and leaves the others on Pike; a mode with no such rule stays
    //!        entirely on Pike (nullptr). Best-effort — see \ref dfa_modes_active.
    //! \param[in] dfa_modes The named modes. The build-time decision always runs; its
    //!            outcome is observable via \ref dfa_modes_active.
    //! \param[in] policy    Whether every mode is tried or only \p dfa_modes.
    //! \throws std::invalid_argument If \p dfa_modes names an unknown mode.
    void build_dfa_modes(const std::vector<std::string>& dfa_modes,
                         dfa_policy                      policy)
    {
      per_mode_dfa_.assign(mode_names_.size(), nullptr);
      std::vector<std::size_t> tried;
      for (const std::string& name : dfa_modes) {
        const auto found {mode_id_.find(name)};
        if (found == mode_id_.end()) {
          throw std::invalid_argument("dfa_modes names an unknown mode: " + name);
        }
        tried.push_back(found->second);
      }
      if (policy == dfa_policy::automatic) {
        tried.resize(mode_names_.size());
        std::iota(tried.begin(), tried.end(), std::size_t {0});
      }
      std::ranges::sort(tried);
      tried.erase(std::ranges::unique(tried).begin(), tried.end()); // a mode named twice is built once
      for (const std::size_t mode : tried) {
        std::vector<std::size_t> to_global;
        for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
          if (rule_active_in_mode(idx, mode)) {
            to_global.push_back(idx);
          }
        }
        if (auto built {try_build_mode_dfa(to_global)}) {
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
        done_(false),
        memos_(owner.memos_for(source))
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
    lexer::munch_memos memos_;                                                          //!< What this scan's munches proved.

    //! \brief Produces the next token, or marks the iterator exhausted.
    void advance()
    {
      if (done_) {
        return;
      }
      if (owner_->scan_next(source_, cursor_, stack_, current_, memos_)) {
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
