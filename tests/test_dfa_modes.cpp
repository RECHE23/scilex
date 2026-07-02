// DFA-accelerated modes (lexer dfa_modes) — robustness suite.
//
// The fast path must be invisible: an opted-in DFA mode lexes byte-identically to the
// Pike path, and a mode whose rules a DFA cannot represent (an assertion) or cannot
// reproduce (a lazy quantifier) silently falls back to Pike, absent from
// dfa_modes_active(). Every check is non-vacuous (asserts a positive compare count).
//
// Self-contained by design: the grammars are built inline and the oracle is an inline
// brute-force munch, so the test pulls in no example/fuzz header (those carry their own
// conventions and are out of this library's lint scope).
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sciforge/test/framework.hpp>

#include <scilex/layout.hpp>
#include <scilex/scilex.hpp>

namespace {

  using scilex::rule;
  using scilex::token;

  //! \brief The independent oracle: a mono-mode brute-force munch — at each cursor try
  //!        EVERY rule (no dispatch, no DFA), longest wins, earliest on a tie, empty
  //!        excluded; byte-by-byte position; skip rules consumed. The DFA and the
  //!        lexer's Pike path must both equal this.
  std::vector<token> brute_munch(const std::vector<rule>& rules,
                                 std::string_view         src)
  {
    std::vector<token> out;
    scilex::position   cursor {.offset = 0, .line = 1, .column = 1};
    while (cursor.offset < src.size()) {
      const std::string_view rest     {src.substr(cursor.offset)};
      std::size_t            best_len {0};
      const rule*            best     {nullptr};
      for (const rule& candidate : rules) {
        const auto        matched {candidate.pattern.match(rest)};
        const std::size_t len     {matched ? matched.end() : std::size_t {0}};
        if (len > 0 && len > best_len) { // strictly greater ⇒ earliest rule wins a tie
          best_len = len;
          best     = &candidate;
        }
      }
      if (best == nullptr) {
        throw scilex::lex_error("no rule matches (reference)", cursor);
      }
      const scilex::position start {cursor};
      for (std::size_t i {0}; i < best_len; ++i) {
        if (src[cursor.offset + i] == '\n') {
          ++cursor.line;
          cursor.column = 1;
        }
        else {
          ++cursor.column;
        }
      }
      cursor.offset += best_len;
      if (!best->skip) {
        out.push_back(token {.kind = best->kind, .lexeme = src.substr(start.offset, best_len), .start = start});
      }
    }
    return out;
  }

  bool tokens_equal(const std::vector<token>& a,
                    const std::vector<token>& b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i {0}; i < a.size(); ++i) {
      if (a[i].kind != b[i].kind || a[i].lexeme != b[i].lexeme
          || a[i].start.offset != b[i].start.offset || a[i].start.line != b[i].start.line
          || a[i].start.column != b[i].start.column) {
        return false;
      }
    }
    return true;
  }

  bool active_has(const scilex::lexer& lex,
                  const std::string&   mode)
  {
    const std::vector<std::string> active {lex.dfa_modes_active()};
    return std::ranges::find(active, mode) != active.end();
  }

  std::size_t count_layout_structure(const std::vector<token>& toks)
  {
    std::size_t n {0};
    for (const token& tok : toks) {
      n += static_cast<std::size_t>(tok.kind == scilex::newline || tok.kind == scilex::indent
                                    || tok.kind == scilex::dedent);
    }
    return n;
  }

  rule plain(int              kind,
             const char*      pattern,
             bool             skip = false)
  {
    // These are ASCII-by-spec grammars used to exercise the DFA fast path; pin real::flags::ascii so
    // \s / \w / \d stay byte-level (a text-mode Unicode shorthand compiles to a code-point predicate
    // the DFA cannot represent, which would silently demote the mode to the general scan).
    return rule {.kind = kind, .pattern = real::regex(pattern, real::flags::ascii), .skip = skip};
  }

  //! \brief A SQL-ish mono-mode grammar (keywords overlap ident on letters — the DFA's
  //!        strongest case), all greedy / assertion-free / lazy-free, hence DFA-able.
  std::vector<rule> sql_like()
  {
    return {plain(0, R"(\s+)", true), plain(1, "select"), plain(2, "from"), plain(3, "set"),
            plain(4, "[A-Za-z_][A-Za-z0-9_]*"), plain(5, R"([0-9]+(\.[0-9]+)?)"),
            plain(6, "'([^']|'')*'"), plain(7, "<>|<=|>=|[-+*/<>=]"), plain(8, "[(),.;]")};
  }

  //! \brief A CSS-ish mono-mode grammar (dimension/percentage/number structure).
  std::vector<rule> css_like()
  {
    return {plain(0, R"(\s+)", true), plain(1, "@[A-Za-z][A-Za-z-]*"), plain(2, "#[A-Za-z0-9_-]+"),
            plain(3, R"(-?[0-9]+(\.[0-9]+)?(px|em|rem|vh|vw))"), plain(4, R"(-?[0-9]+(\.[0-9]+)?%)"),
            plain(5, R"(-?[0-9]+(\.[0-9]+)?)"), plain(6, "-?[A-Za-z_][A-Za-z0-9_-]*"),
            plain(7, R"([{}():;,])"), plain(8, R"([-.*>+~/])")};
  }

  //! \brief DFA lexer ≡ Pike lexer ≡ brute-force reference over inputs; returns the
  //!        number of tokens compared (for the non-vacuity assertion).
  std::size_t expect_equivalent(const std::vector<rule>&             rules,
                                const std::vector<std::string_view>& inputs)
  {
    const scilex::lexer pike {rules};
    const scilex::lexer dfa  {rules, {}, {"default"}};
    EXPECT(active_has(dfa, "default")); // the mode must actually be accelerated here
    std::size_t compared     {0};
    for (const std::string_view input : inputs) {
      const std::vector<token> a {pike.tokenize(input)};
      const std::vector<token> b {dfa.tokenize(input)};
      EXPECT(tokens_equal(a, b));                         // DFA == Pike
      EXPECT(tokens_equal(brute_munch(rules, input), b)); // reference == DFA
      compared += b.size();
    }
    return compared;
  }
} // namespace

// SQL-ish default mode (greedy, assertion-free) is accelerated and byte-identical.
TEST(dfa_modes_sql_default_equivalent)
{
  const std::vector<std::string_view> corpus {
    "select id from users set x where id >= 10 and name <> 'x';",
    "select SeLeCt selects from_table 1 1.5 'it''s' <> <= >= + - * / set",
    "create_t a int b text 'q' 1 2 3 4.5 6.7 'a''b''c' from set select",
    "x = x + 1 y < 100 z null w not null a b c d e f g h <> <= >= 'str'",
    "(a, b, c) select from set; id.name; 1 + 2 - 3 * 4 / 5 <> 6 <= 7 >= 8;",
    "select select from from set set a b c d e f g 1 2 3 4 5 6 7 8 9 'p' 'q';",
  };
  EXPECT(expect_equivalent(sql_like(), corpus) > 100); // non-vacuous
}

// CSS-ish default mode (dimension/percentage/number/hash structure) — same.
TEST(dfa_modes_css_default_equivalent)
{
  const std::vector<std::string_view> corpus {
    "btn { color: #1a2b3c; width: 12px; opacity: 50%; margin: 0 auto; }",
    "@media #id .class { font-size: 1.5em } -5 -5px 1.5 12px 50% 2vh 3vw 1rem",
    "div > p + span ~ a { margin: -1.5em 0 2px 50%; padding: 0 } #fff a-b-c",
    "a { x: 1px; y: 2em; z: 3rem; w: 4vh; v: 5vw } #aaa #bbb #ccc 10% 20% 30%",
    "x > y + z ~ w / a . b * c { p: -1; q: -2px; r: 3.5 } @media @import #fff;",
  };
  EXPECT(expect_equivalent(css_like(), corpus) > 100);
}

// An unknown dfa_mode name is rejected at construction (the C++ ctor validates it).
TEST(dfa_modes_unknown_mode_throws)
{
  bool threw {false};
  try {
    const scilex::lexer lex {sql_like(), {}, {"nonexistent"}};
    (void)lex;
  }
  catch (const std::invalid_argument&) {
    threw = true;
  }
  EXPECT(threw);
}

// A DFA-accelerated mode still raises lex_error at an unmatchable byte: the DFA
// returning no match drives the same #1 error as the Pike path.
TEST(dfa_modes_accelerated_lex_error_on_no_match)
{
  const std::vector<rule> rules {plain(0, R"(\s+)", true), plain(1, "[a-z]+")};
  const scilex::lexer     dfa   {rules, {}, {"default"}};
  EXPECT(active_has(dfa, "default"));
  bool threw                    {false};
  try {
    (void)dfa.tokenize("abc 123"); // '1' matches no rule → the DFA yields no match
  }
  catch (const scilex::lex_error&) {
    threw = true;
  }
  EXPECT(threw);
}

// Fallback #1 — a non-head assertion ($) makes the mode un-DFA-able: real::dfa_error is
// caught, the mode stays on Pike (absent from active), and tokens are still correct.
TEST(dfa_modes_fallback_on_assertion)
{
  const std::vector<rule> rules {plain(0, R"(\s+)", true), plain(1, "end$"), plain(2, "[a-z]+")};
  const scilex::lexer     dfa   {rules, {}, {"default"}};
  EXPECT(!active_has(dfa, "default")); // rejected via dfa_error → Pike
  EXPECT(dfa.dfa_modes_active().empty());

  const scilex::lexer pike {rules};
  EXPECT(tokens_equal(pike.tokenize("foo end"), dfa.tokenize("foo end")));
  EXPECT(tokens_equal(brute_munch(rules, "foo end"), dfa.tokenize("foo end")));
}

// Fallback #2 — a lazy quantifier builds a DFA with no error, but the build-time audit
// detects the longest-vs-shortest divergence and falls back to Pike.
TEST(dfa_modes_fallback_on_lazy_quantifier)
{
  const std::vector<rule> rules {plain(0, R"(\s+)", true), plain(1, R"rx((?s)""".*?""")rx"),
                                 plain(2, "[a-z]+")};
  const scilex::lexer     dfa   {rules, {}, {"default"}};
  EXPECT(!active_has(dfa, "default")); // audit caught the lazy rule → Pike

  const scilex::lexer pike {rules};
  for (const std::string_view input : {std::string_view {R"(a """x""" b """y""")"},
                                       std::string_view {R"("""only""")"}}) {
    EXPECT(tokens_equal(pike.tokenize(input), dfa.tokenize(input)));
    EXPECT(tokens_equal(brute_munch(rules, input), dfa.tokenize(input)));
  }
}

// layout() is unchanged by DFA acceleration: same source, dfa_modes on vs off, the
// laid-out stream (NEWLINE/INDENT/DEDENT included) is byte-identical.
TEST(dfa_modes_layout_non_regression)
{
  const std::vector<rule> rules {plain(1, R"(\s+)", true), plain(2, "[A-Za-z_]+"),
                                 plain(3, "[0-9]+"), plain(4, ":")};
  const scilex::lexer     off   {rules};
  const scilex::lexer     on    {rules, {}, {"default"}};
  EXPECT(active_has(on, "default"));

  const std::string_view   src      {"a:\n    b 1\n    c 2\nd\n"};
  const std::vector<token> laid_off {scilex::layout(off.tokenize(src, scilex::eof_policy::append))};
  const std::vector<token> laid_on  {scilex::layout(on.tokenize(src, scilex::eof_policy::append))};
  EXPECT(tokens_equal(laid_off, laid_on));
  EXPECT(count_layout_structure(laid_on) > 0); // non-vacuous: layout actually shaped indentation
}

// insignificant_modes ∩ dfa_modes — orthogonal: a mode can be both. The DFA recognizes
// its tokens (identical stream, spans included) while layout() still treats it as
// insignificant (its lines add no NEWLINE/INDENT/DEDENT).
TEST(dfa_modes_orthogonal_to_insignificant_modes)
{
  using op = scilex::mode_action::op;
  rule ws   {.kind = 1, .pattern = real::regex(R"(\s+)", real::flags::ascii), .skip = true};
  ws.in_mode = {"default", "flow"};
  rule name {.kind = 2, .pattern = real::regex("[A-Za-z]+")};
  name.in_mode = {"default", "flow"};
  rule open {.kind = 3, .pattern = real::regex(R"(\[)")};
  open.in_mode = {"default"};
  open.action  = scilex::mode_action {.operation = op::push, .target = "flow"};
  rule close {.kind = 4, .pattern = real::regex(R"(\])")};
  close.in_mode = {"flow"};
  close.action  = scilex::mode_action {.operation = op::pop};
  rule comma                    {.kind = 5, .pattern = real::regex(",")};
  comma.in_mode = {"flow"};
  const std::vector<rule> rules {ws, name, open, close, comma};

  const scilex::lexer off       {rules, {"flow"}, {}};       // flow insignificant only
  const scilex::lexer on        {rules, {"flow"}, {"flow"}}; // flow insignificant AND DFA-accelerated
  EXPECT(active_has(on, "flow"));                            // flow's rules are DFA-able

  const std::string_view src {"a [\n    b,\n    c\n] d\n"};
  EXPECT(tokens_equal(off.tokenize(src, scilex::eof_policy::append),
                      on.tokenize(src, scilex::eof_policy::append)));
  const std::vector<token> laid_off {
    scilex::layout(off.tokenize(src, scilex::eof_policy::append), off.mode_significant())};
  const std::vector<token> laid_on  {
    scilex::layout(on.tokenize(src, scilex::eof_policy::append), on.mode_significant())};
  EXPECT(tokens_equal(laid_off, laid_on));
}
