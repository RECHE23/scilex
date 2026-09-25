// The `.lex` grammar format (scilex/grammar.hpp): the parse of each option, each refusal at its line
// and column, and a parsed modal grammar lexing as the same rules written in C++ do.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/grammar.hpp"
#include "scilex/scilex.hpp"

using namespace std::string_view_literals;

namespace {

  //! \brief The grammar_error \p text raises, or a default one when it parses.
  scilex::grammar_error refusal(std::string_view text)
  {
    try {
      static_cast<void>(scilex::parse_grammar(text, "g.lex"));
    }
    catch (const scilex::grammar_error& error) {
      return error;
    }
    return {"<parsed>", 0, 0, "no error"};
  }

  TEST(grammar_plain_rules_take_their_position_as_kind)
  {
    const scilex::grammar g {scilex::parse_grammar("# a comment\n\nWS\t\\s+\tskip\r\nID\t[a-z]+\n  NUM\t[0-9]+  \n")};
    EXPECT_EQ(g.rules.size(), 3U);
    EXPECT_EQ(g.names.size(), 3U);
    EXPECT_EQ(g.names[0], "WS");
    EXPECT_EQ(g.names[2], "NUM"); // leading and trailing blanks around a line are not part of it
    EXPECT_EQ(g.rules[1].kind, 1);
    EXPECT(g.rules[0].skip);
    EXPECT(!g.rules[1].skip);
    EXPECT(g.rules[1].in_mode.empty());
    EXPECT(!g.rules[1].action.has_value());
    EXPECT_EQ(g.name(2), "NUM"sv);
    EXPECT_EQ(g.name(3), "?"sv);
    EXPECT_EQ(g.name(-1), "?"sv);
    EXPECT_EQ(g.name(scilex::error), "?"sv);
  }

  TEST(grammar_options_set_modes_and_transitions)
  {
    const scilex::grammar g {scilex::parse_grammar("STR\t\"\tpush=str\nTXT\t[^\"]+\tin=str\n"
                                                   "END\t\"\tin=str  pop\nMID\tx\tin=default,str set=other skip\n")};
    EXPECT(g.rules[0].action.has_value());
    EXPECT(g.rules[0].action->operation == scilex::mode_action::op::push);
    EXPECT_EQ(g.rules[0].action->target, "str");
    EXPECT_EQ(g.rules[1].in_mode.size(), 1U);
    EXPECT_EQ(g.rules[1].in_mode[0], "str");
    EXPECT(g.rules[2].action->operation == scilex::mode_action::op::pop);
    EXPECT_EQ(g.rules[3].in_mode.size(), 2U);
    EXPECT_EQ(g.rules[3].in_mode[1], "str");
    EXPECT(g.rules[3].action->operation == scilex::mode_action::op::set);
    EXPECT_EQ(g.rules[3].action->target, "other");
    EXPECT(g.rules[3].skip);
  }

  // The parsed grammar lexes as the rules written in C++: modes, transitions and skips included.
  TEST(grammar_a_parsed_modal_grammar_lexes)
  {
    scilex::grammar     g                {scilex::parse_grammar("WS\t\\s+\tskip\nSTR\t\"\tpush=str\nTXT\t[^\"]+\tin=str\n"
                                                                "END\t\"\tin=str pop\nID\t[a-z]+\n")};
    const std::vector<std::string> names {g.names};
    const scilex::lexer            lex   {std::move(g.rules)};
    const std::string              src   {"ab \"hi there\" cd"};
    std::vector<std::string>       got;
    for (const scilex::token& tok : lex.tokenize(src)) {
      got.push_back(names[static_cast<std::size_t>(tok.kind)] + ":" + std::string(tok.lexeme));
    }
    const std::vector<std::string> want {"ID:ab", "STR:\"", "TXT:hi there", "END:\"", "ID:cd"};
    EXPECT(got == want);
  }

  TEST(grammar_refusals_name_their_line_and_column)
  {
    const scilex::grammar_error fields {refusal("A\ta\n\nB\n")};
    EXPECT_EQ(fields.line(), 3U);
    EXPECT_EQ(fields.column(), 0U);
    EXPECT_EQ(std::string(fields.what()).rfind("g.lex:3: expected", 0), 0U);

    const scilex::grammar_error too_many {refusal("A\ta\tskip\textra\n")};
    EXPECT_EQ(too_many.line(), 1U);

    const scilex::grammar_error empty_pattern {refusal("A\t\tskip\n")};
    EXPECT_EQ(empty_pattern.column(), 3U);
    EXPECT_EQ(empty_pattern.cause(), "empty pattern");

    const scilex::grammar_error bad_regex {refusal("ID\t[a-z]+\nBAD\ta(\n")};
    EXPECT_EQ(bad_regex.line(), 2U);
    EXPECT_EQ(bad_regex.column(), 6U); // the pattern starts at column 5, and the engine points at its (
    EXPECT_EQ(bad_regex.cause().rfind("invalid regex: ", 0), 0U);

    const scilex::grammar_error unknown {refusal("A\ta\tskip  bogus\n")};
    EXPECT_EQ(unknown.column(), 11U);
    EXPECT_EQ(unknown.cause(), "unknown option 'bogus' (expected skip, in=, push=, set= or pop)");

    const scilex::grammar_error two {refusal("A\ta\tpush=x pop\n")};
    EXPECT_EQ(two.column(), 12U);

    EXPECT_EQ(refusal("A\ta\tpush=\n").cause(), "empty mode name in 'push='");
    EXPECT_EQ(refusal("A\ta\tset=\n").cause(), "empty mode name in 'set='");
    EXPECT_EQ(refusal("A\ta\tin=x,,y\n").cause(), "empty mode name in 'in=x,,y'");

    const scilex::grammar_error none {refusal("# only a comment\n\n")};
    EXPECT_EQ(none.line(), 0U);
    EXPECT_EQ(std::string(none.what()), "g.lex: no rules (the grammar is empty)");
  }

  TEST(grammar_loads_a_file_and_names_a_missing_one)
  {
    const std::string path {(std::filesystem::temp_directory_path() / "scilex_test_grammar_load.lex").string()};
    {
      std::ofstream out {path, std::ios::binary};
      out << "N\t[0-9]+\n";
    }
    const scilex::grammar g {scilex::load_grammar(path)};
    EXPECT_EQ(g.names[0], "N");
    static_cast<void>(std::remove(path.c_str()));

    const std::string missing {(std::filesystem::temp_directory_path() / "scilex_no_such_grammar.lex").string()};
    bool              refused {false};
    try {
      static_cast<void>(scilex::load_grammar(missing));
    }
    catch (const scilex::grammar_error& error) {
      refused = std::string(error.what()) == missing + ": cannot open grammar file";
    }
    EXPECT(refused);
  }
} // namespace
