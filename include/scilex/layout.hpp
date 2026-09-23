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
 * Indentation width is, by default, the byte column of a line's first token (tabs
 * and spaces each count as one column, and mixed tabs/spaces are not policed). The
 * overload that also takes the source text accepts a \ref scilex::tab_policy, and
 * `tab_policy::python` measures indentation as CPython's tokenizer does and
 * refuses a line whose tabs and spaces make the comparison ambiguous.
 *
 * This pass is positional. With no significance policy it is mode-blind — every
 * token shapes indentation — which is byte-for-byte the original behaviour. A
 * per-mode significance policy (Layout Awareness Level A) lets a mode be marked
 * **insignificant**, so its tokens pass through without affecting layout: this is
 * how a multi-line flow collection (`examples/yaml.hpp`) and implicit line
 * continuation inside brackets (`examples/python.hpp`) avoid spurious structure. A
 * mode marked insignificant must be self-delimited (entered and left by its own
 * tokens). Block scalars `|` / `>` and heredocs are a deeper case (a reference
 * indent in the frame) — Layout Awareness Level B, still to come. Two invariants
 * hold: (1) an empty policy ⇒ byte-for-byte the positional pass, at zero cost; (2)
 * the **mode** is the single source of truth for the policy — there is no per-rule
 * flag (e.g. `ignore_layout`); significance is derived from the mode, never beside it.
 *
 * Input must be an end-of-input-terminated token sequence (the lexer's
 * `eof_policy::append`); the terminal \ref scilex::end_of_input is preserved.
 */
#ifndef SCILEX_LAYOUT_HPP
#define SCILEX_LAYOUT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "token.hpp"

namespace scilex {

  //! \brief Reserved kind: end of a logical line.
  inline constexpr int newline {std::numeric_limits<int>::min() + 1};
  //! \brief Reserved kind: indentation increased (start of a deeper block).
  inline constexpr int indent  {std::numeric_limits<int>::min() + 2};
  //! \brief Reserved kind: indentation decreased (end of a block).
  inline constexpr int dedent  {std::numeric_limits<int>::min() + 3};

  // The reserved kinds (this family plus end_of_input and error) must stay mutually distinct — they
  // share the low end of the int range, and a collision would make two reserved kinds indistinguishable.
  static_assert(end_of_input != newline && newline != indent && indent != dedent
                && end_of_input != error && newline != error && indent != error && dedent != error,
                "reserved token kinds must be distinct");

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
   * \brief How the overload of \ref layout that takes the source measures indentation.
   */
  enum class tab_policy : std::uint8_t
  {
    //! A tab is one column, like a space; mixed tabs and spaces are not policed (the default pass).
    columns,
    /*!
     * CPython's rule. A line's indentation is measured twice over the blanks before its first token:
     * with tabs advancing to the next multiple of 8, and with every tab counted as 1. Levels compare
     * by the first measure; when the second does not order the same way — a deeper, equal or
     * shallower line by one measure and not by the other — the line is refused with
     * "inconsistent use of tabs and spaces in indentation", the message CPython's TabError carries.
     * A form feed resets both measures, as in CPython.
     */
    python,
  };

  namespace detail {

    //! \brief A line's indentation under \ref tab_policy::python (tab stops of 8, and tabs as 1).
    struct indent_measure
    {
      std::size_t tab8; //!< Tabs advance to the next multiple of 8.
      std::size_t tab1; //!< Tabs count as one column.
    };

    //! \brief Measures the bytes of \p source between the start of the line holding \p offset and it.
    [[nodiscard]] inline indent_measure measure_indent(std::string_view source,
                                                       std::size_t      offset)
    {
      if (offset > source.size()) {
        throw std::invalid_argument("scilex::layout: a token's offset lies beyond the source it was given");
      }
      const std::size_t newline_before {source.rfind('\n', offset == 0 ? std::string_view::npos : offset - 1)};
      const std::size_t line_start     {(offset == 0 || newline_before == std::string_view::npos) ? 0
                                                                                                   : newline_before + 1};
      indent_measure width             {0, 0};
      for (const char c : source.substr(line_start, offset - line_start)) {
        if (c == '\t') {
          width.tab8 = ((width.tab8 / 8) + 1) * 8;
          ++width.tab1;
        }
        else if (c == '\f') {
          width = {0, 0};
        }
        else {
          ++width.tab8;
          ++width.tab1;
        }
      }
      return width;
    }

    [[nodiscard]] inline std::vector<token> layout_pass(std::span<const token>   tokens,
                                                        const std::vector<bool>& mode_significant,
                                                        const std::string_view*  source,
                                                        tab_policy               tabs);
  } // namespace detail

  /*!
   * \brief Rewrites \p tokens with NEWLINE / INDENT / DEDENT inserted.
   *
   * \param[in] tokens An end-of-input-terminated token sequence.
   * \param[in] mode_significant Per-mode-id significance policy (Layout Awareness
   *        Level A): index by a token's `mode_id`; `true` (or a mode-id beyond the
   *        vector) means the token shapes layout, `false` means it is passed through
   *        without affecting indentation. An **empty** vector (the default) means
   *        every token is significant — byte-for-byte the positional pass. (A
   *        `std::vector<bool>` rather than a `std::span<const bool>`: the bit-packed
   *        `vector<bool>` cannot be viewed as a contiguous span of `bool`.)
   * \return The layout-aware token sequence (still end-of-input-terminated).
   * \throws layout_error If a line dedents to an indentation that no open block
   *         used.
   */
  [[nodiscard]] inline std::vector<token> layout(std::span<const token>   tokens,
                                                 const std::vector<bool>& mode_significant = {})
  {
    return detail::layout_pass(tokens, mode_significant, nullptr, tab_policy::columns);
  }

  /*!
   * \brief Rewrites \p tokens with NEWLINE / INDENT / DEDENT inserted, measuring indentation in
   *        \p source under \p tabs.
   *
   * \param[in] tokens An end-of-input-terminated token sequence lexed from \p source.
   * \param[in] source The text \p tokens were lexed from; a token's `start.offset` indexes it.
   * \param[in] tabs   How indentation is measured (\ref tab_policy).
   * \param[in] mode_significant As in the overload without a source.
   * \return The layout-aware token sequence (still end-of-input-terminated).
   * \throws layout_error If a line dedents to an indentation that no open block used, or (under
   *         `tab_policy::python`) mixes tabs and spaces so that its level is ambiguous.
   * \throws std::invalid_argument If a token's offset lies beyond \p source.
   */
  [[nodiscard]] inline std::vector<token> layout(std::span<const token>   tokens,
                                                 std::string_view         source,
                                                 tab_policy               tabs,
                                                 const std::vector<bool>& mode_significant = {})
  {
    return detail::layout_pass(tokens, mode_significant, &source, tabs);
  }

  [[nodiscard]] inline std::vector<token> detail::layout_pass(std::span<const token>   tokens,
                                                              const std::vector<bool>& mode_significant,
                                                              const std::string_view*  source,
                                                              tab_policy               tabs)
  {
    const bool                          python_tabs   {source != nullptr && tabs == tab_policy::python};
    std::vector<token>                  out;
    std::vector<std::size_t>            levels        {0};
    std::vector<detail::indent_measure> measured      {{0, 0}}; // python_tabs only: one per level
    bool                                started       {false};
    std::size_t                         previous_line {0};      // last *significant* line seen
    position                            end_position  {0, 1, 1};

    for (const token& current : tokens) {
      if (current.kind == end_of_input) {
        end_position = current.start; // remember; emit our own terminal at the end
        continue;
      }
      // A token shapes layout unless its mode is marked insignificant. An empty
      // policy makes every token significant — identical to the positional pass.
      const bool significant {mode_significant.empty()
                              || current.mode_id >= mode_significant.size()
                              || mode_significant[current.mode_id]};
      if (significant && (!started || current.start.line != previous_line)) {
        if (started) {
          out.push_back(token {newline, {}, current.start});
        }
        const detail::indent_measure here  {python_tabs ? detail::measure_indent(*source, current.start.offset)
                                                        : detail::indent_measure {current.start.column - 1, 0}};
        const std::size_t            width {here.tab8};
        // Under python_tabs the second measure (tabs as 1) must agree with the first at every
        // comparison, in CPython's order: deeper by both, level found by the first then equal by the
        // second. Anywhere it does not, the line's level depends on the tab width.
        constexpr const char* mixed {"inconsistent use of tabs and spaces in indentation"};
        if (width > levels.back()) {
          if (python_tabs && here.tab1 <= measured.back().tab1) {
            throw layout_error(mixed, current.start);
          }
          levels.push_back(width);
          measured.push_back(here);
          out.push_back(token {indent, {}, current.start});
        }
        else {
          while (width < levels.back()) {
            levels.pop_back();
            measured.pop_back();
            out.push_back(token {dedent, {}, current.start});
          }
          if (width != levels.back()) {
            throw layout_error("inconsistent indentation", current.start);
          }
          if (python_tabs && here.tab1 != measured.back().tab1) {
            throw layout_error(mixed, current.start);
          }
        }
        started = true;
      }
      if (significant) {
        // Only significant tokens advance this, and they advance it to the line of their LAST byte: a
        // token spanning lines (a triple-quoted string) leaves the scan on its closing line, so what
        // follows there continues that line rather than opening one. A final newline in the lexeme is
        // excluded -- a token that ends with `\n` ends its line, it does not extend it.
        const std::string_view body {current.lexeme.ends_with('\n') ? current.lexeme.substr(0, current.lexeme.size() - 1)
                                                                     : current.lexeme};
        previous_line = current.start.line + static_cast<std::size_t>(std::ranges::count(body, '\n'));
      }
      out.push_back(current); // every token is kept, significant or not
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
