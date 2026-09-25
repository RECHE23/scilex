/*!
 * \file grammar.hpp
 * \brief The `.lex` grammar format: rules written as text, parsed into \ref scilex::rule lists.
 *
 * Optional, and not included by `scilex.hpp`: the lexer itself takes C++ rule lists, and a program
 * that builds its rules in code needs none of this. It exists so that a grammar kept in a file — the
 * CLI's input, a user-supplied grammar, a Python caller's text — goes through one tested parser.
 *
 * One rule per line: a name, a tab, the pattern, then optionally a tab and space-separated options.
 * Blank lines and lines whose first non-blank character is `#` are ignored; a trailing `\r` is
 * dropped. The options are:
 *
 * - `skip` — matches are consumed but not emitted;
 * - `in=m1,m2` — the modes the rule is active in (default: `default` only);
 * - `push=m`, `set=m`, `pop` — the mode transition fired when the rule wins (at most one).
 *
 * A rule's kind is its 0-based position among the rules; \ref scilex::grammar::names maps it back.
 *
 * \code
 * WS       \s+        skip
 * STRING   "          push=str
 * TEXT     [^"\\]+    in=str
 * ESCAPE   \\.        in=str
 * END      "          in=str pop
 * NUMBER   [0-9]+
 * \endcode
 */
#ifndef SCILEX_GRAMMAR_HPP
#define SCILEX_GRAMMAR_HPP

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <real/real.hpp>

#include "lexer.hpp"

namespace scilex {

  /*!
   * \brief Thrown for a malformed `.lex` grammar: where (origin, 1-based line, and 1-based byte column
   *        in the line when the cause has one) and why.
   *
   * `what()` reads `origin:line[:column]: cause`, the form compilers and editors recognize, or
   * `origin: cause` for an error with no line (an unreadable file, a grammar with no rules).
   */
  class grammar_error : public std::runtime_error
  {
  public:

    /*!
     * \brief Builds the error.
     * \param[in] origin Where the grammar came from (a path, or a label such as `<string>`).
     * \param[in] line   The 1-based line of the offending rule.
     * \param[in] column The 1-based byte column in that line, or 0 when the cause has none.
     * \param[in] cause  What is wrong, without the location.
     */
    grammar_error(const std::string& origin,
                  std::size_t        line,
                  std::size_t        column,
                  const std::string& cause)
      : std::runtime_error(located(origin, line, column, cause)),
        line_(line),
        column_(column),
        cause_(cause)
    {}

    /*!
     * \brief The 1-based line of the offending rule (0 for a grammar-wide error such as no rules).
     * \return The line.
     */
    [[nodiscard]] std::size_t line() const noexcept
    {
      return line_;
    }

    /*!
     * \brief The 1-based byte column in the line, or 0 when the cause has none.
     * \return The column.
     */
    [[nodiscard]] std::size_t column() const noexcept
    {
      return column_;
    }

    /*!
     * \brief What is wrong, without the location.
     * \return The cause.
     */
    [[nodiscard]] const std::string& cause() const noexcept
    {
      return cause_;
    }

  private:

    /*!
     * \brief `origin:line[:column]: cause`.
     * \param[in] origin Where the grammar came from.
     * \param[in] line   The 1-based line.
     * \param[in] column The 1-based column, or 0 for none.
     * \param[in] cause  What is wrong.
     * \return The message.
     */
    static std::string located(const std::string& origin,
                               std::size_t        line,
                               std::size_t        column,
                               const std::string& cause)
    {
      std::string message {origin};
      if (line != 0) {
        message += ':';
        message += std::to_string(line);
      }
      if (line != 0 && column != 0) {
        message += ':';
        message += std::to_string(column);
      }
      message += ": ";
      message += cause;
      return message;
    }

    std::size_t line_;   //!< See line().
    std::size_t column_; //!< See column().
    std::string cause_;  //!< See cause().
  };

  /*!
   * \brief A parsed grammar: the rules in order, and each rule's name (index = the rule's kind).
   */
  struct grammar
  {
    std::vector<rule>        rules; //!< The rules, kind i at index i.
    std::vector<std::string> names; //!< The name of kind i.

    /*!
     * \brief The name of \p kind, or `"?"` for a kind no rule of this grammar has (a reserved kind such
     *        as \ref error).
     * \param[in] kind A token kind.
     * \return Its name.
     */
    [[nodiscard]] std::string_view name(int kind) const noexcept
    {
      if (kind < 0 || static_cast<std::size_t>(kind) >= names.size()) {
        return "?";
      }
      return names[static_cast<std::size_t>(kind)];
    }
  };

  namespace detail {

    /*!
     * \brief \p before, then \p word in quotes, then \p after: a cause naming the offending word.
     * \param[in] before The text before the word.
     * \param[in] word   The offending word.
     * \param[in] after  The text after it.
     * \return The cause.
     */
    inline std::string quoting(std::string_view before,
                               std::string_view word,
                               std::string_view after)
    {
      std::string cause {before};
      cause += '\'';
      cause += word;
      cause += '\'';
      cause += after;
      return cause;
    }

    //! \brief The fields of one rule line, split on tabs, with the byte column each starts at.
    struct grammar_fields
    {
      std::vector<std::string_view> text;   //!< The fields.
      std::vector<std::size_t>      column; //!< The 1-based column of each field.
    };

    /*!
     * \brief Splits \p line on tabs, from \p first (0-based), keeping each field's column.
     * \param[in] line  The line, trailing blanks already removed.
     * \param[in] first Where the first field starts.
     * \return The fields.
     */
    inline grammar_fields split_grammar_line(std::string_view line,
                                             std::size_t      first)
    {
      grammar_fields fields;
      std::size_t    start {first};
      while (true) {
        const std::size_t tab {line.find('\t', start)};
        const std::size_t end {tab == std::string_view::npos ? line.size() : tab};
        fields.text.push_back(line.substr(start, end - start));
        fields.column.push_back(start + 1);
        if (tab == std::string_view::npos) {
          return fields;
        }
        start = tab + 1;
      }
    }

    /*!
     * \brief \p name, refused when it is empty or holds a comma (which separates the modes of `in=`).
     * \param[in] name   A mode name from an option.
     * \param[in] word   For errors: the whole option.
     * \param[in] origin For errors: where the grammar came from.
     * \param[in] line   For errors: the 1-based line.
     * \param[in] column For errors: the 1-based column of the option.
     * \return \p name.
     * \throws grammar_error When \p name is empty or holds a comma.
     */
    inline std::string_view checked_mode(std::string_view   name,
                                         std::string_view   word,
                                         const std::string& origin,
                                         std::size_t        line,
                                         std::size_t        column)
    {
      if (name.empty() || name.find(',') != std::string_view::npos) {
        // A comma separates the modes of in=, so no mode can be named with one.
        std::string cause {name.empty() ? "empty mode name in '" : "a mode name cannot hold a comma: '"};
        cause += word;
        cause += '\'';
        throw grammar_error(origin, line, column, cause);
      }
      return name;
    }

    /*!
     * \brief Gives \p out its transition, refusing a second one.
     * \param[in,out] out       The rule being built.
     * \param[in]     operation The transition.
     * \param[in]     target    The mode it enters (empty for pop).
     * \param[in]     origin    For errors: where the grammar came from.
     * \param[in]     line      For errors: the 1-based line.
     * \param[in]     column    For errors: the 1-based column of the option.
     * \throws grammar_error When \p out already has a transition.
     */
    inline void set_transition(rule&              out,
                               mode_action::op    operation,
                               std::string_view   target,
                               const std::string& origin,
                               std::size_t        line,
                               std::size_t        column)
    {
      if (out.action) {
        throw grammar_error(origin, line, column, "a rule fires at most one transition (push=, set= or pop)");
      }
      mode_action action {.operation = operation, .target = std::string(target)};
      out.action = std::move(action);
    }

    /*!
     * \brief Applies the space-separated \p options of one rule to \p out.
     * \param[in]     options The options field.
     * \param[in]     column  The 1-based column the field starts at.
     * \param[in]     origin  For errors: where the grammar came from.
     * \param[in]     line    For errors: the 1-based line.
     * \param[in,out] out     The rule being built.
     * \throws grammar_error On an unknown option, an empty mode name, or a second transition.
     */
    inline void apply_grammar_options(std::string_view   options,
                                      std::size_t        column,
                                      const std::string& origin,
                                      std::size_t        line,
                                      rule&              out)
    {
      std::size_t at {0};
      while (at < options.size()) {
        if (options[at] == ' ') {
          ++at;
          continue;
        }
        const std::size_t      end  {std::min(options.find(' ', at), options.size())};
        const std::string_view word {options.substr(at, end - at)};
        const std::size_t      col  {column + at};
        if (word == "skip") {
          out.skip = true;
        }
        else if (word == "pop") {
          set_transition(out, mode_action::op::pop, {}, origin, line, col);
        }
        else if (word.starts_with("push=")) {
          set_transition(out, mode_action::op::push, checked_mode(word.substr(5), word, origin, line, col), origin, line, col);
        }
        else if (word.starts_with("set=")) {
          set_transition(out, mode_action::op::set, checked_mode(word.substr(4), word, origin, line, col), origin, line, col);
        }
        else if (word.starts_with("in=")) {
          std::string_view modes {word.substr(3)};
          while (true) {
            const std::size_t comma {modes.find(',')};
            out.in_mode.emplace_back(checked_mode(modes.substr(0, comma), word, origin, line, col));
            if (comma == std::string_view::npos) {
              break;
            }
            modes.remove_prefix(comma + 1);
          }
        }
        else {
          throw grammar_error(origin, line, col,
                              quoting("unknown option ", word, " (expected skip, in=, push=, set= or pop)"));
        }
        at = end;
      }
    }
  } // namespace detail

  /*!
   * \brief Parses a `.lex` grammar from \p text (see the file documentation for the format).
   *
   * Never returns a half-built grammar: the first malformed line throws.
   *
   * \param[in] text   The grammar.
   * \param[in] origin What errors name as the grammar's origin (a path, or a label).
   * \return The rules and their names.
   * \throws grammar_error On a malformed line, an invalid pattern (at its column in the line), or a
   *         grammar with no rules.
   */
  [[nodiscard]] inline grammar parse_grammar(std::string_view   text,
                                             const std::string& origin = "<string>")
  {
    grammar     parsed;
    std::size_t line_no {0};
    std::size_t at      {0};
    while (at <= text.size()) {
      const std::size_t newline {text.find('\n', at)};
      std::string_view  line    {text.substr(at, (newline == std::string_view::npos ? text.size() : newline) - at)};
      at = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
      ++line_no;
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      const std::size_t first {line.find_first_not_of(" \t")};
      if (first == std::string_view::npos || line[first] == '#') {
        continue; // blank or comment
      }
      line = line.substr(0, line.find_last_not_of(" \t") + 1);

      const detail::grammar_fields fields {detail::split_grammar_line(line, first)};
      if (fields.text.size() < 2 || fields.text.size() > 3) {
        throw grammar_error(origin, line_no, 0, "expected 'name<TAB>pattern' with an optional <TAB>options");
      }
      if (fields.text[1].empty()) {
        throw grammar_error(origin, line_no, fields.column[1], "empty pattern");
      }
      const int kind {static_cast<int>(parsed.rules.size())};
      try {
        parsed.rules.push_back(rule {.kind = kind, .pattern = real::regex(fields.text[1])});
      }
      catch (const real::regex_error& error) {
        throw grammar_error(origin, line_no, fields.column[1] + error.position(), "invalid regex: " + error.cause());
      }
      if (fields.text.size() == 3) {
        detail::apply_grammar_options(fields.text[2], fields.column[2], origin, line_no, parsed.rules.back());
      }
      parsed.names.emplace_back(fields.text[0]);
    }
    if (parsed.rules.empty()) {
      throw grammar_error(origin, 0, 0, "no rules (the grammar is empty)");
    }
    return parsed;
  }

  /*!
   * \brief Reads and parses the `.lex` grammar at \p path.
   * \param[in] path The grammar file.
   * \return The rules and their names.
   * \throws grammar_error If the file cannot be read (line 0) or is malformed (see \ref parse_grammar).
   */
  [[nodiscard]] inline grammar load_grammar(const std::string& path)
  {
    const std::ifstream input {path, std::ios::binary};
    if (!input) {
      throw grammar_error(path, 0, 0, "cannot open grammar file");
    }
    std::ostringstream text;
    text << input.rdbuf();
    return parse_grammar(text.str(), path);
  }
} // namespace scilex

#endif // SCILEX_GRAMMAR_HPP
