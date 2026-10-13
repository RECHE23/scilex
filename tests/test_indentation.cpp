// Layout Awareness Level B: a token extended over the lines indented past the block it belongs to, the blocks
// tracked by the rules' indentation roles. The spans are PyYAML's (yaml.scan) on the same text: a block scalar
// from its indicator to the start of the line that ends it, a plain scalar to the last character of its last
// continued line. Each case is lexed whole, through a range, and as a stream fed one byte at a time.
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>
#include "scilex/scilex.hpp"

namespace {

  enum kind
  {
    blank = 1,
    comment,
    document,
    entry,
    value,
    block,
    scalar,
    open,
    close,
  };

  //! A small block language with YAML's roles: `-` entries, `:` values, `---` resets, `|`/`>` block bodies,
  //! and words continued on lines indented past their block. `[` … `]` is an insignificant mode.
  scilex::lexer make_lexer()
  {
    const std::vector<std::string> both {"default", "inner"};
    return scilex::lexer {
      {
        {.kind         = blank, .pattern = real::regex {"[ \\t\\r\\n]+"}, .skip = true, .in_mode = both},
        {.kind         = comment, .pattern = real::regex {"#[^\\n]*"}, .skip = true, .in_mode = both},
        {.kind         = document, .pattern = real::regex {"---|\\.\\.\\."}, .indent = scilex::indent_role::reset},
        {.kind         = entry, .pattern = real::regex {"-"}, .indent = scilex::indent_role::entry},
        {.kind         = value, .pattern = real::regex {":"}, .indent = scilex::indent_role::value, .in_mode = both},
        {.kind         = block, .pattern = real::regex {"[|>]([1-9][+-]?|[+-][1-9]?)?"}, .extend = scilex::extension::block},
        {.kind         = scalar,
         .pattern      = real::regex {"[A-Za-z0-9_]+( +[A-Za-z0-9_]+)*"},
         .indent       = scilex::indent_role::key,
         .extend       = scilex::extension::lines,
         .in_mode      = both,
         .continuation = real::regex {"[A-Za-z0-9_\"-]+( +[A-Za-z0-9_\"-]+)*"}},
        {.kind         = open, .pattern = real::regex {"\\["}, .indent = scilex::indent_role::key, .in_mode = both,
         .action       = scilex::mode_action {.operation = scilex::mode_action::op::push, .target = "inner"}},
        {.kind         = close, .pattern = real::regex {"\\]"}, .in_mode = {"inner"},
         .action       = scilex::mode_action {.operation = scilex::mode_action::op::pop}},
      },
      {"inner"}};
  }

  //! The block and scalar lexemes of \p text, lexed whole.
  std::vector<std::string> bodies(const std::vector<scilex::token>& tokens)
  {
    std::vector<std::string> out;
    for (const scilex::token& t : tokens) {
      if (t.kind == block || t.kind == scalar) {
        out.emplace_back(t.lexeme);
      }
    }
    return out;
  }

  //! Whether \p text lexes under \p lex to the bodies \p want whole, through a range, and as a stream fed
  //! byte by byte.
  bool lexes_with(const scilex::lexer&            lex,
                  std::string_view                text,
                  const std::vector<std::string>& want)
  {
    const std::vector<scilex::token> whole {lex.tokenize(text)};
    std::vector<scilex::token>       ranged;
    for (const scilex::token& t : lex.scan(text)) {
      ranged.push_back(t);
    }
    scilex::token_stream     in {lex.stream()};
    std::vector<std::string> streamed;
    std::vector<std::string> positions;
    const auto               keep {[&](const std::vector<scilex::token>& tokens) {
                                     for (const scilex::token & t : tokens) {
                                       if (t.kind == block || t.kind == scalar) {
                                         streamed.emplace_back(t.lexeme);
                                       }
                                       positions.push_back(std::to_string(t.start.offset));
                                     }
                                   }};
    for (std::size_t i {0}; i < text.size(); ++i) {
      keep(in.feed(text.substr(i, 1)));
    }
    keep(in.finish());
    std::vector<std::string> whole_positions;
    for (const scilex::token& t : whole) {
      whole_positions.push_back(std::to_string(t.start.offset));
    }
    return bodies(whole) == want && bodies(ranged) == want && streamed == want && positions == whole_positions;
  }

  //! Whether \p text lexes to the bodies \p want under the small block language (\ref make_lexer).
  bool lexes(std::string_view                text,
             const std::vector<std::string>& want)
  {
    return lexes_with(make_lexer(), text, want);
  }
} // namespace

// A block scalar runs to the line that ends it, its trailing blank lines included; an empty body keeps its
// line break; an indentation indicator sets the body's indentation past the enclosing block.
TEST(a_block_body_runs_to_the_first_line_indented_less)
{
  EXPECT(lexes("a: |\n  one\n  two\n\nb: 1\n", {"a", "|\n  one\n  two\n\n", "b", "1"}));
  EXPECT(lexes("a: >+\n  x\n\n\nb: 1\n", {"a", ">+\n  x\n\n\n", "b", "1"}));
  EXPECT(lexes("a: |\nb: 1\n", {"a", "|\n", "b", "1"}));
  EXPECT(lexes("a: |\n\n   x\nb: 1\n", {"a", "|\n\n   x\n", "b", "1"}));
  EXPECT(lexes("a: |2\n    x\nb: 1\n", {"a", "|2\n    x\n", "b", "1"}));
  EXPECT(lexes("k:\n  m: |-\n    body\n    more\n  n: x\n", {"k", "m", "|-\n    body\n    more\n", "n", "x"}));
  EXPECT(lexes("a: | # note\n  x\n", {"a", "| # note\n  x\n"}));
  EXPECT(lexes("a: |\r\n  x\r\nb: 1", {"a", "|\r\n  x\r\n", "b", "1"}));
  EXPECT(lexes("a: |\n  x", {"a", "|\n  x"}));
  EXPECT(lexes("a: |", {"a", "|"}));
}

// The reference is the innermost open block, not the line's indentation: a value inside a sequence entry
// is bounded by its key's column.
TEST(a_block_body_is_bounded_by_the_innermost_open_block)
{
  EXPECT(lexes("- k: |\n  j: v\n", {"k", "|\n", "j", "v"}));
  EXPECT(lexes("- k: |2\n      x\n  j: v\n", {"k", "|2\n      x\n", "j", "v"}));
  EXPECT(lexes("- |\n   s\n- q\n", {"|\n   s\n", "q"}));
  EXPECT(lexes("k:\n  - |\n    x\n  - y\n", {"k", "|\n    x\n", "y"}));
}

// A plain scalar continues on the lines indented past its block, each read by the continuation pattern; a
// comment, a key on the next line at the block's column, or a document marker ends it.
TEST(a_scalar_continues_on_the_lines_indented_past_its_block)
{
  EXPECT(lexes("a: one\n  two\n  three\nb: c\n", {"a", "one\n  two\n  three", "b", "c"}));
  EXPECT(lexes("- word\n  more words\n- x\n", {"word\n  more words", "x"}));
  EXPECT(lexes("- name: foo\n  value: bar\n", {"name", "foo", "value", "bar"}));
  EXPECT(lexes("key:\n  long text\n  continued\n", {"key", "long text\n  continued"}));
  EXPECT(lexes("x\ny\n", {"x\ny"}));
  EXPECT(lexes("a: x\n  - y\n", {"a", "x\n  - y"}));
  EXPECT(lexes("a: x\n  y # c\nb: 1\n", {"a", "x\n  y", "b", "1"}));
  EXPECT(lexes("a: x # c\n  y\n", {"a", "x", "y"}));
  EXPECT(lexes("a: x\n\n  y\n", {"a", "x\n\n  y"}));
  EXPECT(lexes("x\n---\ny\n", {"x", "y"}));
  EXPECT(lexes("--- x\n... \n", {"x"}));
}

// A key less indented than the open blocks closes them: the next body is bounded by the block that holds it.
// A header or a continued line that reaches the end of the text ends there.
TEST(a_less_indented_key_closes_the_blocks_deeper_than_it)
{
  EXPECT(lexes("a:\n  b:\n    c: |\n      x\nd: |\n  y\n", {"a", "b", "c", "|\n      x\n", "d", "|\n  y\n"}));
  EXPECT(lexes("a:\n  b:\n    c: x\n      y\nd: z\n", {"a", "b", "c", "x\n      y", "d", "z"}));
  EXPECT(lexes("a: |\n", {"a", "|\n"}));
  EXPECT(lexes("a: x\n  ", {"a", "x"}));
}

// The pure pieces the lexer and the fuzz oracle share: the reference an entry or a value gives its own
// extension is the block it opens, and a reset closes every block, whatever the column.
TEST(the_indentation_roles_open_and_close_blocks)
{
  const auto with {[](scilex::indent_role role) {
                     return scilex::rule {.kind = 1, .pattern = real::regex {"x"}, .indent = role, .extend = scilex::extension::block};
                   }};
  const auto at {[](std::size_t line, std::size_t column) {
                   return scilex::position {.offset = 0, .line = line, .column = column};
                 }};
  scilex::indent_state state {};
  EXPECT_EQ(scilex::indent_reference(with(scilex::indent_role::entry), at(1, 3), state), 2);
  scilex::apply_indent(with(scilex::indent_role::entry), at(1, 3), state);
  EXPECT_EQ(state.levels.back(), 2);
  scilex::apply_indent(with(scilex::indent_role::key), at(1, 5), state);
  EXPECT_EQ(scilex::indent_reference(with(scilex::indent_role::value), at(1, 6), state), 4);
  EXPECT_EQ(scilex::indent_reference(with(scilex::indent_role::value), at(2, 6), state), 2); // another line: no key
  scilex::apply_indent(with(scilex::indent_role::value), at(1, 6), state);
  EXPECT_EQ(state.levels.back(), 4);
  EXPECT_EQ(scilex::indent_reference(with(scilex::indent_role::reset), at(3, 9), state), -1);
  scilex::apply_indent(with(scilex::indent_role::none), at(3, 1), state);
  EXPECT_EQ(state.levels.size(), 1U);
}

// The edges: blanks that end the text are not a block's body; a value with no key opens no block, and a second
// key candidate on a line is not kept; a line that only begins like a document marker continues; a continued
// line its continuation does not match, or matches empty, ends the token.
TEST(the_indentation_edges_hold)
{
  EXPECT(lexes("a: |\n  ", {"a", "|\n"}));
  EXPECT(lexes("x\n---y\n", {"x\n---y"}));
  EXPECT(lexes("a: x\n  : y\n", {"a", "x", "y"}));
  EXPECT(lexes(": |\n x\n", {"|\n x\n"}));
  EXPECT(lexes("[a] b: |\n  x\n", {"a", "b", "|\n  x\n"}));
  const real::regex nullable {"z*"};
  EXPECT(scilex::lines_extent("x\n  y", 1, nullable, -1, true) == std::optional<std::size_t> {1});
  EXPECT(scilex::lines_extent("x\n  y", 1, nullable, -1, false) == std::nullopt);
  scilex::indent_state state {};
  scilex::apply_indent({.kind   = 1, .pattern = real::regex {"x"}, .indent = scilex::indent_role::value},
                       {.offset = 0, .line = 1, .column = 1}, state);
  EXPECT_EQ(state.levels.size(), 1U);
}

// Without an insignificant mode every mode takes part, and a rule continued with no continuation pattern
// reads its continued lines with its own.
TEST(every_mode_takes_part_without_an_insignificant_one)
{
  const scilex::lexer lex {{
    {.kind = blank, .pattern = real::regex {"[ \\n]+"}, .skip = true},
    {.kind = value, .pattern = real::regex {":"}, .indent = scilex::indent_role::value},
    {.kind = scalar, .pattern = real::regex {"[a-z]+"}, .indent = scilex::indent_role::key, .extend = scilex::extension::lines},
  }};
  EXPECT(lexes_with(lex, "a: b\n  c\nd: e", {"a", "b\n  c", "d", "e"}));
}

// A mode named insignificant takes no part: a word inside `[` … `]` is not continued, and a block after the
// collection is still bounded by the block that holds it.
TEST(an_insignificant_mode_takes_no_part_in_the_indentation)
{
  EXPECT(lexes("a: [x\n  y]\n", {"a", "x", "y"}));
  EXPECT(lexes("- [x]\n- k: |\n  j\n", {"x", "k", "|\n", "j"}));
}

// A skip rule emits nothing, so a role or an extension on one is refused when the lexer is built.
TEST(a_skip_rule_takes_no_part_in_the_indentation)
{
  EXPECT_THROWS(scilex::lexer({{.kind = 1, .pattern = real::regex {" +"}, .skip = true, .indent = scilex::indent_role::key}}),
                std::invalid_argument);
  EXPECT_THROWS(scilex::lexer({{.kind = 1, .pattern = real::regex {" +"}, .skip = true, .extend = scilex::extension::block}}),
                std::invalid_argument);
}
