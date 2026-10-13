/*!
 * \file grammar.hpp
 * \brief The `.lex` grammar format: rules written as text, parsed into \ref scilex::rule lists.
 *
 * Optional, and not included by `scilex.hpp`: the lexer itself takes C++ rule lists, and a program
 * that builds its rules in code needs none of this. It exists so that a grammar kept in a file — the
 * CLI's input, a user-supplied grammar, a Python caller's text — goes through one tested parser.
 *
 * One rule per line: a name, a tab, the pattern, then optionally a tab and space-separated options.
 * Blank lines and lines whose first non-blank character is `#` are ignored; a UTF-8 byte order mark
 * that starts the text is dropped, a trailing `\r` too, and so are the spaces and tabs that end a
 * line, so a pattern never ends in one: write a final space `[ ]` or `\x20`. The options are:
 *
 * - `skip` — matches are consumed but not emitted;
 * - `in=m1,m2` — the modes the rule is active in (default: `default` only);
 * - `push=m`, `set=m`, `pop` — the mode transition fired when the rule wins (at most one); a mode
 *   that `push=` or `set=` enters must have a rule active in it;
 * - `after=A,B` or `not_after=A,B` — the rule is active only right after (or except right after) a token
 *   of the rules named `A` or `B` (\ref scilex::rule::after): a name stands for every rule that bears
 *   it, earlier or later in the file, and `^` for the start of the input;
 * - `indent=entry`, `indent=key`, `indent=value` or `indent=reset` — the rule's part in the indentation the
 *   lexer tracks, and `extend=block` or `extend=lines` — how its token extends below its line (Layout
 *   Awareness Level B, \ref rule_indentation);
 * - `continue=N` — with `extend=lines`, what each continued line must hold: the pattern of the fragment (or
 *   else the first rule) named `N`, earlier or later in the file;
 * - `fragment` — the line names a pattern for `continue=`, not a rule: it takes no other option, has no
 *   kind, and is not lexed.
 *
 * A line whose first non-blank character is `%` is a directive, not a rule; the one directive is
 *
 * - `%insignificant m1,m2` — the modes whose tokens carry no layout structure (the lexer's
 *   `insignificant_modes`); each must have a rule active in it, and the directive may repeat.
 *
 * A rule's kind is its 0-based position among the rules, fragments not counted; \ref scilex::grammar::names
 * maps it back.
 *
 * \code
 * WS       \s+        skip
 * %insignificant str
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
#include <optional>
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
    std::vector<rule>        rules;               //!< The rules, kind i at index i.
    std::vector<std::string> names;               //!< The name of kind i.
    std::vector<std::string> insignificant_modes; //!< The modes `%insignificant` names, in order.

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

    //! \brief An `after=` or `not_after=` option, its names resolved once every rule is read.
    struct predicate_site
    {
      std::size_t                   rule;   //!< The rule's index.
      bool                          after;  //!< `after=` (else `not_after=`).
      std::vector<std::string_view> names;  //!< The names it lists (views into the grammar text).
      std::size_t                   line;   //!< Its 1-based line.
      std::size_t                   column; //!< The 1-based column of the option.
    };

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

    //! \brief The options of a line that name another line, or make it a fragment, resolved once every line
    //!        is read.
    struct line_extras
    {
      bool             fragment        {false}; //!< `fragment`: the line names a pattern, not a rule.
      std::size_t      fragment_column {0};     //!< The 1-based column of `fragment`.
      std::string_view continuation    {};      //!< The name `continue=` gives, or empty.
      std::size_t      continue_column {0};     //!< The 1-based column of `continue=`.
      std::size_t      options         {0};     //!< How many options the line gives.
    };

    /*!
     * \brief Applies the space-separated \p options of one rule to \p out.
     * \param[in]     options The options field.
     * \param[in]     column  The 1-based column the field starts at.
     * \param[in]     origin  For errors: where the grammar came from.
     * \param[in]     line    For errors: the 1-based line.
     * \param[in,out] out     The rule being built.
     * \param[in]     index   The rule's index (for its `after=` / `not_after=` entry).
     * \param[in,out] predicates Each `after=` / `not_after=` option, appended with its names unresolved.
     * \param[out]    extras  Its `fragment` and `continue=` options.
     * \return The 1-based column of its `push=` or `set=` option, or 0 when it has none.
     * \throws grammar_error On an unknown option or value, an empty mode name, or a second transition.
     */
    inline std::size_t apply_grammar_options(std::string_view             options,
                                             std::size_t                  column,
                                             const std::string&           origin,
                                             std::size_t                  line,
                                             rule&                        out,
                                             std::size_t                  index,
                                             std::vector<predicate_site>& predicates,
                                             line_extras&                 extras)
    {
      std::size_t transition_column {0};
      std::size_t at                {0};
      while (at < options.size()) {
        if (options[at] == ' ') {
          ++at;
          continue;
        }
        const std::size_t      end  {std::min(options.find(' ', at), options.size())};
        const std::string_view word {options.substr(at, end - at)};
        const std::size_t      col  {column + at};
        ++extras.options;
        if (word == "skip") {
          out.skip = true;
        }
        else if (word == "pop") {
          set_transition(out, mode_action::op::pop, {}, origin, line, col);
        }
        else if (word.starts_with("push=")) {
          set_transition(out, mode_action::op::push, checked_mode(word.substr(5), word, origin, line, col), origin, line, col);
          transition_column = col;
        }
        else if (word.starts_with("set=")) {
          set_transition(out, mode_action::op::set, checked_mode(word.substr(4), word, origin, line, col), origin, line, col);
          transition_column = col;
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
        else if (word.starts_with("after=") || word.starts_with("not_after=")) {
          const bool after {word.starts_with("after=")};
          if (std::ranges::any_of(predicates, [&](const predicate_site& p) { return p.rule == index; })) {
            throw grammar_error(origin, line, col, "a rule takes one of after= and not_after=, once");
          }
          predicate_site   site  {.rule = index, .after = after, .names = {}, .line = line, .column = col};
          std::string_view names {word.substr(after ? 6 : 10)};
          while (true) {
            const std::size_t      comma {names.find(',')};
            const std::string_view name  {names.substr(0, comma)};
            if (name.empty()) {
              throw grammar_error(origin, line, col, quoting("empty rule name in ", word, ""));
            }
            site.names.push_back(name);
            if (comma == std::string_view::npos) {
              break;
            }
            names.remove_prefix(comma + 1);
          }
          predicates.push_back(std::move(site));
        }
        else if (word.starts_with("indent=")) {
          const std::string_view role {word.substr(7)};
          if (role == "entry") {
            out.indent = indent_role::entry;
          }
          else if (role == "key") {
            out.indent = indent_role::key;
          }
          else if (role == "value") {
            out.indent = indent_role::value;
          }
          else if (role == "reset") {
            out.indent = indent_role::reset;
          }
          else {
            throw grammar_error(origin, line, col, quoting("unknown role ", word, " (expected entry, key, value or reset)"));
          }
        }
        else if (word.starts_with("extend=")) {
          const std::string_view how {word.substr(7)};
          if (how == "block") {
            out.extend = extension::block;
          }
          else if (how == "lines") {
            out.extend = extension::lines;
          }
          else {
            throw grammar_error(origin, line, col, quoting("unknown extension ", word, " (expected block or lines)"));
          }
        }
        else if (word.starts_with("continue=")) {
          if (word.size() == 9) {
            throw grammar_error(origin, line, col, "empty name in continue=");
          }
          extras.continuation    = word.substr(9);
          extras.continue_column = col;
        }
        else if (word == "fragment") {
          extras.fragment        = true;
          extras.fragment_column = col;
        }
        else {
          throw grammar_error(origin, line, col,
                              quoting("unknown option ", word,
                                      " (expected skip, in=, push=, set=, pop, after=, not_after=, indent=, extend=, continue= or fragment)"));
        }
        at = end;
      }
      return transition_column;
    }

    //! \brief Where a rule's `push=` or `set=` option is written.
    struct transition_site
    {
      std::size_t rule;   //!< The rule's index.
      std::size_t line;   //!< Its 1-based line.
      std::size_t column; //!< The 1-based column of the option.
    };

    /*!
     * \brief Refuses a `push=` or `set=` whose mode no rule is active in, at the option.
     * \param[in] parsed The grammar.
     * \param[in] sites  Where each `push=` and `set=` is written.
     * \param[in] origin For errors: where the grammar came from.
     * \throws grammar_error For the first such transition.
     */
    inline void check_transition_targets(const grammar&                      parsed,
                                         const std::vector<transition_site>& sites,
                                         const std::string&                  origin)
    {
      const auto active_in = [&](std::string_view mode) {
                               return std::ranges::any_of(parsed.rules, [&](const rule& r) {
                                                            return r.in_mode.empty()
                                                                   ? mode == "default"
                                                                   : std::ranges::find(r.in_mode, mode) != r.in_mode.end();
                                                          });
                             };
      for (const transition_site& site : sites) {
        const std::string& target {parsed.rules[site.rule].action->target};
        if (!active_in(target)) {
          throw grammar_error(origin, site.line, site.column,
                              quoting("no rule is active in mode ", target, " (give one in=") + target + ")");
        }
      }
    }

    /*!
     * \brief Gives each rule of \p sites the kinds its names stand for: every rule bearing the name, and
     *        \ref scilex::end_of_input for `^`.
     * \throws grammar_error At the option, for a name no rule bears.
     */
    inline void resolve_predicates(grammar&                           parsed,
                                   const std::vector<predicate_site>& sites,
                                   const std::string&                 origin)
    {
      for (const predicate_site& site : sites) {
        std::vector<int> kinds;
        for (const std::string_view name : site.names) {
          if (name == "^") {
            kinds.push_back(end_of_input);
            continue;
          }
          bool found {false};
          for (std::size_t k {0}; k < parsed.names.size(); ++k) {
            if (parsed.names[k] == name) {
              kinds.push_back(static_cast<int>(k));
              found = true;
            }
          }
          if (!found) {
            throw grammar_error(origin, site.line, site.column,
                                quoting("no rule is named ", name, " (after= and not_after= name rules, or ^)"));
          }
        }
        (site.after ? parsed.rules[site.rule].after : parsed.rules[site.rule].not_after) = std::move(kinds);
      }
    }

    //! \brief A `fragment` line: a named pattern for `continue=`.
    struct fragment
    {
      std::string_view name;    //!< Its name (a view into the grammar text).
      real::regex      pattern; //!< Its pattern.
    };

    //! \brief A `continue=` option, its name resolved once every line is read.
    struct continue_site
    {
      std::size_t      rule;   //!< The rule's index.
      std::string_view name;   //!< The name it gives.
      std::size_t      line;   //!< Its 1-based line.
      std::size_t      column; //!< The 1-based column of the option.
    };

    /*!
     * \brief Gives each rule of \p sites the pattern its `continue=` names: the fragment of that name, else the
     *        first rule of that name.
     * \throws grammar_error At the option, for a rule without `extend=lines` or a name nothing bears.
     */
    inline void resolve_continuations(grammar&                          parsed,
                                      const std::vector<fragment>&      fragments,
                                      const std::vector<continue_site>& sites,
                                      const std::string&                origin)
    {
      for (const continue_site& site : sites) {
        rule& target {parsed.rules[site.rule]};
        if (target.extend != extension::lines) {
          throw grammar_error(origin, site.line, site.column, "continue= reads the lines an extend=lines rule continues on");
        }
        const auto named {std::ranges::find(fragments, site.name, &fragment::name)};
        if (named != fragments.end()) {
          target.continuation = named->pattern;
          continue;
        }
        const auto ruled {std::ranges::find(parsed.names, site.name)};
        if (ruled == parsed.names.end()) {
          throw grammar_error(origin, site.line, site.column,
                              quoting("no fragment or rule is named ", site.name, " (continue= names one)"));
        }
        target.continuation = parsed.rules[static_cast<std::size_t>(ruled - parsed.names.begin())].pattern;
      }
    }

    //! \brief Where a `%insignificant` directive names a mode.
    struct directive_site
    {
      std::string mode;   //!< The mode.
      std::size_t line;   //!< The 1-based line.
      std::size_t column; //!< The 1-based column of the mode list.
    };

    /*!
     * \brief Parses the directive \p line (its `%` at 0-based \p first): `%insignificant m1,m2`.
     * \param[in]     line   The line, trailing blanks already removed.
     * \param[in]     first  Where the `%` is.
     * \param[in]     origin For errors: where the grammar came from.
     * \param[in]     number For errors: the 1-based line.
     * \param[in,out] sites  Each named mode is appended, with where it is written.
     * \throws grammar_error On another directive, or an empty or malformed mode list.
     */
    inline void parse_directive(std::string_view             line,
                                std::size_t                  first,
                                const std::string&           origin,
                                std::size_t                  number,
                                std::vector<directive_site>& sites)
    {
      constexpr std::string_view insignificant {"%insignificant"};
      const std::size_t          end           {std::min(line.find_first_of(" \t", first), line.size())};
      const std::string_view     word          {line.substr(first, end - first)};
      if (word != insignificant) {
        throw grammar_error(origin, number, first + 1, quoting("unknown directive ", word, " (expected %insignificant)"));
      }
      const std::size_t list {line.find_first_not_of(" \t", end)};
      if (list == std::string_view::npos) {
        throw grammar_error(origin, number, line.size() + 1, "%insignificant names at least one mode");
      }
      std::string_view modes  {line.substr(list)};
      std::size_t      column {list + 1};
      while (true) {
        const std::size_t      comma {modes.find(',')};
        const std::string_view name  {modes.substr(0, comma)};
        if (name.empty() || name.find_first_of(" \t") != std::string_view::npos) {
          throw grammar_error(origin, number, column, quoting("malformed mode list ", line.substr(list), " (write m1,m2)"));
        }
        sites.push_back({.mode = std::string(name), .line = number, .column = column});
        if (comma == std::string_view::npos) {
          return;
        }
        column += comma + 1;
        modes.remove_prefix(comma + 1);
      }
    }

    /*!
     * \brief Records the modes of \p sites in \p parsed, refusing one no rule is active in, at its column.
     * \throws grammar_error For the first such mode.
     */
    inline void apply_directives(grammar&                           parsed,
                                 const std::vector<directive_site>& sites,
                                 const std::string&                 origin)
    {
      for (const directive_site& site : sites) {
        const bool active {std::ranges::any_of(parsed.rules, [&](const rule& r) {
                                                 return r.in_mode.empty()
                                                        ? site.mode == "default"
                                                        : std::ranges::find(r.in_mode, site.mode) != r.in_mode.end();
                                               })};
        if (!active) {
          throw grammar_error(origin, site.line, site.column,
                              quoting("no rule is active in mode ", site.mode, " (give one in=") + site.mode + ")");
        }
        parsed.insignificant_modes.push_back(site.mode);
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
   * \throws grammar_error On a malformed line or directive, an invalid pattern (at its column in the
   *         line), a mode no rule is active in, or a grammar with no rules.
   */
  [[nodiscard]] inline grammar parse_grammar(std::string_view   text,
                                             const std::string& origin = "<string>")
  {
    constexpr std::string_view           byte_order_mark {"\xEF\xBB\xBF"};
    grammar                              parsed;
    std::vector<detail::transition_site> transitions;
    std::vector<detail::directive_site>  directives;
    std::vector<detail::predicate_site>  predicates;
    std::vector<detail::fragment>        fragments;
    std::vector<detail::continue_site>   continuations;
    std::size_t                          line_no {0};
    std::size_t                          at      {text.starts_with(byte_order_mark) ? byte_order_mark.size() : 0};
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
      const std::string_view untrimmed {line};
      line = line.substr(0, line.find_last_not_of(" \t") + 1);
      if (line[first] == '%') {
        detail::parse_directive(line, first, origin, line_no, directives);
        continue;
      }

      const detail::grammar_fields fields {detail::split_grammar_line(line, first)};
      if (fields.text.size() < 2 && untrimmed.find('\t', first) != std::string_view::npos) {
        throw grammar_error(origin, line_no, line.size() + 2,
                            "empty pattern: the blanks that end a line are dropped (write a space as [ ] or \\x20)");
      }
      if (fields.text.size() < 2 || fields.text.size() > 3) {
        throw grammar_error(origin, line_no, 0, "expected 'name<TAB>pattern' with an optional <TAB>options");
      }
      if (fields.text[1].empty()) {
        throw grammar_error(origin, line_no, fields.column[1], "empty pattern");
      }
      const int           kind {static_cast<int>(parsed.rules.size())};
      std::optional<rule> read;
      try {
        read.emplace(rule {.kind = kind, .pattern = real::regex(fields.text[1])});
      }
      catch (const real::regex_error& invalid) {
        throw grammar_error(origin, line_no, fields.column[1] + invalid.position(), "invalid regex: " + invalid.cause());
      }
      detail::line_extras extras {};
      std::size_t         column {0};
      if (fields.text.size() == 3) {
        column = detail::apply_grammar_options(fields.text[2], fields.column[2], origin, line_no, *read,
                                               parsed.rules.size(), predicates, extras);
      }
      if (extras.fragment) {
        if (extras.options != 1) {
          throw grammar_error(origin, line_no, extras.fragment_column, "a fragment takes no other option");
        }
        fragments.push_back({.name = fields.text[0], .pattern = std::move(read->pattern)});
        continue;
      }
      if (column != 0) {
        transitions.push_back({.rule = parsed.rules.size(), .line = line_no, .column = column});
      }
      if (!extras.continuation.empty()) {
        continuations.push_back({.rule   = parsed.rules.size(), .name = extras.continuation, .line = line_no,
                                 .column = extras.continue_column});
      }
      parsed.rules.push_back(std::move(*read));
      parsed.names.emplace_back(fields.text[0]);
    }
    if (parsed.rules.empty()) {
      throw grammar_error(origin, 0, 0, "no rules (the grammar is empty)");
    }
    detail::check_transition_targets(parsed, transitions, origin);
    detail::resolve_predicates(parsed, predicates, origin);
    detail::resolve_continuations(parsed, fragments, continuations, origin);
    detail::apply_directives(parsed, directives, origin);
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
