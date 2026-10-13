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
 * tokens). A token whose end the indentation decides (YAML's block scalar `|` / `>`,
 * a continued plain scalar) is the lexer's — Layout Awareness Level B, see
 * \ref rule_indentation — and spans its lines; this pass reads where it starts, so
 * its body adds no structure. The same roles reach this pass through \ref scilex::layout_roles, where
 * the node past an entry (`indent=entry`, YAML's `-`) on its line opens a block at its column when it is
 * an entry itself or a value (`indent=value`, YAML's `:`) follows it there, so the compact `- key: v` and
 * `- - x` nest as PyYAML's indentation stack nests them (an INDENT before `key`, its sibling keys at that
 * column), while `- x`, `- [a]` or a node's properties alone (`- &anchor`) open none; and a deeper line
 * opens a block only as a collection. No roles (the default) is the positional pass. Two invariants
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
   * \brief How the overload of `layout()` that takes the source measures indentation.
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

    //! \brief Measures the bytes of a source between the start of the line holding an offset and it, over offsets
    //!        that mostly increase: each byte is read once from the last offset measured, so a pass over a long line
    //!        stays linear (measured from its line's start at every token, 40 000 entries on one line took 2.5 s).
    class indent_meter
    {
    public:

      //! \brief A meter over \p source, at its start.
      explicit indent_meter(std::string_view source) noexcept
        : source_ {source}
      {}

      //! \brief The indentation of the bytes before \p offset on its line.
      //! \throws std::invalid_argument When \p offset lies beyond the source.
      [[nodiscard]] indent_measure operator()(std::size_t offset)
      {
        if (offset > source_.size()) {
          throw std::invalid_argument("scilex::layout: a token's offset lies beyond the source it was given");
        }
        if (offset < at_) { // a token before the last one measured: from the start again
          at_    = 0;
          width_ = {0, 0};
        }
        for (; at_ < offset; ++at_) {
          const char c {source_[at_]};
          if (c == '\n' || c == '\f') {
            width_ = {0, 0};
          }
          else if (c == '\t') {
            width_.tab8 = ((width_.tab8 / 8) + 1) * 8;
            ++width_.tab1;
          }
          else {
            ++width_.tab8;
            ++width_.tab1;
          }
        }
        return width_;
      }

    private:

      std::string_view source_;          //!< The source measured.
      std::size_t      at_    {0};       //!< The offset measured last.
      indent_measure   width_ {0, 0};    //!< Its measure.
    };

    [[nodiscard]] inline std::vector<token> layout_pass(std::span<const token>   tokens,
                                                        const std::vector<bool>& mode_significant,
                                                        const layout_roles&      roles,
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
   * \param[in] roles The kinds of the entry and value rules (Layout Awareness Level B: a lexer's
   *        \ref lexer::layout_roles). None (the default) is the positional pass.
   * \return The layout-aware token sequence (still end-of-input-terminated).
   * \throws layout_error If a line dedents to an indentation that no open block
   *         used.
   */
  [[nodiscard]] inline std::vector<token> layout(std::span<const token>   tokens,
                                                 const std::vector<bool>& mode_significant = {},
                                                 const layout_roles&      roles            = {})
  {
    return detail::layout_pass(tokens, mode_significant, roles, nullptr, tab_policy::columns);
  }

  /*!
   * \brief Rewrites \p tokens with NEWLINE / INDENT / DEDENT inserted, measuring indentation in
   *        \p source under \p tabs.
   *
   * \param[in] tokens An end-of-input-terminated token sequence lexed from \p source.
   * \param[in] source The text \p tokens were lexed from; a token's `start.offset` indexes it.
   * \param[in] tabs   How indentation is measured (\ref tab_policy).
   * \param[in] mode_significant As in the overload without a source.
   * \param[in] roles            As in the overload without a source.
   * \return The layout-aware token sequence (still end-of-input-terminated).
   * \throws layout_error If a line dedents to an indentation that no open block used, or (under
   *         `tab_policy::python`) mixes tabs and spaces so that its level is ambiguous.
   * \throws std::invalid_argument If a token's offset lies beyond \p source.
   */
  [[nodiscard]] inline std::vector<token> layout(std::span<const token>   tokens,
                                                 std::string_view         source,
                                                 tab_policy               tabs,
                                                 const std::vector<bool>& mode_significant = {},
                                                 const layout_roles&      roles            = {})
  {
    return detail::layout_pass(tokens, mode_significant, roles, &source, tabs);
  }

  [[nodiscard]] inline std::vector<token> detail::layout_pass(std::span<const token>   tokens,
                                                              const std::vector<bool>& mode_significant,
                                                              const layout_roles&      roles,
                                                              const std::string_view*  source,
                                                              tab_policy               tabs)
  {
    const bool                          python_tabs   {source != nullptr && tabs == tab_policy::python};
    const bool                          with_roles    {!roles.entries.empty() || !roles.values.empty()};
    std::vector<token>                  out;
    std::vector<std::size_t>            levels        {0};
    std::vector<detail::indent_measure> measured      {{0, 0}}; // python_tabs only: one per level
    bool                                started       {false};
    bool                                opens         {false};  // the last significant token is an entry's
    constexpr std::size_t               none          {std::numeric_limits<std::size_t>::max()};
    std::size_t                         pending       {none};   // with roles: out's index of a node that may open
    detail::indent_measure              pending_at    {0, 0};   // that node's column
    std::size_t                         previous_line {0};      // last line a token reached
    position                            end_position  {0, 1, 1};
    detail::indent_meter                measure       {python_tabs ? *source : std::string_view {}};

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
      const bool line_start {significant && (!started || current.start.line != previous_line)};
      bool       deeper     {false}; // with roles: a line deeper than its block, which opens one only as a collection
      if (line_start) {
        if (started) {
          out.push_back(token {newline, {}, current.start});
        }
        const detail::indent_measure here  {python_tabs ? measure(current.start.offset)
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
          if (with_roles) {
            deeper     = true;
            pending_at = here;
          }
          else {
            levels.push_back(width);
            measured.push_back(here);
            out.push_back(token {indent, {}, current.start});
          }
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
      if (significant && with_roles) {
        // A node opens a block at its column once it shows itself a collection: an entry itself (`- - x`, a deeper
        // line's `- x`), or a value past it on its line (`- key: v`, `- &a key: v`, a deeper line's `key: v`). The
        // nodes that may open are the one past an entry on its line and a line deeper than its block; a scalar, a
        // flow collection or a node's properties alone (`- x`, `- [a]`, `- &a`, a value on the line below its key)
        // open none, as PyYAML's indentation stack takes them.
        const bool entry {std::ranges::find(roles.entries, current.kind) != roles.entries.end()};
        const bool value {std::ranges::find(roles.values, current.kind) != roles.values.end()};
        const bool past  {opens && !line_start};
        if (past) {
          pending_at = python_tabs ? measure(current.start.offset)
                                   : detail::indent_measure {current.start.column - 1, 0};
        }
        if ((past || deeper) && !entry) {
          pending = out.size(); // its index once pushed
        }
        else if ((past || deeper) || (pending != none && value && !line_start)) {
          // Deeper than the block it is in: a deeper line by its test above, a node past an entry by its column,
          // and a node left pending was the last to open on its line, so it lies right of any block opened there.
          const std::size_t node {past || deeper ? out.size() : pending};
          levels.push_back(pending_at.tab8);
          measured.push_back(pending_at);
          out.insert(out.begin() + static_cast<std::ptrdiff_t>(node), token {indent, {}, node < out.size() ? out[node].start : current.start});
          pending = none;
        }
        else if (line_start) {
          pending = none;
        }
        // An entry, or a value opening its line (an explicit key's `: v`), arms the node past it, as PyYAML's
        // indentation stack opens a block after each.
        opens = entry || (value && line_start);
      }
      // Every token advances this, to the line of its LAST byte: a token spanning lines (a triple-quoted
      // string) leaves the scan on its closing line, so what follows there continues that line rather
      // than opening one. An insignificant token does too: its mode was entered by a significant token
      // of the current logical line, so the bracket it closes -- and what follows it on that line --
      // still belongs to that line. A final newline in the lexeme is excluded -- a token that ends with
      // `\n` ends its line, it does not extend it.
      const std::string_view body {current.lexeme.ends_with('\n') ? current.lexeme.substr(0, current.lexeme.size() - 1)
                                                                   : current.lexeme};
      previous_line = current.start.line + static_cast<std::size_t>(std::ranges::count(body, '\n'));
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
