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
 */
#ifndef SCILEX_LEXER_HPP
#define SCILEX_LEXER_HPP

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <real/real.hpp>

#include "token.hpp"

namespace scilex {

  /*!
   * \brief A token rule: a kind, the pattern that recognizes it, and whether
   *        matches are discarded (whitespace, comments).
   */
  struct rule
  {
    int         kind;          //!< Kind assigned to tokens this rule produces.
    real::regex pattern;       //!< The recognizer (a linear-time REAL regex).
    bool        skip {false};  //!< If true, matches are consumed but not emitted.
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
      std::size_t        offset {0};
      std::size_t        line   {1};
      std::size_t        column {1};

      while (offset < source.size()) {
        const std::string_view rest     {source.substr(offset)};
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
          throw lex_error("no rule matches the input", position {offset, line, column});
        }

        if (!best->skip) {
          out.push_back(token {best->kind,
                               source.substr(offset, best_len),
                               position {offset, line, column}});
        }

        // Advance the line/column tracker across the consumed bytes.
        for (std::size_t i {0}; i < best_len; ++i) {
          if (source[offset + i] == '\n') {
            ++line;
            column = 1;
          }
          else {
            ++column;
          }
        }
        offset += best_len;
      }
      return out;
    }

  private:

    std::vector<rule> rules_; //!< The ordered token rules.
  };
} // namespace scilex

#endif // SCILEX_LEXER_HPP
