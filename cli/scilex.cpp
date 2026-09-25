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
 * Grammar files (see `examples/sample.lex`) use the `.lex` format of the optional
 * `scilex/grammar.hpp` — one rule per line:
 *
 *   name<TAB>regex[<TAB>options]       (`#` comments and blank lines ignored)
 *
 * where the options are `skip`, `in=m1,m2`, and one of `push=m`, `set=m`, `pop`.
 * The lexer itself takes plain C++ rule lists; `scilex.hpp` does not include the
 * format. Built-in grammars reuse the `examples/<lang>.hpp` registry, so the
 * nine showcase languages are defined exactly once.
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
#include <real/version.hpp>
#include <scilex/grammar.hpp>
#include <scilex/layout.hpp>
#include <scilex/scilex.hpp>

#include "cpp.hpp"
#include "css.hpp"
#include "json.hpp"
#include "lisp.hpp"
#include "math.hpp"
#include "python.hpp"
#include "sql.hpp"
#include "xml.hpp"
#include "yaml.hpp"

namespace {

  //! \brief One registered example language: its name and reusable entry points.
  struct example
  {
    std::string_view          name;                      //!< CLI name (`--example <name>`).
    std::vector<scilex::rule> (* make_rules)();          //!< The rule list (to rebuild under an error policy).
    scilex::lexer             (* make_lexer)();          //!< Builds the language's lexer.
    const char              * (*  kind_name)(int);       //!< Names a token kind for printing.
    std::string_view          sample;                    //!< The built-in sample document.
    bool                      (*          self_check)(); //!< Invariant self-check (true = ok).
    bool                      uses_layout;               //!< Run the indentation layout pass.
  };

  // The registry. Each new language: include its header above, add one line here.
  const std::vector<example> registry {
    {"json",
     &scilex::examples::json::make_rules, &scilex::examples::json::make_lexer, &scilex::examples::json::kind_name,
     scilex::examples::json::sample, &scilex::examples::json::self_check, false},
    {"python",
     &scilex::examples::python::make_rules, &scilex::examples::python::make_lexer, &scilex::examples::python::kind_name,
     scilex::examples::python::sample, &scilex::examples::python::self_check, true},
    // The Unicode-identifier variant: the SAME grammar (kind names and sample reused), only the
    // identifier rule differs — it reads café / 変数. Its self-check pins that and the DFA demote.
    {"python-unicode",
     &scilex::examples::python::make_rules_unicode, &scilex::examples::python::make_lexer_unicode,
     &scilex::examples::python::kind_name, scilex::examples::python::sample,
     &scilex::examples::python::self_check_unicode, true},
    {"cpp",
     &scilex::examples::cpp::make_rules, &scilex::examples::cpp::make_lexer, &scilex::examples::cpp::kind_name,
     scilex::examples::cpp::sample, &scilex::examples::cpp::self_check, false},
    {"sql",
     &scilex::examples::sql::make_rules, &scilex::examples::sql::make_lexer, &scilex::examples::sql::kind_name,
     scilex::examples::sql::sample, &scilex::examples::sql::self_check, false},
    {"css",
     &scilex::examples::css::make_rules, &scilex::examples::css::make_lexer, &scilex::examples::css::kind_name,
     scilex::examples::css::sample, &scilex::examples::css::self_check, false},
    {"lisp",
     &scilex::examples::lisp::make_rules, &scilex::examples::lisp::make_lexer, &scilex::examples::lisp::kind_name,
     scilex::examples::lisp::sample, &scilex::examples::lisp::self_check, false},
    {"math",
     &scilex::examples::math::make_rules, &scilex::examples::math::make_lexer, &scilex::examples::math::kind_name,
     scilex::examples::math::sample, &scilex::examples::math::self_check, false},
    {"xml",
     &scilex::examples::xml::make_rules, &scilex::examples::xml::make_lexer, &scilex::examples::xml::kind_name,
     scilex::examples::xml::sample, &scilex::examples::xml::self_check, false},
    {"yaml",
     &scilex::examples::yaml::make_rules, &scilex::examples::yaml::make_lexer, &scilex::examples::yaml::kind_name,
     scilex::examples::yaml::sample, &scilex::examples::yaml::self_check, true},
  };

  //! \brief Names a layout / end-of-input kind, or nullptr when \p kind is an
  //!        ordinary grammar kind (which the grammar's own namer handles).
  const char* layout_name(int kind)
  {
    switch (kind) {
      case scilex::end_of_input: return "EOF";
      case scilex::error:        return "ERROR";
      case scilex::newline:      return "NEWLINE";
      case scilex::indent:       return "INDENT";
      case scilex::dedent:       return "DEDENT";
      default:                   return nullptr;
    }
  }

  //! \brief The name of a lexer's column unit, for the output header.
  const char* column_unit_name(scilex::column_unit unit)
  {
    switch (unit) {
      case scilex::column_unit::codepoints: return "codepoints";
      case scilex::column_unit::utf16:      return "utf16";
      case scilex::column_unit::bytes:      break;
    }
    return "bytes";
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
    // Header: positions do not carry their column unit, so the tool declares it (KIND<TAB>lexeme<TAB>
    // line:col, columns counted in the named unit).
    std::cout << "# columns: " << column_unit_name(lex.columns()) << '\n';
    const auto print = [&](const scilex::token& tok) {
                         const char* const special {layout_name(tok.kind)};
                         std::cout << (special != nullptr ? special : name_of(tok.kind)) << '\t'
                                   << tok.lexeme << '\t' << tok.start.line << ':' << tok.start.column
                                   << '\n';
                       };
    if (layout) {
      const std::vector<scilex::token> flat {lex.tokenize(source, scilex::eof_policy::append)};
      // Policy-aware: an insignificant mode (e.g. YAML flow) adds no layout structure.
      for (const scilex::token& tok : scilex::layout(flat, lex.mode_significant())) {
        print(tok);
      }
    }
    else {
      for (const scilex::token& tok : lex.scan(source)) {
        print(tok);
      }
    }
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
        << "  scilex --version                  print SciLex's version and the REAL it was built with\n"
        << "options:\n"
        << "  --layout                          emit indentation tokens (NEWLINE / INDENT / DEDENT)\n"
        << "  --errors=token                    recover from unlexable bytes (emit ERROR tokens; default: raise)\n"
        << "  --columns=bytes|codepoints|utf16  unit for token columns (default: bytes)\n"
        << "grammar file: one rule per line   name<TAB>regex[<TAB>skip]   ('#' comments, blank lines ok)\n"
        << "output: one token per line        KIND<TAB>lexeme<TAB>line:col\n";
  }

  //! \brief Lexes with a built-in grammar (`--example <lang> [file|-]`).
  int run_example(const std::vector<std::string_view>& args,
                  bool                                 layout,
                  scilex::error_policy                 errors,
                  scilex::column_unit                  columns)
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
    // With a non-default error or column policy, rebuild from the rule list with it; otherwise the
    // grammar's own lexer (with its dfa/layout config) is used unchanged.
    const bool          plain  {errors == scilex::error_policy::raise && columns == scilex::column_unit::bytes};
    const scilex::lexer lex    {plain ? lang->make_lexer()
                             : scilex::lexer {lang->make_rules(), {}, {}, errors, columns}};
    const std::string   source {args.size() >= 3 ? read_input(args[2]) : std::string {lang->sample}};
    dump(lex, source, [lang](int kind) {
           return lang->kind_name(kind);
         }, layout || lang->uses_layout);
    return 0;
  }

  //! \brief Lexes with a user grammar file (`<grammar.lex> [file|-]`).
  int run_grammar(const std::vector<std::string_view>& args,
                  bool                                 layout,
                  scilex::error_policy                 errors,
                  scilex::column_unit                  columns)
  {
    const std::string path {args[0]};
    scilex::grammar   parsed;
    try {
      parsed = scilex::load_grammar(path);
    }
    catch (const scilex::grammar_error& error) {
      if (error.line() == 0 && error.cause() == "cannot open grammar file") {
        throw std::runtime_error(std::string(error.what())
                                 + " (for a built-in grammar use --example; --list shows them)");
      }
      throw;
    }
    const std::vector<std::string> names  {std::move(parsed.names)};
    const scilex::lexer            lex    {std::move(parsed.rules), {}, {}, errors, columns};
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
  bool                          layout  {false};
  scilex::error_policy          errors  {scilex::error_policy::raise};
  scilex::column_unit           columns {scilex::column_unit::bytes};
  for (int i {1}; i < argc; ++i) {
    const std::string_view arg {argv[i]};
    if (arg == "--layout") {
      layout = true;
    }
    else if (arg == "--errors=token") {
      errors = scilex::error_policy::token;
    }
    else if (arg == "--errors=raise") {
      errors = scilex::error_policy::raise;
    }
    else if (arg == "--columns=bytes") {
      columns = scilex::column_unit::bytes;
    }
    else if (arg == "--columns=codepoints") {
      columns = scilex::column_unit::codepoints;
    }
    else if (arg == "--columns=utf16") {
      columns = scilex::column_unit::utf16;
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
    if (command == "--version") {
      std::cout << "scilex " << SCILEX_VERSION_STRING << " (REAL " << REAL_VERSION_STRING << ")\n";
      return 0;
    }
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
      return run_example(args, layout, errors, columns);
    }
    return run_grammar(args, layout, errors, columns);
  }
  catch (const scilex::lex_error& error) {
    std::cerr << "lex error at " << error.where().line << ':' << error.where().column << ": " << error.what()
              << '\n';
    return 1;
  }
  catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
