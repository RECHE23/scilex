/*!
 * \file tokens.cpp
 * \brief Demo + CLI seed + gate driver for the example lexers.
 *
 * Each `examples/<lang>.hpp` defines a reusable grammar (`make_lexer`,
 * `kind_name`, `sample`, `self_check`) in namespace `scilex::examples::<lang>`.
 * This thin driver dispatches over them through plain function pointers — one
 * binary, not one per language:
 *
 *   tokens --list           list the example languages
 *   tokens --check          run every self-check (the `make example` gate)
 *   tokens <lang>           tokenize <lang>'s built-in sample
 *   tokens <lang> <text>…   tokenize each <text> argument
 *
 * Printing one token per line (KIND, lexeme, line:col) is also the seed of the
 * eventual `scilex` command-line tool.
 */
#include <iostream>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

#include "json.hpp"

namespace {

  //! \brief One registered example language: its name and reusable entry points.
  struct example
  {
    std::string_view name;                      //!< CLI name (`tokens <name>`).
    scilex::lexer    (* make_lexer)();          //!< Builds the language's lexer.
    const char     * (*  kind_name)(int);       //!< Names a token kind for printing.
    std::string_view sample;                    //!< The built-in sample document.
    bool             (*          self_check)(); //!< Invariant self-check (true = ok).
  };

  // The registry. Each new language: include its header above, add one line here.
  const std::vector<example> registry {
    {"json",
     &scilex::examples::json::make_lexer,
     &scilex::examples::json::kind_name,
     scilex::examples::json::sample,
     &scilex::examples::json::self_check},
  };

  //! \brief Tokenizes \p source with \p lang's grammar and prints each token.
  void dump(const example&   lang,
            std::string_view source)
  {
    const scilex::lexer lex {lang.make_lexer()};
    for (const scilex::token& tok : lex.scan(source)) {
      std::cout << lang.kind_name(tok.kind) << '\t' << tok.lexeme << '\t'
                << tok.start.line << ':' << tok.start.column << '\n';
    }
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

  int usage()
  {
    std::cerr << "usage: tokens --list | --check | <lang> [text...]\n";
    return 2;
  }
} // namespace

int main(int    argc,
         char** argv)
{
  if (argc < 2) {
    return usage();
  }
  const std::string_view command {argv[1]};
  if (command == "--list") {
    return list();
  }
  if (command == "--check") {
    return check();
  }
  const example* lang {find(command)};
  if (lang == nullptr) {
    std::cerr << "unknown example language: " << command << '\n';
    return usage();
  }
  try {
    if (argc == 2) {
      dump(*lang, lang->sample);
    }
    else {
      for (int i {2}; i < argc; ++i) {
        dump(*lang, std::string_view {argv[i]});
      }
    }
  }
  catch (const scilex::lex_error& error) {
    std::cerr << "lex error at " << error.where().line << ':' << error.where().column << '\n';
    return 1;
  }
  return 0;
}
