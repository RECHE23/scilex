/*!
 * \file scilex.cpp
 * \brief The `scilex` command-line lexer — SciLex as a universal tool.
 *
 * Two input modes:
 *
 *   scilex --list                      list the built-in example grammars
 *   scilex --example <lang> [file|-]   lex with a built-in grammar (its bundled
 *                                      sample if no file is given) — the showcase
 *   scilex <grammar.lex> [file|-]      lex with YOUR grammar (stdin if no file)
 *                                      — the universal lexer
 *   scilex --check                     run every example self-check (dev gate)
 *
 * Option `--layout` runs the indentation pass (emitting NEWLINE / INDENT /
 * DEDENT). Output is one token per line: KIND<TAB>lexeme<TAB>line:col.
 *
 * Grammar files (see `examples/sample.lex`) are a deliberately thin, **CLI-only**
 * format — one rule per line:
 *
 *   name<TAB>regex[<TAB>skip]          (`#` comments and blank lines ignored)
 *
 * The CLI parses them straight into `scilex::rule`. The library itself stays
 * plain C++ rule lists; no spec language is embedded — this format lives only in
 * the tool. Built-in grammars reuse the `examples/<lang>.hpp` registry, so the
 * seven showcase languages are defined exactly once.
 */
#include <cstddef>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <real/real.hpp>
#include <scilex/layout.hpp>
#include <scilex/scilex.hpp>

#include "cpp.hpp"
#include "css.hpp"
#include "json.hpp"
#include "lisp.hpp"
#include "math.hpp"
#include "python.hpp"
#include "sql.hpp"

namespace {

  //! \brief One registered example language: its name and reusable entry points.
  struct example
  {
    std::string_view name;                      //!< CLI name (`--example <name>`).
    scilex::lexer    (* make_lexer)();          //!< Builds the language's lexer.
    const char     * (*  kind_name)(int);       //!< Names a token kind for printing.
    std::string_view sample;                    //!< The built-in sample document.
    bool             (*          self_check)(); //!< Invariant self-check (true = ok).
    bool             uses_layout;               //!< Run the indentation layout pass.
  };

  // The registry. Each new language: include its header above, add one line here.
  const std::vector<example> registry {
    {"json",
     &scilex::examples::json::make_lexer, &scilex::examples::json::kind_name,
     scilex::examples::json::sample, &scilex::examples::json::self_check, false},
    {"python",
     &scilex::examples::python::make_lexer, &scilex::examples::python::kind_name,
     scilex::examples::python::sample, &scilex::examples::python::self_check, true},
    {"cpp",
     &scilex::examples::cpp::make_lexer, &scilex::examples::cpp::kind_name,
     scilex::examples::cpp::sample, &scilex::examples::cpp::self_check, false},
    {"sql",
     &scilex::examples::sql::make_lexer, &scilex::examples::sql::kind_name,
     scilex::examples::sql::sample, &scilex::examples::sql::self_check, false},
    {"css",
     &scilex::examples::css::make_lexer, &scilex::examples::css::kind_name,
     scilex::examples::css::sample, &scilex::examples::css::self_check, false},
    {"lisp",
     &scilex::examples::lisp::make_lexer, &scilex::examples::lisp::kind_name,
     scilex::examples::lisp::sample, &scilex::examples::lisp::self_check, false},
    {"math",
     &scilex::examples::math::make_lexer, &scilex::examples::math::kind_name,
     scilex::examples::math::sample, &scilex::examples::math::self_check, false},
  };

  //! \brief Names a layout / end-of-input kind, or nullptr when \p kind is an
  //!        ordinary grammar kind (which the grammar's own namer handles).
  const char* layout_name(int kind)
  {
    switch (kind) {
      case scilex::end_of_input: return "EOF";
      case scilex::newline:      return "NEWLINE";
      case scilex::indent:       return "INDENT";
      case scilex::dedent:       return "DEDENT";
      default:                   return nullptr;
    }
  }

  //! \brief Tokenizes \p source with \p lex and prints each token as
  //!        KIND<TAB>lexeme<TAB>line:col. \p name_of names ordinary kinds; layout
  //!        and EOF kinds are named directly. With \p layout, runs the indentation
  //!        pass (NEWLINE / INDENT / DEDENT) over an EOF-terminated stream.
  void dump(const scilex::lexer&                    lex,
            std::string_view                        source,
            const std::function<const char* (int)>& name_of,
            bool                                    layout)
  {
    const auto print = [&](const scilex::token& tok) {
                         const char* const special {layout_name(tok.kind)};
                         std::cout << (special != nullptr ? special : name_of(tok.kind)) << '\t'
                                   << tok.lexeme << '\t' << tok.start.line << ':' << tok.start.column
                                   << '\n';
                       };
    if (layout) {
      const std::vector<scilex::token> flat {lex.tokenize(source, scilex::eof_policy::append)};
      for (const scilex::token& tok : scilex::layout(flat)) {
        print(tok);
      }
    }
    else {
      for (const scilex::token& tok : lex.scan(source)) {
        print(tok);
      }
    }
  }

  //! \brief A grammar parsed from a `.lex` file: the rules plus the names that
  //!        label each kind (kind i is the rule's 0-based position).
  struct grammar
  {
    std::vector<scilex::rule> rules;
    std::vector<std::string>  names;
  };

  //! \brief Throws a clear, positioned grammar-file error.
  [[noreturn]] void grammar_error(const std::string& path,
                                  int                line,
                                  const std::string& why)
  {
    throw std::runtime_error(path + ":" + std::to_string(line) + ": " + why);
  }

  //! \brief Splits \p text on tab characters (no trimming of the parts).
  std::vector<std::string> split_tabs(std::string_view text)
  {
    std::vector<std::string> parts;
    std::size_t              start {0};
    while (true) {
      const std::size_t tab {text.find('\t', start)};
      if (tab == std::string_view::npos) {
        parts.emplace_back(text.substr(start));
        return parts;
      }
      parts.emplace_back(text.substr(start, tab - start));
      start = tab + 1;
    }
  }

  //! \brief Parses a `.lex` grammar file. Each non-blank, non-`#` line is
  //!        `name<TAB>regex[<TAB>skip]`. Throws (positioned) on any malformed line
  //!        or invalid regex — never returns a half-built grammar.
  grammar parse_grammar(const std::string& path)
  {
    std::ifstream input {path};
    if (!input) {
      throw std::runtime_error(path + ": cannot open grammar file"
                               " (for a built-in grammar use --example; --list shows them)");
    }
    grammar     parsed;
    std::string line;
    int         lineno {0};
    while (std::getline(input, line)) {
      ++lineno;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back(); // tolerate CRLF
      }
      const std::size_t first {line.find_first_not_of(" \t")};
      if (first == std::string::npos) {
        continue; // blank line
      }
      if (line[first] == '#') {
        continue; // comment
      }
      std::string       content {line.substr(first)};
      const std::size_t last    {content.find_last_not_of(" \t")};
      content.erase(last + 1); // trim trailing whitespace

      const std::vector<std::string> fields {split_tabs(content)};
      if (fields.size() < 2 || fields.size() > 3) {
        grammar_error(path, lineno, "expected 'name<TAB>regex' with an optional <TAB>skip");
      }
      if (fields[0].empty()) {
        grammar_error(path, lineno, "empty rule name");
      }
      if (fields[1].empty()) {
        grammar_error(path, lineno, "empty pattern");
      }
      bool skip {false};
      if (fields.size() == 3) {
        if (fields[2] == "skip") {
          skip = true;
        }
        else {
          grammar_error(path, lineno, "third field must be 'skip' or omitted (got '" + fields[2] + "')");
        }
      }
      const int kind {static_cast<int>(parsed.names.size())};
      try {
        parsed.rules.push_back(scilex::rule {kind, real::regex(fields[1]), skip});
      }
      catch (const real::regex_error& error) {
        grammar_error(path, lineno, std::string {"invalid regex: "} + error.what());
      }
      parsed.names.push_back(fields[0]);
    }
    if (parsed.rules.empty()) {
      throw std::runtime_error(path + ": no rules (the grammar is empty)");
    }
    return parsed;
  }

  //! \brief Reads all of \p in into a string.
  std::string read_all(std::istream& in)
  {
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  //! \brief Reads input: stdin when \p path is empty or "-", else the named file.
  std::string read_input(std::string_view path)
  {
    if (path.empty() || path == "-") {
      return read_all(std::cin);
    }
    std::ifstream file {std::string {path}, std::ios::binary};
    if (!file) {
      throw std::runtime_error(std::string {path} + ": cannot open input file");
    }
    return read_all(file);
  }

  const example* find(std::string_view name)
  {
    for (const example& entry : registry) {
      if (entry.name == name) {
        return &entry;
      }
    }
    return nullptr;
  }

  int list()
  {
    for (const example& entry : registry) {
      std::cout << entry.name << '\n';
    }
    return 0;
  }

  int check()
  {
    bool ok {true};
    for (const example& entry : registry) {
      if (!entry.self_check()) {
        std::cerr << entry.name << ": self-check failed\n";
        ok = false;
      }
    }
    return ok ? 0 : 1;
  }

  void usage(std::ostream& out)
  {
    out << "usage:\n"
        << "  scilex --list                     list the built-in example grammars\n"
        << "  scilex --example <lang> [file|-]  lex with a built-in grammar (its sample if no file)\n"
        << "  scilex <grammar.lex> [file|-]     lex with your grammar (stdin if no file)\n"
        << "  scilex --check                    run every example self-check\n"
        << "options:\n"
        << "  --layout                          emit indentation tokens (NEWLINE / INDENT / DEDENT)\n"
        << "grammar file: one rule per line   name<TAB>regex[<TAB>skip]   ('#' comments, blank lines ok)\n"
        << "output: one token per line        KIND<TAB>lexeme<TAB>line:col\n";
  }

  //! \brief Lexes with a built-in grammar (`--example <lang> [file|-]`).
  int run_example(const std::vector<std::string_view>& args,
                  bool                                 layout)
  {
    if (args.size() < 2) {
      std::cerr << "scilex --example needs a language (try --list)\n";
      return 2;
    }
    const example* const lang {find(args[1])};
    if (lang == nullptr) {
      std::cerr << "unknown example grammar: " << args[1] << " (try --list)\n";
      return 2;
    }
    const scilex::lexer lex    {lang->make_lexer()};
    const std::string   source {args.size() >= 3 ? read_input(args[2]) : std::string {lang->sample}};
    dump(lex, source, [lang](int kind) {
           return lang->kind_name(kind);
         }, layout || lang->uses_layout);
    return 0;
  }

  //! \brief Lexes with a user grammar file (`<grammar.lex> [file|-]`).
  int run_grammar(const std::vector<std::string_view>& args,
                  bool                                 layout)
  {
    grammar                        parsed {parse_grammar(std::string {args[0]})};
    const std::vector<std::string> names  {std::move(parsed.names)};
    const scilex::lexer            lex    {std::move(parsed.rules)};
    const std::string              source {read_input(args.size() >= 2 ? args[1] : std::string_view {"-"})};
    dump(lex, source, [&names](int kind) {
           return (kind >= 0 && static_cast<std::size_t>(kind) < names.size())
                  ? names[static_cast<std::size_t>(kind)].c_str()
                  : "?";
         }, layout);
    return 0;
  }
} // namespace

int main(int    argc,
         char** argv)
{
  std::vector<std::string_view> args;
  bool                          layout {false};
  for (int i {1}; i < argc; ++i) {
    const std::string_view arg {argv[i]};
    if (arg == "--layout") {
      layout = true;
    }
    else {
      args.push_back(arg);
    }
  }
  if (args.empty()) {
    usage(std::cerr);
    return 2;
  }

  const std::string_view command {args[0]};
  try {
    if (command == "--help" || command == "-h") {
      usage(std::cout);
      return 0;
    }
    if (command == "--list") {
      return list();
    }
    if (command == "--check") {
      return check();
    }
    if (command == "--example") {
      return run_example(args, layout);
    }
    return run_grammar(args, layout);
  }
  catch (const scilex::lex_error& error) {
    std::cerr << "lex error at " << error.where().line << ':' << error.where().column << '\n';
    return 1;
  }
  catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
