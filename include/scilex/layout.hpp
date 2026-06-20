/*!
 * \file layout.hpp
 * \brief Optional indentation layout: insert NEWLINE / INDENT / DEDENT tokens.
 *
 * Some languages (Python-like, e.g. SciLang) make indentation significant. This
 * opt-in pass turns a flat token stream into a layout-aware one: it inserts a
 * \ref scilex::newline at each logical line end, and \ref scilex::indent /
 * \ref scilex::dedent where the leading indentation changes.
 *
 * It works purely from token **positions** — every \ref scilex::token already
 * carries its source line and (byte) column — so the base lexer needs no change
 * and may keep skipping whitespace. Lines with no token (blank or
 * comment-only) carry no structure and are naturally ignored.
 *
 * Indentation width is the byte column of a line's first token (tabs and spaces
 * each count as one column; it does not police mixed tabs/spaces, and there is
 * no implicit line continuation inside brackets).
 *
 * Input must be an end-of-input-terminated token sequence (the lexer's
 * `eof_policy::append`); the terminal \ref scilex::end_of_input is preserved.
 */
#ifndef SCILEX_LAYOUT_HPP
#define SCILEX_LAYOUT_HPP

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "token.hpp"

namespace scilex {

  //! \brief Reserved kind: end of a logical line.
  inline constexpr int newline {std::numeric_limits<int>::min() + 1};
  //! \brief Reserved kind: indentation increased (start of a deeper block).
  inline constexpr int indent  {std::numeric_limits<int>::min() + 2};
  //! \brief Reserved kind: indentation decreased (end of a block).
  inline constexpr int dedent  {std::numeric_limits<int>::min() + 3};

  /*!
   * \brief Thrown when a line's indentation matches no enclosing level.
   */
  class layout_error : public std::runtime_error
  {
  public:

    //! \brief Builds the error. \param[in] message Cause. \param[in] where Position.
    layout_error(const std::string& message,
                 position           where)
      : std::runtime_error(message),
        where_(where)
    {}

    //! \brief Returns the position of the offending line.
    [[nodiscard]] position where() const noexcept
    {
      return where_;
    }

  private:

    position where_; //!< Where the indentation was inconsistent.
  };

  /*!
   * \brief Rewrites \p tokens with NEWLINE / INDENT / DEDENT inserted.
   *
   * \param[in] tokens An end-of-input-terminated token sequence.
   * \return The layout-aware token sequence (still end-of-input-terminated).
   * \throws layout_error If a line dedents to an indentation that no open block
   *         used.
   */
  [[nodiscard]] inline std::vector<token> layout(std::span<const token> tokens)
  {
    std::vector<token>       out;
    std::vector<std::size_t> levels        {0};
    bool                     started       {false};
    std::size_t              previous_line {0};
    position                 end_position  {0, 1, 1};

    for (const token& current : tokens) {
      if (current.kind == end_of_input) {
        end_position = current.start; // remember; emit our own terminal at the end
        continue;
      }
      if (!started || current.start.line != previous_line) {
        if (started) {
          out.push_back(token {newline, {}, current.start});
        }
        const std::size_t width {current.start.column - 1};
        if (width > levels.back()) {
          levels.push_back(width);
          out.push_back(token {indent, {}, current.start});
        }
        else {
          while (width < levels.back()) {
            levels.pop_back();
            out.push_back(token {dedent, {}, current.start});
          }
          if (width != levels.back()) {
            throw layout_error("inconsistent indentation", current.start);
          }
        }
        started = true;
      }
      out.push_back(current);
      previous_line = current.start.line;
    }

    if (started) {
      out.push_back(token {newline, {}, end_position});
    }
    while (levels.back() > 0) {
      levels.pop_back();
      out.push_back(token {dedent, {}, end_position});
    }
    out.push_back(token {end_of_input, {}, end_position});
    return out;
  }
} // namespace scilex

#endif // SCILEX_LAYOUT_HPP
