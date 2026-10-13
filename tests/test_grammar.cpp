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
                                                   "END\t\"\tin=str  pop\nMID\tx\tin=default,str set=other skip\n"
                                                   "OTHER\ty\tin=other\n")};
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

    // The blanks that end a line are dropped, so a pattern of spaces vanishes: named, not a field count.
    const scilex::grammar_error blank_pattern {refusal("A\ta\nSPACE\t \n")};
    EXPECT_EQ(blank_pattern.line(), 2U);
    EXPECT_EQ(blank_pattern.column(), 7U);
    EXPECT_EQ(blank_pattern.cause().rfind("empty pattern: the blanks that end a line are dropped", 0), 0U);
    EXPECT_EQ(scilex::parse_grammar("SPACE\t[ ]\t\n").rules.size(), 1U); // the spelling the cause names

    const scilex::grammar_error bad_regex {refusal("ID\t[a-z]+\nBAD\ta(\n")};
    EXPECT_EQ(bad_regex.line(), 2U);
    EXPECT_EQ(bad_regex.column(), 6U); // the pattern starts at column 5, and the engine points at its (
    EXPECT_EQ(bad_regex.cause().rfind("invalid regex: ", 0), 0U);

    const scilex::grammar_error unknown {refusal("A\ta\tskip  bogus\n")};
    EXPECT_EQ(unknown.column(), 11U);
    EXPECT_EQ(unknown.cause(), "unknown option 'bogus' (expected skip, in=, push=, set=, pop, after=, not_after=, indent=, "
              "extend=, continue= or fragment)");

    const scilex::grammar_error two {refusal("A\ta\tpush=x pop\n")};
    EXPECT_EQ(two.column(), 12U);

    EXPECT_EQ(refusal("A\ta\tpush=\n").cause(), "empty mode name in 'push='");
    EXPECT_EQ(refusal("A\ta\tset=\n").cause(), "empty mode name in 'set='");
    EXPECT_EQ(refusal("A\ta\tin=x,,y\n").cause(), "empty mode name in 'in=x,,y'");
    EXPECT_EQ(refusal("A\ta\tpush=x,y\n").cause(), "a mode name cannot hold a comma: 'push=x,y'");

    const scilex::grammar_error none {refusal("# only a comment\n\n")};
    EXPECT_EQ(none.line(), 0U);
    EXPECT_EQ(std::string(none.what()), "g.lex: no rules (the grammar is empty)");
  }

  // Level B: the roles and extensions are read by name, continue= resolves to a fragment (else a rule) wherever
  // it is written, and a fragment is a pattern only: no kind, not lexed, no other option.
  TEST(grammar_reads_the_indentation_options)
  {
    const scilex::grammar parsed {scilex::parse_grammar("DASH\t-\tindent=entry\n"
                                                        "COLON\t:\tindent=value\n"
                                                        "DOC\t---\tindent=reset\n"
                                                        "BLOCK\t[|>]\textend=block\n"
                                                        "WORD\t[a-z]+\tindent=key extend=lines continue=MORE\n"
                                                        "MORE\t[a-z-]+\tfragment\n"
                                                        "NAME\t[A-Z]+\textend=lines continue=WORD\n")};
    EXPECT_EQ(parsed.rules.size(), 6U);
    EXPECT_EQ(parsed.names.back(), "NAME");
    EXPECT(parsed.rules[0].indent == scilex::indent_role::entry);
    EXPECT(parsed.rules[1].indent == scilex::indent_role::value);
    EXPECT(parsed.rules[2].indent == scilex::indent_role::reset);
    EXPECT(parsed.rules[3].extend == scilex::extension::block);
    EXPECT(parsed.rules[4].indent == scilex::indent_role::key && parsed.rules[4].extend == scilex::extension::lines);
    EXPECT(parsed.rules[4].continuation && parsed.rules[4].continuation->match("a-b", 0).end() == 3);
    EXPECT(parsed.rules[5].continuation && parsed.rules[5].continuation->match("ab", 0).end() == 2);
    EXPECT_EQ(refusal("A\ta\tindent=side\n").cause(), "unknown role 'indent=side' (expected entry, key, value or reset)");
    EXPECT_EQ(refusal("A\ta\textend=far\n").cause(), "unknown extension 'extend=far' (expected block or lines)");
    EXPECT_EQ(refusal("A\ta\textend=lines continue=\n").cause(), "empty name in continue=");
    EXPECT_EQ(refusal("A\ta\textend=lines continue=B\n").cause(), "no fragment or rule is named 'B' (continue= names one)");
    const scilex::grammar_error unextended {refusal("A\ta\tcontinue=A\n")};
    EXPECT_EQ(unextended.column(), 5U);
    EXPECT_EQ(unextended.cause(), "continue= reads the lines an extend=lines rule continues on");
    const scilex::grammar_error busy {refusal("A\ta\nF\tb\tskip fragment\n")};
    EXPECT_EQ(busy.line(), 2U);
    EXPECT_EQ(busy.cause(), "a fragment takes no other option");
  }

  // A mode that push= or set= enters must have a rule active in it, or the lexer refuses the grammar with
  // no line; the parse refuses it at the option. A byte order mark is not part of the first rule's name.
  TEST(grammar_a_transition_enters_a_mode_with_rules)
  {
    const scilex::grammar_error push {refusal("A\ta\nS\t\"\tpush=strr\nT\t[^\"]+\tin=str\n")};
    EXPECT_EQ(push.line(), 2U);
    EXPECT_EQ(push.column(), 5U);
    EXPECT_EQ(push.cause(), "no rule is active in mode 'strr' (give one in=strr)");
    const scilex::grammar_error set {refusal("A\ta\tskip set=b\n")};
    EXPECT_EQ(set.line(), 1U);
    EXPECT_EQ(set.column(), 10U);
    EXPECT_EQ(set.cause(), "no rule is active in mode 'b' (give one in=b)");
    // `default` holds the rules with no in=; a mode named in in= holds its rules.
    EXPECT_EQ(scilex::parse_grammar("A\ta\tin=x set=default\nB\tb\tset=x\n").rules.size(), 2U);

    const scilex::grammar bom {scilex::parse_grammar("\xEF\xBB\xBFWS\t\\s+\tskip\nID\t[a-z]+\n")};
    EXPECT_EQ(bom.names[0], "WS");
    EXPECT_EQ(refusal("\xEF\xBB\xBF" "A\ta(\n").column(), 4U); // columns count from after the mark
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

  // %insignificant names the modes whose tokens carry no layout structure; it is not a rule, so the kinds
  // after it keep their positions among the rules, and the lexer built from it marks those modes.
  TEST(grammar_insignificant_directive_names_layout_free_modes)
  {
    const scilex::grammar g {scilex::parse_grammar("%insignificant br\nOPEN\t\\(\tpush=br\nCLOSE\t\\)\tin=br pop\n"
                                                   "  %insignificant\tdefault,br\nX\t[a-z]+\tin=default,br\n")};
    EXPECT_EQ(g.rules.size(), 3U);
    EXPECT_EQ(g.names[2], "X");
    EXPECT_EQ(g.rules[2].kind, 2);
    EXPECT(g.insignificant_modes == (std::vector<std::string> {"br", "default", "br"}));
    const scilex::lexer              lex    {g.rules, g.insignificant_modes};
    const std::vector<scilex::token> tokens {lex.tokenize("f(x)")};
    EXPECT(!lex.mode_significant().empty());
    for (const scilex::token& token : tokens) {
      if (token.kind == 2 && !lex.mode_significant().empty()) {
        EXPECT(!lex.mode_significant()[token.mode_id]);
      }
    }
    EXPECT(scilex::parse_grammar("X\tx\n").insignificant_modes.empty());
  }

  TEST(grammar_directive_refusals_name_their_line_and_column)
  {
    const scilex::grammar_error unknown {refusal("A\ta\n%insignifcant x\n")};
    EXPECT_EQ(unknown.line(), 2U);
    EXPECT_EQ(unknown.column(), 1U);
    EXPECT_EQ(unknown.cause(), "unknown directive '%insignifcant' (expected %insignificant)");
    const scilex::grammar_error empty {refusal("A\ta\n%insignificant   \n")};
    EXPECT_EQ(empty.column(), 15U);
    EXPECT_EQ(empty.cause(), "%insignificant names at least one mode");
    const scilex::grammar_error malformed {refusal("A\ta\tin=a,b\n%insignificant a,,b\n")};
    EXPECT_EQ(malformed.column(), 18U);
    EXPECT_EQ(malformed.cause(), "malformed mode list 'a,,b' (write m1,m2)");
    EXPECT_EQ(refusal("A\ta\n%insignificant a b\n").cause(), "malformed mode list 'a b' (write m1,m2)");
    const scilex::grammar_error inactive {refusal("A\ta\tin=a\n%insignificant a,nope\n")};
    EXPECT_EQ(inactive.line(), 2U);
    EXPECT_EQ(inactive.column(), 18U);
    EXPECT_EQ(inactive.cause(), "no rule is active in mode 'nope' (give one in=nope)");
  }

  // after= and not_after= name rules (each name every rule bearing it, earlier or later) or ^, the start.
  TEST(grammar_predicates_name_rules_and_the_start)
  {
    const scilex::grammar g {scilex::parse_grammar("WS\t\\s+\tskip\nID\t[a-z]+\nNUM\t[0-9]+\n"
                                                   "DIV\t/\tafter=ID,NUM,CLOSE\nRE\t/[^/]+/\tnot_after=ID,NUM,CLOSE\n"
                                                   "CLOSE\t\\)\nCLOSE\t\\]\nHASH\t#!.*\tafter=^\nOP\t#\n")};
    EXPECT(g.rules[3].after == (std::vector<int> {1, 2, 5, 6})); // both CLOSE rules
    EXPECT(g.rules[3].not_after.empty());
    EXPECT(g.rules[4].not_after == (std::vector<int> {1, 2, 5, 6}));
    EXPECT(g.rules[7].after == (std::vector<int> {scilex::end_of_input}));
    const scilex::lexer lex {g.rules};
    std::vector<int>    kinds;
    for (const scilex::token& t : lex.tokenize("a / 2 # /x/ ] / 1")) { // after `#`, not an operand: a regex
      kinds.push_back(t.kind);
    }
    EXPECT(kinds == (std::vector<int> {1, 3, 2, 8, 4, 6, 3, 2}));
    std::vector<int> start;
    for (const scilex::token& t : lex.tokenize("#!x #")) {
      start.push_back(t.kind);
    }
    EXPECT(start == (std::vector<int> {7})); // HASH runs to the end of the line
  }

  TEST(grammar_predicate_refusals_name_their_line_and_column)
  {
    const scilex::grammar_error unknown {refusal("A\ta\nB\tb\tafter=A,Z\n")};
    EXPECT_EQ(unknown.line(), 2U);
    EXPECT_EQ(unknown.column(), 5U);
    EXPECT_EQ(unknown.cause(), "no rule is named 'Z' (after= and not_after= name rules, or ^)");
    EXPECT_EQ(refusal("A\ta\tafter=A not_after=A\n").cause(), "a rule takes one of after= and not_after=, once");
    EXPECT_EQ(refusal("A\ta\tafter=A,\n").cause(), "empty rule name in 'after=A,'");
  }
} // namespace
