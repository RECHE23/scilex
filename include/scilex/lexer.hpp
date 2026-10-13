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
#include <limits>
#include <map>
#include <numeric>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <real/dfa.hpp>
#include <real/real.hpp>

#include "token.hpp"

namespace scilex {

  class token_iterator;
  class token_range;
  class token_stream;

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
   * \brief A rule's part in the indentation the lexer tracks, Layout Awareness Level B (see
   *        \ref rule_indentation). The roles are those of a block language whose collections open at a column:
   *        YAML's sequence entries, mapping keys and values, and document markers.
   */
  enum class indent_role : std::uint8_t
  {
    none,  //!< No part (the default): the token leaves the indentation as it is.
    entry, //!< Opens a block at its own column (YAML's `-` and `?`).
    key,   //!< May be a key: its column is kept if it is the line's first candidate since an entry or a value.
    value, //!< Opens a block at the kept key's column (YAML's `:`).
    reset, //!< Closes every block (YAML's `---` and `...`).
  };

  /*!
   * \brief How a token extends over the lines below its own, bounded by the indentation the lexer tracks
   *        (see \ref rule_indentation).
   */
  enum class extension : std::uint8_t
  {
    none,  //!< The token is its match (the default).
    block, //!< A body indented past the enclosing block: YAML's `|` and `>` scalars.
    lines, //!< Lines indented past the enclosing block, each read by the rule's `continuation` (YAML's plain
           //!< scalar continued on the next line).
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
   * `\d` and `\s` expand and stay on the DFA. BENCHMARKS.md measures the cost per grammar: the
   * `python-unicode` grammar, whose identifier rule stays on Pike, keeps a smaller part of the DFA's
   * gain than `python`, whose rules all take the DFA — the Unicode identifier costs part of the fast path.
   *
   * ## The previous token {#rule_previous_token}
   *
   * \ref after and \ref not_after make a rule depend on the token emitted before it, in any mode (a skip
   * rule emits none; an error run counts, as \ref scilex::error): with `after`, the rule is active only
   * right after a token of one of those kinds; with `not_after`, everywhere else. Before the first token
   * the previous kind is \ref scilex::end_of_input. A rule has at most one of the two. This is how a `/`
   * after an operand is a division and anywhere else opens a regular expression literal, without a mode
   * for every context. The lexer gives a mode one DFA per distinct set of active rules, so the choice
   * costs nothing per token.
   *
   * ## Indentation {#rule_indentation}
   *
   * \ref indent and \ref extend (Layout Awareness Level B) let a token's extent depend on the indentation of
   * the block it belongs to, which no pattern knows. The lexer keeps a stack of open blocks' columns, as
   * YAML's scanner does, and updates it at each emitted token of a rule with a role or an extension, in a
   * significant mode (a mode named insignificant, such as YAML's flow collections, takes no part): first the
   * blocks deeper than the token's column close (all of them for \ref indent_role::reset), then an
   * \ref indent_role::entry opens a block at its column, an \ref indent_role::key is kept as the line's key
   * candidate, and an \ref indent_role::value opens a block at that candidate's column. The innermost open
   * block is the reference an extension reads:
   * - \ref extension::block takes the rest of its line, then every line that is blank or indented at least
   *   as far as the body: the column a digit 1–9 in the match gives past the reference, else that of its first
   *   line that is not blank (at least one past the reference). It ends where a line less indented begins.
   * - \ref extension::lines, when only blanks follow the match on its line, takes the next line that is not
   *   blank if it is indented past the reference and \ref continuation matches at its first non-blank, up to
   *   that match's end, and so on; a document marker (`---`, `...`) at column 0 ends it.
   * Columns are counted in the lexer's unit; an indentation counts spaces. A token so extended spans lines;
   * the layout pass reads where it starts, so its body adds no structure.
   *
   * If your identifiers are ASCII by specification (JSON, SQL, C), pin `(?a)` inline in the pattern
   * (or pass `real::flags::ascii`) to keep `\w \d \s \b` ASCII and small, DFA-representable, and fast —
   * this is what the `examples/` grammars do. If you want Unicode identifiers, write `\w+` and accept
   * the general-engine floor. The two spellings tokenize the same ASCII input identically; they differ
   * only on non-ASCII input and on whether the mode can be a DFA.
   */
  struct rule
  {
    int                        kind;                          //!< Kind assigned to tokens this rule produces.
    real::regex                pattern;                       //!< The recognizer (a linear-time REAL regex; its flags are the author's — see above).
    bool                       skip      {false};             //!< If true, matches are consumed but not emitted.
    indent_role                indent    {indent_role::none}; //!< Its part in the indentation (\ref rule_indentation).
    extension                  extend    {extension::none};   //!< How it extends below its line (\ref rule_indentation).
    std::vector<std::string>   in_mode   {};                  //!< Modes this rule is active in; empty ⇒ {"default"}.
    std::optional<mode_action> action    {};                  //!< Mode transition fired when this rule wins.
    //! Active only right after a token of one of these kinds (see \ref rule_previous_token).
    std::vector<int>           after     {};
    //! Active except right after a token of one of these kinds (see \ref rule_previous_token).
    std::vector<int>           not_after {};
    //! What each line an \ref extension::lines token continues on must hold at its first non-blank (matched
    //! within the line); empty ⇒ \ref pattern.
    std::optional<real::regex> continuation {};
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
     * \brief Builds the error a stream raises after deciding tokens in the same call.
     * \param[in] message Human-readable cause.
     * \param[in] where   Position of the byte that no rule could match.
     * \param[in] decided The tokens the call decided before it, in order.
     */
    lex_error(const std::string& message,
              position           where,
              std::vector<token> decided)
      : std::runtime_error(message),
        where_(where),
        decided_(std::move(decided))
    {}

    /*!
     * \brief Returns the position of the unmatched byte.
     */
    [[nodiscard]] position where() const noexcept
    {
      return where_;
    }

    /*!
     * \brief The tokens a \ref token_stream call decided before the error, in order: with the tokens its
     *        earlier calls returned, every token before the error, wherever the text was cut. Empty from
     *        \ref lexer::tokenize.
     * \return The tokens; a lexeme views the stream's buffer, valid until its next call.
     */
    [[nodiscard]] const std::vector<token>& decided() const noexcept
    {
      return decided_;
    }

  private:

    position           where_;   //!< Where tokenization failed.
    std::vector<token> decided_; //!< Tokens a stream call decided before the error.
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

  //! \brief The open blocks of a scan (\ref rule_indentation), one per scan as its mode stack is.
  struct indent_state
  {
    std::vector<std::ptrdiff_t> levels {-1};   //!< The open blocks' columns (0-based), innermost last, above a −1.
    std::size_t                 line   {0};    //!< The line of the last token that took part.
    std::ptrdiff_t              key    {-1};   //!< That line's key candidate's column, −1 for none.
    bool                        open   {true}; //!< Whether that line's next key candidate is kept.
  };

  //! \brief Whether a token of \p r takes part in the indentation (a role or an extension).
  [[nodiscard]] inline bool takes_part(const rule& r) noexcept
  {
    return r.indent != indent_role::none || r.extend != extension::none;
  }

  /*!
   * \brief The innermost open block once the token of \p r at \p start has updated \p state (\ref apply_indent),
   *        \p state itself left as it is: the reference the token's extension reads, decided before the
   *        update is, as a stream waiting for more text must not have made it.
   */
  [[nodiscard]] inline std::ptrdiff_t indent_reference(const rule&         r,
                                                       position            start,
                                                       const indent_state& state) noexcept
  {
    const std::ptrdiff_t column {static_cast<std::ptrdiff_t>(start.column) - 1};
    std::ptrdiff_t       top    {-1};
    if (r.indent != indent_role::reset) { // the innermost not past the column: the −1 at the bottom always is
      top = *std::ranges::find_if(std::views::reverse(state.levels), [column](std::ptrdiff_t level) { return level <= column; });
    }
    if (r.indent == indent_role::entry) {
      return std::max(top, column);
    }
    if (r.indent == indent_role::value) {
      return std::max(top, start.line == state.line ? state.key : std::ptrdiff_t {-1});
    }
    return top;
  }

  /*!
   * \brief Updates \p state for the token of \p r at \p start (\ref rule_indentation): the blocks deeper than its
   *        column close (every block for a reset), then an entry opens one at its column, a key is kept as the
   *        line's candidate if it is the first since the line began or since an entry or a value, and a value
   *        opens one at that candidate's column. Pure, so the lexer and the fuzz oracle share it.
   */
  inline void apply_indent(const rule&   r,
                           position      start,
                           indent_state& state)
  {
    const std::ptrdiff_t column {static_cast<std::ptrdiff_t>(start.column) - 1};
    if (start.line != state.line) {
      state.line = start.line;
      state.key  = -1;
      state.open = true;
    }
    if (r.indent == indent_role::reset) {
      state.levels.assign(1, -1);
    }
    while (state.levels.back() > column) { // never the −1 below the first block
      state.levels.pop_back();
    }
    if (r.indent == indent_role::entry) {
      if (state.levels.back() < column) {
        state.levels.push_back(column);
      }
      state.open = true;
    }
    else if (r.indent == indent_role::value) {
      if (state.key >= 0 && state.levels.back() < state.key) {
        state.levels.push_back(state.key);
      }
      state.key  = -1;
      state.open = true;
    }
    else if (r.indent == indent_role::key && state.open) {
      state.key  = column;
      state.open = false;
    }
  }

  //! \brief The spaces that begin the text at \p at of \p source.
  [[nodiscard]] inline std::ptrdiff_t leading_spaces(std::string_view source,
                                                     std::size_t      at) noexcept
  {
    std::size_t end {at};
    while (end < source.size() && source[end] == ' ') {
      ++end;
    }
    return static_cast<std::ptrdiff_t>(end - at);
  }

  //! \brief Whether \p source holds only bytes of \p blanks from \p first to \p last.
  [[nodiscard]] inline bool blank_between(std::string_view source,
                                          std::size_t      first,
                                          std::size_t      last,
                                          std::string_view blanks) noexcept
  {
    return source.substr(first, last - first).find_first_not_of(blanks) == std::string_view::npos;
  }

  /*!
   * \brief Where an \ref extension::block token matched up to \p end of \p source (\p match its match) ends,
   *        with \p reference the innermost open block (\ref rule_indentation); none while the text past
   *        \p source may still change it (\p final false: a stream's buffer).
   */
  [[nodiscard]] inline std::optional<std::size_t> block_extent(std::string_view source,
                                                               std::size_t      end,
                                                               std::string_view match,
                                                               std::ptrdiff_t   reference,
                                                               bool             final)
  {
    constexpr std::string_view blanks {" \r"};
    const std::size_t          header {source.find('\n', end)};
    if (header == std::string_view::npos) {
      return final ? std::optional<std::size_t> {source.size()} : std::nullopt;
    }
    const std::ptrdiff_t least   {std::max<std::ptrdiff_t>(reference + 1, 1)};
    std::ptrdiff_t       content {least};
    const auto           digit   {std::ranges::find_if(match, [](char c) { return c >= '1' && c <= '9'; })};
    if (digit != match.end()) {
      content = least + (*digit - '0') - 1;
    }
    else { // the widest of the blank lines that lead, and of the first that is not
      std::ptrdiff_t widest {0};
      for (std::size_t line {header + 1};;) {
        if (line == source.size()) {
          if (!final) {
            return std::nullopt;
          }
          break;
        }
        const std::size_t eol {source.find('\n', line)};
        if (eol == std::string_view::npos && !final) {
          return std::nullopt;
        }
        const std::size_t last {eol == std::string_view::npos ? source.size() : eol};
        widest = std::max(widest, leading_spaces(source, line));
        if (!blank_between(source, line, last, blanks) || last == source.size()) {
          break;
        }
        line = last + 1;
      }
      content = std::max(least, widest);
    }
    std::size_t line {header + 1};
    while (true) {
      if (line == source.size()) {
        return final ? std::optional<std::size_t> {line} : std::nullopt;
      }
      const std::size_t eol {source.find('\n', line)};
      if (eol == std::string_view::npos && !final) {
        return std::nullopt;
      }
      const std::size_t last  {eol == std::string_view::npos ? source.size() : eol};
      const bool        blank {blank_between(source, line, last, blanks)};
      if (blank ? eol == std::string_view::npos : leading_spaces(source, line) < content) {
        return line; // a line less indented, or blanks that end the text with no line break
      }
      line = eol == std::string_view::npos ? source.size() : eol + 1;
    }
  }

  /*!
   * \brief Where an \ref extension::lines token matched up to \p end of \p source ends, each continued line read
   *        by \p continuation (\ref rule_indentation), with \p reference the innermost open block; none while
   *        the text past \p source may still change it (\p final false).
   */
  [[nodiscard]] inline std::optional<std::size_t> lines_extent(std::string_view   source,
                                                               std::size_t        end,
                                                               const real::regex& continuation,
                                                               std::ptrdiff_t     reference,
                                                               bool               final)
  {
    constexpr std::string_view blanks {" \t\r"};
    const auto                 wait   {[final](std::size_t at) {
                                         return final ? std::optional<std::size_t> {at} : std::nullopt;
                                       }};
    for (std::size_t reached {end};;) {
      const std::size_t after {source.find_first_not_of(blanks, reached)};
      if (after == std::string_view::npos) {
        return wait(reached);
      }
      if (source[after] != '\n') {
        return reached; // more on the line: a comment or another token ends the continuation
      }
      std::size_t line {after + 1};
      std::size_t last {0};
      while (true) { // the blank lines between
        if (line == source.size()) {
          return wait(reached);
        }
        const std::size_t eol {source.find('\n', line)};
        if (eol == std::string_view::npos && !final) {
          return std::nullopt;
        }
        last = eol == std::string_view::npos ? source.size() : eol;
        if (!blank_between(source, line, last, blanks)) {
          break;
        }
        if (last == source.size()) {
          return reached;
        }
        line = last + 1;
      }
      const std::string_view text {source.substr(line, last - line)};
      if ((text.starts_with("---") || text.starts_with("...")) && (text.size() == 3 || blanks.find(text[3]) != std::string_view::npos)) {
        return reached; // a document marker at column 0
      }
      const std::size_t first {source.find_first_not_of(" \t", line)};
      const auto        found {continuation.match(source.substr(0, last), first)};
      if (leading_spaces(source, line) <= reference || !found || found.end() == first) {
        return reached;
      }
      reached = found.end();
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
   * tokenizing its own source. A \ref token_iterator and a \ref token_stream are cursors:
   * drive each one from a single thread. Rules left on Pike (\ref pike_rules) call
   * `real::regex`, whose lazy DFAs each thread leases from the regex's own pool (REAL
   * 2026.9.8 and later), so threads scanning with such a rule take no lock.
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
     * \throws std::invalid_argument If a rule's kind is reserved (\ref end_of_input to \ref scilex::error),
     *         a transition rule is malformed (empty pattern or target), or \p insignificant_modes /
     *         \p dfa_modes names an unknown mode.
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
      build_units();
      build_dfa_modes(dfa_modes, dfa);
      for (const rule& r : rules_) {
        left_context_ = std::max(left_context_, r.pattern.left_context());
      }
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
      token              next     {};
      munch_memos        memos    {memos_for(source)};
      int                previous {end_of_input};                           // the kind of the token before, for after/not_after
      indent_state       indents  {};                                       // the open blocks, for Level B
      while (scan_next(source, cursor, stack, next, memos, previous, indents)) {
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

    /*!
     * \brief A stream: text fed in pieces, the tokens returned as soon as no text still to come can change
     *        them, and exactly the tokens \ref tokenize gives the whole text (see \ref token_stream).
     * \return A stream over this lexer, which must outlive it.
     */
    [[nodiscard]] token_stream stream() const&;

    //! \brief Deleted: the stream would point into a temporary lexer.
    token_stream               stream() const&& = delete;

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
      for (std::size_t m {0}; m < mode_names_.size(); ++m) {
        if (std::ranges::any_of(units_of_mode_[m], [this](std::size_t u) { return per_mode_dfa_[u] != nullptr; })) {
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
      std::vector<bool> on_pike(rules_.size(), false); // a rule on Pike in any of the mode's units
      for (const std::size_t u : units_of_mode_[found->second]) {
        const mode_dfa* const hybrid {per_mode_dfa_[u].get()};
        for (const std::size_t idx : unit_rules_[u]) {
          if (hybrid == nullptr || hybrid->on_pike[idx]) {
            on_pike[idx] = true;
          }
        }
      }
      std::vector<std::size_t> indices;
      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        if (on_pike[idx]) {
          indices.push_back(idx);
        }
      }
      return indices;
    }

    /*!
     * \brief Where \p tok ends in \p source: the position just past its last byte, with the line and
     *        column this lexer's \ref scilex::column_unit gives it — exactly where the scan's cursor stood after
     *        the token.
     *
     * A token carries its start and its lexeme, not its end, so the token stream costs no more for the
     * callers that never ask. A zero-width token (a synthetic newline, indent or dedent from \ref layout,
     * the end-of-input token) ends where it starts.
     *
     * \param[in] source The text \p tok was lexed from; its lexeme must view into it.
     * \param[in] tok    A token this lexer produced from \p source.
     * \return The position after \p tok.
     * \throws std::invalid_argument If \p tok's lexeme is not the bytes of \p source at its start offset.
     */
    [[nodiscard]] position end_of(std::string_view source,
                                  const token&     tok) const
    {
      if (tok.lexeme.empty()) {
        return tok.start;
      }
      if (tok.start.offset > source.size() || tok.lexeme.size() > source.size() - tok.start.offset
          || tok.lexeme.data() != source.data() + tok.start.offset) {
        throw std::invalid_argument("end_of: the token's lexeme is not a view into the source at its start");
      }
      position cursor {tok.start};
      advance(source, cursor, tok.lexeme.size());
      return cursor;
    }

  private:

    friend class token_iterator;
    friend class token_stream;

    //! \brief Per mode, what the munches over one source have proved (\c real::dfa_munch_memo); one
    //!        object per tokenization or iterator, made by \ref memos_for. An unarmed memo holds
    //!        nothing, so making one per mode up front costs nothing a scan would notice; an armed
    //!        one holds up to one bit per source byte per DFA state (Q·n bits for the mode).
    using munch_memos = std::vector<real::dfa_munch_memo>;

    //! \brief The memos for one scan of \p source: one per unit (a mode, or a mode's variant for a
    //!        previous token), each unarmed.
    //! \param[in] source The text about to be scanned.
    //! \return The memos, indexed by unit.
    [[nodiscard]] munch_memos memos_for(std::string_view source) const
    {
      return munch_memos(per_mode_.size(), real::dfa_munch_memo {source.size()});
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
     * advancing \p cursor (offset, line, column) and the mode \p stack past it.
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
     * \param[in,out] previous The kind of the token before; set to the kind of the token produced.
     * \param[in,out] indents The open blocks (\ref rule_indentation).
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
                   munch_memos&        memos,
                   int&                previous,
                   indent_state&       indents) const
    {
      return scan_step(source, cursor, stack, out, memos, previous, indents, whole_text {}) == scan_status::token;
    }

    //! \brief What \ref scan_step did.
    enum class scan_status : std::uint8_t
    {
      token,   //!< Produced a token.
      end,     //!< Reached the end of the input.
      pending, //!< Stopped where more text could change what comes next (the caller restores its cursor).
    };

    //! \brief The gate of a scan over a whole text: every munch may go ahead, and the end is the end.
    struct whole_text
    {
      //! \brief Always.
      //! \return True.
      [[nodiscard]] static constexpr bool ready(std::size_t /*mode*/,
                                                std::size_t /*offset*/) noexcept
      {
        return true;
      }

      //! \brief Always.
      //! \return True.
      [[nodiscard]] static constexpr bool at_end() noexcept
      {
        return true;
      }

      //! \brief Nothing: a scan over the whole text never takes a step back.
      static constexpr void before_transition(const std::vector<frame>& /*stack*/) noexcept
      {
      }
    };

    /*!
     * \brief \ref scan_next with a gate asked before each munch: `gate.ready(mode, offset)` is false where
     *        text not yet seen could change the munch there, and `gate.at_end()` whether the end of
     *        \p source is the end of the input; `gate.before_transition(stack)` sees the stack before each
     *        mode transition. A scan over a whole text passes \ref whole_text; a stream (\ref token_stream)
     *        passes one that stops at the first munch still open, and undoes the step's transitions.
     * \tparam Gate The gate's type.
     * \param[in]     source The text available.
     * \param[in,out] cursor Current position; on \ref scan_status::pending it may have moved over skipped
     *                matches, which the caller undoes.
     * \param[in,out] stack  The mode stack (likewise).
     * \param[out]    out    The token, on \ref scan_status::token.
     * \param[in,out] memos  What earlier munches over \p source proved.
     * \param[in,out] previous The kind of the token before (\ref scilex::end_of_input before the first);
     *                         set to the kind of the token produced.
     * \param[in,out] indents The open blocks (\ref rule_indentation); left as they were when the step waits.
     * \param[in]     gate   The gate.
     * \return What the step did.
     * \throws lex_error As \ref scan_next.
     */
    template <typename Gate>
    scan_status scan_step(std::string_view    source,
                          position&           cursor,
                          std::vector<frame>& stack,
                          token&              out,
                          munch_memos&        memos,
                          int&                previous,
                          indent_state&       indents,
                          const Gate&         gate) const
    {
      while (cursor.offset < source.size()) {
        const std::size_t mode {stack.back().mode_id};
        const std::size_t unit {unit_of(mode, previous)}; // the mode's rules active after `previous`
        if (!gate.ready(unit, cursor.offset)) {
          return scan_status::pending;
        }
        const munch_result m {munch_at<may_wait<Gate>>(unit, source, cursor.offset, memos)};
        if (m.open && !gate.at_end()) {
          return scan_status::pending; // the DFA rules' munch may still change with what comes next
        }

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
          while (cursor.offset < source.size()) {
            if (!gate.ready(unit, cursor.offset)) {
              return scan_status::pending;
            }
            if (may_start(unit, static_cast<unsigned char>(source[cursor.offset]))) {
              const munch_result probe {munch_at<may_wait<Gate>>(unit, source, cursor.offset, memos)};
              if (probe.open && !gate.at_end()) {
                return scan_status::pending;
              }
              if (probe.have && probe.len > 0) {
                break; // a match of no length (a general rule's) does not end the run
              }
            }
            advance(source, cursor, 1);
          }
          if (cursor.offset == source.size() && !gate.at_end()) {
            return scan_status::pending; // the run may go on into text still to come
          }
          out      = token {scilex::error, source.substr(err_start.offset, cursor.offset - err_start.offset),
                            err_start, mode};
          previous = scilex::error;
          return scan_status::token;
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
        std::size_t       best_len {m.len};
        const position    start    {cursor};
        const rule&       won      {rules_[best_idx]};
        // Level B: a token with a role or an extension, in a significant mode, reads and updates the open
        // blocks. Its extent is decided first, the state left as it is if the text may still change it.
        const bool indented {!won.skip && takes_part(won) && (mode_significant_.empty() || mode_significant_[mode])};
        if (indented) {
          if (won.extend != extension::none) {
            const std::ptrdiff_t             reference {indent_reference(won, start, indents)};
            const std::optional<std::size_t> extent    {won.extend == extension::block
                                                          ? block_extent(source, start.offset + best_len, source.substr(start.offset, best_len),
                                                                         reference, gate.at_end())
                                                          : lines_extent(source, start.offset + best_len,
                                                                         won.continuation ? *won.continuation : won.pattern,
                                                                         reference, gate.at_end())};
            if (!extent) {
              return scan_status::pending;
            }
            best_len = *extent - start.offset;
          }
          apply_indent(won, start, indents);
        }
        advance(source, cursor, best_len);

        // Most tokens carry no action: testing it here keeps the out-of-line transition, and the
        // frame it sets up for its throws, off their path.
        if (rules_[best_idx].action) {
          gate.before_transition(stack);
          apply_transition(rules_[best_idx], start, stack); // advances, then transitions (#2 on a bad pop)
        }

        if (!rules_[best_idx].skip) {
          // Tag the token with the mode it was lexed in (captured before the
          // transition above) — Layout Awareness reads it; the scan is untouched.
          out      = token {rules_[best_idx].kind, source.substr(start.offset, best_len), start, mode};
          previous = out.kind;
          return scan_status::token;
        }
        // Skip rule: keep scanning for the next emitted token (possibly in a new mode).
      }
      if (!gate.at_end()) {
        return scan_status::pending;
      }
      if (stack.size() > 1) {
        if (errors_ == error_policy::raise) {
          throw lex_error("unterminated mode '" + mode_names_[stack.back().mode_id] + "' (entered at "
                          + position_label(stack.back().entry_pos) + ")", stack.back().entry_pos); // #3
        }
        // Recovery (error_policy::token): a mode was still pushed at end of input. Emit one zero-width
        // error token positioned at the EOF (the partial tokens already emitted stay), then unwind to
        // the root so the next call reports a clean end of input.
        out = token {scilex::error, source.substr(cursor.offset, 0), cursor, stack.back().mode_id}; // the last token
        stack.resize(1);
        return scan_status::token;
      }
      return scan_status::end;
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

    //! \brief Refuses a rule whose kind is one SciLex reserves: its tokens would read as an end of input,
    //!        a layout token or an error run.
    //! \throws std::invalid_argument On the first such rule.
    void validate_kinds() const
    {
      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        if (rules_[idx].kind <= scilex::error) {
          throw std::invalid_argument("rule " + std::to_string(idx) + ": kind " + std::to_string(rules_[idx].kind)
                                      + " is reserved (scilex::end_of_input to scilex::error)");
        }
        if (rules_[idx].skip && takes_part(rules_[idx])) {
          throw std::invalid_argument("rule " + std::to_string(idx)
                                      + ": a skip rule emits no token, so it takes no part in the indentation");
        }
      }
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
      validate_kinds();
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

    /*!
     * \brief Splits each mode by the previous token (\ref rule::after, \ref rule::not_after): the kinds
     *        are grouped by which of the mode's predicated rules they allow, and each group is a unit with
     *        its own dispatch (and DFA). The group of a kind no predicate names keeps the mode's id; the
     *        others take ids past the modes'. A mode with no predicated rule is one unit, its own.
     * \throws std::invalid_argument On a rule with both `after` and `not_after`.
     */
    void build_units()
    {
      const std::size_t modes {mode_names_.size()};
      units_of_mode_.assign(modes, {});
      unit_rules_.assign(modes, {});
      variants_.assign(modes, {});
      for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
        if (!rules_[idx].after.empty() && !rules_[idx].not_after.empty()) {
          throw std::invalid_argument("rule " + std::to_string(idx) + ": a rule has after or not_after, not both");
        }
      }
      const auto allowed {[this](std::size_t idx, int previous) {
                            const rule& r {rules_[idx]};
                            if (!r.after.empty()) {
                              return std::ranges::find(r.after, previous) != r.after.end();
                            }
                            return std::ranges::find(r.not_after, previous) == r.not_after.end();
                          }};
      for (std::size_t m {0}; m < modes; ++m) {
        std::vector<std::size_t> active;
        std::vector<int>         named; // every kind a predicate of this mode names
        for (std::size_t idx {0}; idx < rules_.size(); ++idx) {
          if (rule_active_in_mode(idx, m)) {
            active.push_back(idx);
            named.insert(named.end(), rules_[idx].after.begin(), rules_[idx].after.end());
            named.insert(named.end(), rules_[idx].not_after.begin(), rules_[idx].not_after.end());
          }
        }
        std::ranges::sort(named);
        named.erase(std::ranges::unique(named).begin(), named.end());
        // A kind no predicate names stands for every such kind: they all share the mode's own unit.
        int unnamed {std::numeric_limits<int>::min()};
        for (const int kind : named) { // ascending and distinct: the first gap
          if (kind != unnamed) {
            break;
          }
          ++unnamed;
        }
        const auto rules_after {[&](int previous) {
                                  std::vector<std::size_t> out;
                                  std::ranges::copy_if(active, std::back_inserter(out),
                                                       [&](std::size_t idx) { return allowed(idx, previous); });
                                  return out;
                                }};
        const std::vector<std::size_t> own {rules_after(unnamed)};
        unit_rules_[m] = own;
        units_of_mode_[m].push_back(m);
        if (own.size() != active.size()) {
          per_mode_[m] = dispatch {};
          for (const std::size_t idx : own) {
            add_to_mode(m, idx);
          }
        }
        for (const int kind : named) {
          std::vector<std::size_t> rules_for {rules_after(kind)};
          const auto               existing  {std::ranges::find_if(units_of_mode_[m], [&](std::size_t u) { return unit_rules_[u] == rules_for; })};
          std::size_t              unit      {};
          if (existing != units_of_mode_[m].end()) {
            unit = *existing;
          }
          else {
            unit = per_mode_.size();
            per_mode_.emplace_back();
            for (const std::size_t idx : rules_for) {
              add_to_mode(unit, idx);
            }
            unit_rules_.push_back(std::move(rules_for));
            units_of_mode_[m].push_back(unit);
          }
          // Never the mode's own unit: a predicate of this mode names `kind`, so treats it unlike an unnamed one.
          variants_[m].emplace_back(kind, unit);
        }
      }
    }

    //! \brief The unit that munches in \p mode after a token of kind \p previous (\ref build_units).
    [[nodiscard]] std::size_t unit_of(std::size_t mode,
                                      int         previous) const
    {
      const std::vector<std::pair<int, std::size_t>>& variants {variants_[mode]};
      if (variants.empty()) [[likely]] {
        return mode;
      }
      const auto found {std::ranges::lower_bound(variants, previous, {}, &std::pair<int, std::size_t>::first)};
      return found != variants.end() && found->first == previous ? found->second : mode;
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
      bool        open {false}; //!< The mode's DFA was alive at the end of the source: text after it may change its share.
    };

    //! \brief The winning munch in \p mode at \p offset of \p source (\p memos: see \ref munch_memos),
    //!        dispatching to the mode's DFA when it has one, else the Pike + first-byte munch. The
    //!        single match primitive both the forward scan and the error-recovery probe call, so the
    //!        two never diverge on which rule wins.
    //! \tparam Wait `std::true_type` when a scan may wait for more text (a stream), so the munch must tell
    //!        whether more text may change it (munch_result::open); `std::false_type` over a whole text.
    template <typename Wait>
    munch_result munch_at(std::size_t      mode,
                          std::string_view source,
                          std::size_t      offset,
                          munch_memos&     memos) const
    {
      const mode_dfa* const hybrid {per_mode_dfa_[mode].get()};
      // A mode wholly on its DFA is the per-token case: it stays free of the Pike path's frame, which
      // an out-of-line call keeps for the modes that need it.
      if (hybrid == nullptr || hybrid->any_on_pike) [[unlikely]] {
        return munch_with_pike(mode, source, offset, memos, hybrid, Wait {});
      }
      return dfa_munch(*hybrid, source, offset, memos[mode], Wait {});
    }

    //! \brief Whether a scan through \p Gate may stop to wait for more text: every gate but a whole text's.
    template <typename Gate>
    using may_wait = std::bool_constant<!std::is_same_v<Gate, whole_text>>;

    //! \brief The DFA's share of \ref munch_at: its rules' longest non-empty match, else the empty
    //!        match of a nullable DFA rule (\ref mode_dfa::empty_winner), else nothing.
    //! \param[in]     hybrid The mode's DFA.
    //! \param[in]     source The whole source.
    //! \param[in]     offset Where the munch starts.
    //! \param[in,out] memo   The mode's walk memo over \p source.
    //! \return The DFA rules' winning munch, as a global rule index, and whether more text may change it.
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline))
#endif
    static munch_result dfa_munch(const mode_dfa&       hybrid,
                                  std::string_view      source,
                                  std::size_t           offset,
                                  real::dfa_munch_memo& memo,
                                  std::true_type /*may_wait*/)
    {
      // The walk is memoized over the whole source (real::dfa_munch_memo): a state a walk proved leads
      // to no accept stops every later walk that reaches it, so the DFA's share of a tokenization is
      // linear in the source rather than quadratic. Each rule's match() is its longest, so the DFA's
      // longest is theirs; a DFA rule's empty match, which the DFA never reports, competes through
      // empty_winner.
      const real::dfa_munch munch {hybrid.dfa.munch(source, offset, memo)};
      if (munch.match) {
        return munch_result {.have = true, .idx = hybrid.to_global[munch.match->rule_index], .len = munch.match->length,
                             .open = munch.more_text_may_change};
      }
      if (hybrid.empty_winner) {
        return munch_result {.have = true, .idx = *hybrid.empty_winner, .len = 0, .open = munch.more_text_may_change};
      }
      return munch_result {.open = munch.more_text_may_change};
    }

    //! \brief \ref dfa_munch over a whole text, where no text follows: REAL's `match`, which skips telling
    //!        whether more text may change the answer (that costs 5–17 % of a scan on the DFA).
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline))
#endif
    static munch_result dfa_munch(const mode_dfa&       hybrid,
                                  std::string_view      source,
                                  std::size_t           offset,
                                  real::dfa_munch_memo& memo,
                                  std::false_type /*may_wait*/)
    {
      if (const std::optional<real::dfa_match> matched {hybrid.dfa.match(source, offset, memo)}) {
        return munch_result {.have = true, .idx = hybrid.to_global[matched->rule_index], .len = matched->length};
      }
      if (hybrid.empty_winner) {
        return munch_result {.have = true, .idx = *hybrid.empty_winner, .len = 0};
      }
      return munch_result {};
    }

    //! \brief \ref munch_at for a mode with rules on Pike: all of them when it has no DFA (\p hybrid
    //!        null), else the DFA's share merged with the rest — the longest match wins and the lowest
    //!        index breaks a tie, as in the per-rule munch over the whole mode.
    //! \param[in]     mode   The active mode.
    //! \param[in]     source The whole source.
    //! \param[in]     offset Where the munch starts.
    //! \param[in,out] memos  The walk memos over \p source.
    //! \param[in]     hybrid The mode's DFA, or null.
    //! \param[in]     wait   Whether the scan may wait for more text (see \ref munch_at).
    //! \return The winning munch.
    template <typename Wait>
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    munch_result munch_with_pike(std::size_t      mode,
                                 std::string_view source,
                                 std::size_t      offset,
                                 munch_memos&     memos,
                                 const mode_dfa*  hybrid,
                                 Wait             wait) const
    {
      const unsigned char first {static_cast<unsigned char>(source[offset])};
      if (hybrid == nullptr) {
        return pike_munch_in_mode(mode, source, offset, first, nullptr);
      }
      munch_result       best         {dfa_munch(*hybrid, source, offset, memos[mode], wait)};
      const bool         dfa_open     {best.open};
      const munch_result rest_of_mode {pike_munch_in_mode(mode, source, offset, first, &hybrid->on_pike)};
      if (rest_of_mode.have
          && (!best.have || rest_of_mode.len > best.len
              || (rest_of_mode.len == best.len && rest_of_mode.idx < best.idx))) {
        best = rest_of_mode;
      }
      best.open = dfa_open; // the Pike rules answer to the gate (can_extend), the DFA's through this
      return best;
    }

    //! \brief O(1) pre-filter for error recovery: can a rule in \p mode begin with \p byte?
    //!        A false is conclusive (no rule can match, so the byte is error text); a true still needs a
    //!        full \ref munch_at to confirm a real match. This is what skips the bulk of noise cheaply.
    //!
    //! A mode's general rules (no first-byte set: nullable ones) can match anywhere: `(?m)^[ \t]*`
    //! matches nothing in the middle of a line, so a run starts there, and the blanks that begin the
    //! next line. A mode that has one is asked at every byte.
    bool may_start(std::size_t   mode,
                   unsigned char byte) const
    {
      return !per_mode_[mode].first_byte_index[byte].empty() || !per_mode_[mode].general.empty();
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

    //! \brief The per-rule Pike + first-byte-dispatch munch in \p mode at \p offset of \p source
    //!        (\p lead is the byte there). Each rule matches anchored at \p offset in the whole
    //!        source, so a lookbehind, `\b` or a line start reads the text before the token as it
    //!        does in `match(text, pos)`. Zero allocation; shared by scan_next's Pike branch. A
    //!        zero-length match is reported as a candidate (it wins only when nothing matches >0);
    //!        scan_next's shared guard turns such a win into a lexical error.
    munch_result pike_munch_in_mode(std::size_t              mode,
                                    std::string_view         source,
                                    std::size_t              offset,
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
                              const auto matched {rules_[idx].pattern.match(source, offset)};
                              // A zero-length match participates (it can only win when no rule
                              // matches >0 here); the shared guard in scan_next turns that win
                              // into a lexical error rather than a stalled scan. Maximal munch
                              // still prefers any longer non-empty match.
                              const std::size_t length {matched ? matched.end() - offset : 0};
                              if (matched
                                  && (!have || length > best_len
                                      || (length == best_len && idx < best_idx))) {
                                best_len = length;
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
    //! lookaround, a class too wide) raises \c real::dfa_error and fails too, and a rule that reads the
    //! text before its token (a leading `^` or `\A`) stays off it. Every failing rule stays
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
        // A DFA matches from the token's start, where `^`, `\A` or a lookbehind would read the text
        // before it: Pike matches in the whole source and answers differently past the first token.
        if (faithful && rules_[g].pattern.left_context() == 0) {
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
      per_mode_dfa_.assign(per_mode_.size(), nullptr);
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
        for (const std::size_t unit : units_of_mode_[mode]) {
          if (auto built {try_build_mode_dfa(unit_rules_[unit])}) {
            per_mode_dfa_[unit] = std::make_shared<const mode_dfa>(std::move(*built));
          }
        }
      }
    }

    //! \brief Per-mode dispatch index: the first-byte buckets scoped to one mode.
    struct dispatch
    {
      std::array<std::vector<std::size_t>, 256> first_byte_index; //!< Rule indices by leading byte.
      std::vector<std::size_t>                  general;          //!< Nullable rules (tried everywhere).
    };

    std::vector<rule>                            rules_;                     //!< The ordered token rules.
    error_policy                                 errors_;                    //!< What to do at an unmatched byte.
    scilex::column_unit                          columns_;                   //!< The unit position::column is counted in.
    std::vector<std::string>                     mode_names_;                //!< Mode id -> name ("default" is id 0).
    std::map<std::string, std::size_t>           mode_id_;                   //!< Mode name -> id.
    std::vector<dispatch>                        per_mode_;                  //!< Dispatch index, one per unit (a mode's id is its own unit).
    std::vector<std::shared_ptr<const mode_dfa>> per_mode_dfa_;              //!< Per-unit DFA fast path (nullptr = Pike).
    std::vector<std::vector<std::size_t>>        units_of_mode_;             //!< Mode id -> its units (its own id first).
    std::vector<std::vector<std::size_t>>        unit_rules_;                //!< Unit -> its active rules, ascending.
    //! Mode id -> (previous kind, unit) for the kinds whose unit is not the mode's own, by kind.
    std::vector<std::vector<std::pair<int, std::size_t>>> variants_;
    std::vector<bool>                                     mode_significant_; //!< Layout policy (empty = all significant).
    std::size_t                                           left_context_ {0}; //!< Bytes before a token start any rule may read.
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
    int                previous_ {end_of_input};                                        //!< The kind of the token before.
    indent_state       indents_  {};                                                    //!< The open blocks (Level B).

    //! \brief Produces the next token, or marks the iterator exhausted.
    void advance()
    {
      if (done_) {
        return;
      }
      if (owner_->scan_next(source_, cursor_, stack_, current_, memos_, previous_, indents_)) {
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

  /*!
   * \brief Text fed in pieces, lexed as it arrives, with exactly the tokens \ref lexer::tokenize gives the
   *        whole text: the same munches, modes, positions and errors.
   *
   * A token is returned only once no text still to come can change it, which is not "every token but the
   * last": a rule `a[^z]*z` gives way to one token the moment a `z` arrives, however many tokens other rules
   * made of the text before it. So before each munch the stream asks every rule of the active mode whether
   * more text could change its match there (`real::regex::can_extend`), and stops at the first munch
   * where one could. \ref finish lexes the rest with the end of the text as the end.
   *
   * The stream keeps the text from the first token it has not returned, and before it the few bytes a rule
   * may still read there (a lookbehind's width, one code point for `\b`; `real::regex::left_context`); the
   * rest of what it returned is dropped at the next \ref feed. So memory follows the longest token, not the
   * input -- and a token's lexeme views that buffer: it is valid until the next \ref feed or \ref finish.
   * Positions are in the whole text.
   *
   * \code
   * scilex::token_stream in {lex.stream()};
   * while (read(chunk)) {
   *   for (const scilex::token& t : in.feed(chunk)) { use(t); }
   * }
   * for (const scilex::token& t : in.finish()) { use(t); }
   * \endcode
   */
  class token_stream
  {
  public:

    /*!
     * \brief A stream over \p owner, at the start of the text.
     * \param[in] owner The lexer (must outlive the stream).
     */
    explicit token_stream(const lexer& owner)
      : owner_(&owner),
        stack_ {frame {.mode_id = 0, .entry_pos = cursor_}}
    {}

    /*!
     * \brief Appends \p chunk and returns the tokens no text still to come can change.
     * \param[in] chunk The next piece of the text.
     * \return The tokens, in order; their lexemes are valid until the next call.
     * \throws lex_error Where \ref lexer::tokenize would throw on the whole text, once the text decides it,
     *         carrying the tokens this call decided before it (\ref lex_error::decided).
     * \throws std::logic_error After \ref finish.
     */
    [[nodiscard]] std::vector<token> feed(std::string_view chunk)
    {
      if (finished_) {
        throw std::logic_error("token_stream::feed after finish");
      }
      drop_returned();
      buffer_.append(tail_);
      tail_.clear();
      buffer_.append(chunk);
      hold_incomplete_tail();
      return collect(false);
    }

    /*!
     * \brief Ends the text and returns the tokens that remain.
     * \return The tokens, in order; their lexemes are valid until the stream is destroyed.
     * \throws lex_error Where \ref lexer::tokenize would throw on the whole text, carrying the tokens this
     *         call decided before it (\ref lex_error::decided).
     * \throws std::logic_error When called twice.
     */
    [[nodiscard]] std::vector<token> finish()
    {
      if (finished_) {
        throw std::logic_error("token_stream::finish called twice");
      }
      drop_returned();
      buffer_.append(tail_);
      tail_.clear();
      finished_ = true;
      return collect(true);
    }

    //! \brief Bytes the stream holds: from the first token not yet returned to the end of what was fed.
    //! \return The count.
    [[nodiscard]] std::size_t buffered() const noexcept
    {
      return buffer_.size() - cursor_.offset + tail_.size();
    }

  private:

    //! \brief The stack as a transition found it: its depth and its top frame. A transition changes the
    //!        top frame only (push and pop at the top, set in place), so these two undo it.
    struct undo_entry
    {
      std::size_t depth; //!< The stack's size before the transition.
      frame       top;   //!< Its top frame before the transition.
    };

    //! \brief The gate a stream scans with: a munch goes ahead only where no rule of the mode can extend.
    struct gate
    {
      const token_stream*      stream; //!< The stream (its lexer and buffer).
      bool                     final;  //!< The end of the buffer is the end of the text.
      std::vector<undo_entry>* undo;   //!< The step's transitions, latest last.

      //! \brief Whether the munch at \p offset in unit \p mode is final.
      //! \param[in] mode   The active unit (the mode, or its variant for the previous token).
      //! \param[in] offset Where the munch starts, in the buffer.
      //! \return True when no text still to come can change it.
      [[nodiscard]] bool ready(std::size_t mode,
                               std::size_t offset) const
      {
        if (final) {
          return true;
        }
        // A rule that cannot start with the byte at offset (the scan asks only within the buffer), and
        // cannot match empty, cannot match there whatever follows: only the mode's rules filed under that
        // byte, and its nullable ones, are asked.
        const std::string& text {stream->buffer_};
        const lexer&       lex     {*stream->owner_};
        const auto&        by_byte {lex.per_mode_[mode]};
        // A rule on the mode's DFA answers through the munch (munch_result::open), at no further cost;
        // can_extend is for the rules the DFA cannot take.
        const auto* const hybrid  {lex.per_mode_dfa_[mode].get()};
        const auto        extends {[&](const std::vector<std::size_t>& rules) {
                                     return std::ranges::any_of(rules, [&](std::size_t idx) {
                                                                  return (hybrid == nullptr || hybrid->on_pike[idx])
                                                                         && lex.rules_[idx].pattern.can_extend(text, offset);
                                                                });
                                   }};
        return !extends(by_byte.first_byte_index[static_cast<unsigned char>(text[offset])])
               && !extends(by_byte.general);
      }

      //! \brief Whether the end of the buffer is the end of the text.
      //! \return True after \ref finish.
      [[nodiscard]] bool at_end() const noexcept
      {
        return final;
      }

      //! \brief Records what a transition is about to change, for \ref collect to undo if the step waits.
      //! \param[in] stack The mode stack before the transition.
      void before_transition(const std::vector<frame>& stack) const
      {
        undo->push_back(undo_entry {.depth = stack.size(), .top = stack.back()});
      }
    };

    //! \brief Lexes the buffer from the cursor as far as the gate allows.
    //! \param[in] final Whether the end of the buffer is the end of the text.
    //! \return The tokens produced, their positions in the whole text.
    std::vector<token> collect(bool final)
    {
      std::vector<token>  out;
      lexer::munch_memos  memos {owner_->memos_for(buffer_)};
      const gate          g {.stream = this, .final = final, .undo = &undo_};
      token               next {};
      while (true) {
        const position     saved_cursor {cursor_};
        lexer::scan_status status       {lexer::scan_status::end};
        undo_.clear();
        try {
          status = owner_->scan_step(buffer_, cursor_, stack_, next, memos, previous_, indents_, g);
        }
        catch (const lex_error& e) {
          finished_ = true;
          throw lex_error(e.what(), in_text(e.where()), std::move(out)); // the tokens before it, too
        }
        if (status == lexer::scan_status::pending) {
          // Undo any skipped matches the step went over, and their transitions.
          cursor_ = saved_cursor;
          for (const undo_entry& e : std::views::reverse(undo_)) {
            stack_.resize(e.depth - 1);
            stack_.push_back(e.top);
          }
          return out;
        }
        if (status == lexer::scan_status::end) {
          return out;
        }
        next.start = in_text(next.start);
        out.push_back(next);
      }
    }

    //! \brief Drops the text before the cursor -- returned, and no longer viewed by anything valid -- but
    //!        the bytes a rule may still read before the next token: a lookbehind, `\b` or `\A` there
    //!        answers as on the whole text (`real::regex::left_context`).
    void drop_returned()
    {
      // A column in code points or UTF-16 units reads up to 3 bytes back to find a code point's lead.
      const std::size_t keep {owner_->columns_ == column_unit::bytes ? owner_->left_context_
                                                                     : std::max<std::size_t>(owner_->left_context_, 3)};
      const std::size_t k    {cursor_.offset - std::min(cursor_.offset, keep)};
      buffer_.erase(0, k);
      base_          += k;
      cursor_.offset -= k;
      for (frame& f : stack_) {
        f.entry_pos.offset -= k; // modular: a mode entered before the dropped text keeps a consistent offset
      }
    }

    //! \brief Holds back the bytes that end the buffer when they begin a UTF-8 code point the text has
    //!        not finished: a column in code points or UTF-16 units scores a lead and its continuations
    //!        by the whole sequence, so none of them is lexed until it is all there (or the text ends).
    void hold_incomplete_tail()
    {
      if (owner_->columns_ == column_unit::bytes) {
        return;
      }
      const std::size_t end {buffer_.size()};
      for (std::size_t back {1}; back <= std::min<std::size_t>(3, end - cursor_.offset); ++back) {
        const auto byte {static_cast<unsigned char>(buffer_[end - back])};
        if ((byte & 0xC0U) == 0x80U) {
          continue; // a continuation: its lead is further back
        }
        const std::size_t length {byte >= 0xF0U ? 4U : byte >= 0xE0U ? 3U : byte >= 0xC0U ? 2U : 1U};
        if (length > back) {
          tail_.assign(buffer_, end - back);
          buffer_.resize(end - back);
        }
        return;
      }
    }

    //! \brief \p where, a position in the buffer, as a position in the whole text.
    //! \param[in] where A position whose offset is relative to the buffer.
    //! \return The same position, offset from the start of the text.
    [[nodiscard]] position in_text(position where) const noexcept
    {
      where.offset += base_;
      return where;
    }

    const lexer     *       owner_;                   //!< The lexer.
    std::string             buffer_;                  //!< The text from the first token not yet returned.
    std::size_t             base_     {0};            //!< Offset of the buffer's first byte in the whole text.
    position                cursor_   {0, 1, 1};      //!< Where lexing resumes, in the buffer (line and column in the text).
    std::vector<frame>      stack_;                   //!< The mode stack.
    std::vector<undo_entry> undo_;                    //!< The transitions of the step under way (see \ref gate::undo).
    std::string             tail_;                    //!< Fed bytes held back: a code point not yet complete.
    bool                    finished_ {false};        //!< \ref finish was called, or a lex_error ended the text.
    int                     previous_ {end_of_input}; //!< The kind of the token before the cursor.
    indent_state            indents_  {};             //!< The open blocks before the cursor (Level B).
  };

  inline token_stream lexer::stream() const&
  {
    return token_stream(*this);
  }
} // namespace scilex

#endif // SCILEX_LEXER_HPP
