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

#include <iterator>
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
    {}

    /*!
     * \brief Tokenizes \p source into the sequence of non-skipped tokens.
     *
     * At each position every rule is matched anchored; the longest match wins
     * (ties broken by rule order). Empty matches are ignored — a rule that
     * matches nothing cannot consume input, so it never stalls the scan.
     *
     * \param[in] source The text to tokenize (must outlive the returned tokens;
     *            each token's lexeme views into it).
     * \return The tokens in source order, skip-rule matches omitted.
     * \throws lex_error If some position is matched by no rule.
     */
    [[nodiscard]] std::vector<token> tokenize(std::string_view source) const
    {
      std::vector<token> out;
      position           cursor {0, 1, 1};
      token              next   {};
      while (scan_next(source, cursor, next)) {
        out.push_back(next);
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
     * \return A \ref token_range whose iterators yield \ref token values.
     * \throws lex_error (while iterating) if some position matches no rule.
     */
    [[nodiscard]] token_range scan(std::string_view source) const&;

    //! \brief Deleted: the range would point into a temporary lexer.
    token_range               scan(std::string_view) const&& = delete;

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
        const rule*            best     {nullptr};

        // Longest anchored match across all rules; strict '>' keeps the earlier
        // rule on a tie, so rule order is the priority order.
        for (const rule& candidate : rules_) {
          const auto matched {candidate.pattern.match(rest)};
          if (matched && matched.end() > best_len) {
            best_len = matched.end();
            best     = &candidate;
          }
        }

        if (best == nullptr) {
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

        if (!best->skip) {
          out = token {best->kind, source.substr(start.offset, best_len), start};
          return true;
        }
        // Skip rule: keep scanning for the next emitted token.
      }
      return false;
    }

    std::vector<rule> rules_; //!< The ordered token rules.
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
     */
    token_iterator(const lexer&     owner,
                   std::string_view source)
      : owner_(&owner),
        source_(source),
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

    const lexer*     owner_   {nullptr}; //!< Rules provider (not owned).
    std::string_view source_;            //!< Text being scanned.
    position         cursor_  {0, 1, 1}; //!< Current scan position.
    token            current_ {};        //!< The current token.
    bool             done_    {true};    //!< True once exhausted (end sentinel).

    //! \brief Produces the next token, or marks the iterator exhausted.
    void advance()
    {
      if (done_) {
        return;
      }
      if (!owner_->scan_next(source_, cursor_, current_)) {
        done_ = true;
      }
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
     */
    token_range(const lexer&     owner,
                std::string_view source)
      : owner_(&owner),
        source_(source)
    {}

    //! \brief Begin iterator (produces the first token). \return The iterator.
    [[nodiscard]] token_iterator begin() const
    {
      return token_iterator(*owner_, source_);
    }

    //! \brief End sentinel. \return A default-constructed iterator.
    [[nodiscard]] token_iterator end() const
    {
      return token_iterator();
    }

  private:

    const lexer*     owner_ {nullptr}; //!< Rules provider (not owned).
    std::string_view source_;          //!< Text being scanned.
  };

  inline token_range lexer::scan(std::string_view source) const&
  {
    return token_range(*this, source);
  }
} // namespace scilex

#endif // SCILEX_LEXER_HPP
