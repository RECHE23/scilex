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

#include <array>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
   * \brief A token rule: a kind, the pattern that recognizes it, and whether
   *        matches are discarded (whitespace, comments).
   */
  struct rule
  {
    int         kind;         //!< Kind assigned to tokens this rule produces.
    real::regex pattern;      //!< The recognizer (a linear-time REAL regex).
    bool        skip {false}; //!< If true, matches are consumed but not emitted.
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
     */
    explicit lexer(std::vector<rule> rules)
      : rules_(std::move(rules))
    {
      build_dispatch();
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
      token              next   {};
      while (scan_next(source, cursor, next)) {
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

  private:

    friend class token_iterator;

    /*!
     * \brief Advances \p cursor to and past the next non-skipped token.
     *
     * Skips over skip-rule matches, then fills \p out with the next emitted
     * token, advancing \p cursor (offset, line, byte column) past it. The
     * single source of scanning truth shared by \ref tokenize and the lazy
     * \ref token_iterator.
     *
     * \param[in]     source The text being scanned.
     * \param[in,out] cursor Current position; advanced past the consumed bytes.
     * \param[out]    out    Receives the next non-skipped token on success.
     * \return `true` if a token was produced, `false` at end of input.
     * \throws lex_error If \p cursor sits on input no rule matches.
     */
    bool scan_next(std::string_view source,
                   position&        cursor,
                   token&           out) const
    {
      while (cursor.offset < source.size()) {
        const std::string_view rest     {source.substr(cursor.offset)};
        std::size_t            best_len {0};
        std::size_t            best_idx {0};
        bool                   have     {false};

        // First-byte dispatch: only rules whose possible-first-byte set contains the
        // byte at the cursor (its bucket) plus the general rules (nullable patterns,
        // which can match the empty string and so never narrow the position) are tried
        // here — every other rule provably cannot match, per REAL's first-byte API. The
        // longest match wins; on a tie the earlier rule in rules_ wins (lowest index),
        // so priority order is preserved no matter which list is scanned first.
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
        for (const std::size_t idx : first_byte_index_[static_cast<unsigned char>(source[cursor.offset])]) {
          consider(idx);
        }
        for (const std::size_t idx : general_rules_) {
          consider(idx);
        }

        if (!have) {
          throw lex_error("no rule matches the input", cursor);
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

        if (!rules_[best_idx].skip) {
          out = token {rules_[best_idx].kind, source.substr(start.offset, best_len), start};
          return true;
        }
        // Skip rule: keep scanning for the next emitted token.
      }
      return false;
    }

    /*!
     * \brief Builds the first-byte dispatch index from \ref rules_ (once, at
     *        construction), via REAL's exact first-byte API:
     *        - a nullable pattern (no usable first-byte set — it can match the empty
     *          string, e.g. `a?`) joins \ref general_rules_ and is tried everywhere;
     *        - a pattern with a single possible first byte joins that one bucket;
     *        - any other pattern (a class, an alternation, an `icase` literal that
     *          folds to several bytes…) joins every bucket its set admits.
     *        Buckets and the general list keep \ref rules_ order, so the index-order
     *        tie-break in \ref scan_next is the rule priority order. The dispatch is
     *        exact — a rule is tried at a position iff its pattern can begin there —
     *        so it only ever skips rules that provably cannot match.
     */
    void build_dispatch()
    {
      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        const real::regex& pattern {rules_[idx].pattern};
        if (!pattern.has_first_byte_set()) {
          general_rules_.push_back(idx);
        }
        else if (const std::optional<unsigned char> byte {pattern.unique_first_byte()}) {
          first_byte_index_[*byte].push_back(idx);
        }
        else {
          for (int candidate {0}; candidate < 256; ++candidate) {
            if (pattern.may_start_with(static_cast<unsigned char>(candidate))) {
              first_byte_index_[static_cast<unsigned char>(candidate)].push_back(idx);
            }
          }
        }
      }
    }

    std::vector<rule>                         rules_;            //!< The ordered token rules.
    std::array<std::vector<std::size_t>, 256> first_byte_index_; //!< Rule indices by possible leading byte.
    std::vector<std::size_t>                  general_rules_;    //!< Indices of nullable rules (tried everywhere).
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

    const lexer*     owner_    {nullptr};          //!< Rules provider (not owned).
    std::string_view source_;                      //!< Text being scanned.
    position         cursor_   {0, 1, 1};          //!< Current scan position.
    token            current_  {};                 //!< The current token.
    eof_policy       policy_   {eof_policy::omit}; //!< End-of-input policy.
    bool             eof_done_ {false};            //!< End-of-input token already yielded.
    bool             done_     {true};             //!< True once exhausted (end sentinel).

    //! \brief Produces the next token, or marks the iterator exhausted.
    void advance()
    {
      if (done_) {
        return;
      }
      if (owner_->scan_next(source_, cursor_, current_)) {
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
