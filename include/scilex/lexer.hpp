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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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
  };

  /*!
   * \brief A token rule: a kind, the pattern that recognizes it, whether matches
   *        are discarded (whitespace, comments), and — for contextual lexing — the
   *        modes it is active in and an optional mode transition it fires when it wins.
   *
   * \ref in_mode empty means the rule is active in the implicit "default" mode only,
   * so a plain `{kind, pattern, skip}` rule keeps working unchanged.
   */
  struct rule
  {
    int                        kind;            //!< Kind assigned to tokens this rule produces.
    real::regex                pattern;         //!< The recognizer (a linear-time REAL regex).
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
   * Depends only on \p r's action, the token start \p start, the \p stack, and the
   * \p mode_id name→id map; it mutates only \p stack. push enters the target
   * (remembering \p start), pop leaves the current mode, set replaces it in place.
   *
   * \throws lex_error On a pop while the stack is at its root (nothing to leave).
   */
  inline void apply_transition(const rule&                               r,
                               position                                  start,
                               std::vector<frame>&                       stack,
                               const std::map<std::string, std::size_t>& mode_id)
  {
    if (!r.action) {
      return;
    }
    if (r.action->operation == mode_action::op::push) {
      stack.push_back(frame {.mode_id = mode_id.at(r.action->target), .entry_pos = start});
    }
    else if (r.action->operation == mode_action::op::pop) {
      if (stack.size() == 1) {
        throw lex_error("cannot pop the mode stack: already at the root mode", start);
      }
      stack.pop_back();
    }
    else { // set: replace the active mode in place (depth unchanged)
      stack.back().mode_id = mode_id.at(r.action->target);
    }
  }

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
     * \throws std::invalid_argument If a transition rule is malformed (empty
     *         pattern or target), or \p insignificant_modes names an unknown mode.
     */
    explicit lexer(std::vector<rule>               rules,
                   std::unordered_set<std::string> insignificant_modes = {})
      : rules_(std::move(rules))
    {
      build_dispatch();
      build_significance(insignificant_modes);
    }

    /*!
     * \brief Tokenizes \p source into the sequence of non-skipped tokens.
     *
     * At each position every rule is matched anchored; the longest match wins
     * (ties broken by rule order). Empty matches are ignored — a rule that
     * matches nothing cannot consume input, so it never stalls the scan.
     *
     * \param[in] source The text to tokenize (must outlive the returned tokens;
     *            each token's lexeme views into it).
     * \param[in] policy Whether to append a terminal \ref end_of_input token.
     * \return The tokens in source order, skip-rule matches omitted.
     * \throws lex_error If some position is matched by no rule.
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
     *         pops at the stack root (#2), or input ends inside a pushed mode (#3).
     */
    bool scan_next(std::string_view    source,
                   position&           cursor,
                   std::vector<frame>& stack,
                   token&              out) const
    {
      while (cursor.offset < source.size()) {
        const std::string_view rest     {source.substr(cursor.offset)};
        const std::size_t      mode     {stack.back().mode_id};
        std::size_t            best_len {0};
        std::size_t            best_idx {0};
        bool                   have     {false};

        // First-byte dispatch within the active mode: only rules whose possible-first-
        // byte set contains the cursor byte (its bucket) plus that mode's general
        // (nullable) rules are tried, per REAL's first-byte API. Longest match wins;
        // ties go to the earlier rule (lowest index), preserving rule priority.
        const auto consider = [&](std::size_t idx) {
                                const auto matched {rules_[idx].pattern.match(rest)};
                                if (matched && matched.end() > 0
                                    && (!have || matched.end() > best_len
                                        || (matched.end() == best_len && idx < best_idx))) {
                                  best_len = matched.end();
                                  best_idx = idx;
                                  have     = true;
                                }
                              };
        const dispatch& active {per_mode_[mode]};
        for (const std::size_t idx : active.first_byte_index[static_cast<unsigned char>(source[cursor.offset])]) {
          consider(idx);
        }
        for (const std::size_t idx : active.general) {
          consider(idx);
        }

        if (!have) {
          throw lex_error("no rule matches in mode '" + mode_names_[mode] + "' (entered at "
                          + position_label(stack.back().entry_pos) + ")", cursor); // #1
        }

        const position start {cursor};
        // Advance the line/column tracker across the consumed bytes.
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

        apply_transition(rules_[best_idx], start, stack, mode_id_); // advances, then transitions (#2 on a bad pop)

        if (!rules_[best_idx].skip) {
          // Tag the token with the mode it was lexed in (captured before the
          // transition above) — Layout Awareness reads it; the scan is untouched.
          out = token {rules_[best_idx].kind, source.substr(start.offset, best_len), start, mode};
          return true;
        }
        // Skip rule: keep scanning for the next emitted token (possibly in a new mode).
      }
      if (stack.size() > 1) {
        throw lex_error("unterminated mode '" + mode_names_[stack.back().mode_id] + "' (entered at "
                        + position_label(stack.back().entry_pos) + ")", stack.back().entry_pos); // #3
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
        if (candidate.action && candidate.action->operation != mode_action::op::pop) {
          intern_mode(candidate.action->target);
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
    }

    //! \brief Builds the layout-significance policy from the insignificant-mode
    //!        names (validated against the interned modes). With none, the policy
    //!        stays empty — every mode significant, so \ref scilex::layout is the
    //!        positional pass (invariant 1).
    void build_significance(const std::unordered_set<std::string>& insignificant_modes)
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

    //! \brief Per-mode dispatch index: the ⑤ first-byte buckets scoped to one mode.
    struct dispatch
    {
      std::array<std::vector<std::size_t>, 256> first_byte_index; //!< Rule indices by leading byte.
      std::vector<std::size_t>                  general;          //!< Nullable rules (tried everywhere).
    };

    std::vector<rule>                  rules_;            //!< The ordered token rules.
    std::vector<std::string>           mode_names_;       //!< Mode id -> name ("default" is id 0).
    std::map<std::string, std::size_t> mode_id_;          //!< Mode name -> id.
    std::vector<dispatch>              per_mode_;         //!< Dispatch index, one per mode (by id).
    std::vector<bool>                  mode_significant_; //!< Layout policy (empty = all significant).
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
