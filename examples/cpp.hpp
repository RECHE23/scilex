/*!
 * \file cpp.hpp
 * \brief A C++ lexer grammar — the operator-dense maximal-munch example.
 *
 * C++ has the richest operator set of the suite, so it is the stress test for
 * maximal munch: multi-byte operators (`<<=`, `->*`, `::`, `...`, `<=>`) must be
 * recognized whole rather than split into their single-byte pieces. It reads C++
 * as Clang's lexer does (`make cpp-oracle` compares the two over the headers of the
 * C++ standard library): preprocessing numbers (`1'000`, `0x1p-3`, `.5f`), encoding
 * prefixes and user-defined suffixes, every punctuator, line splices, and each
 * preprocessing directive as one token, to the end of its logical line.
 *
 * Honest scope: a raw string literal is read only with an empty delimiter
 * (`R"( … )"`); with one (`R"d( … )d"`) the closing sequence must repeat it, which
 * no regular rule can. The digraphs `<:` and `:>` are not punctuators here (see
 * the operator rule). Template `<…>` versus less-than is a parser concern, not the
 * lexer's; both are simply the `<` / `>` operators here.
 */
#ifndef SCILEX_EXAMPLE_CPP_HPP
#define SCILEX_EXAMPLE_CPP_HPP

#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::cpp {

  //! \brief C++-like token kinds (whitespace and comments are skip rules).
  enum kind
  {
    ws,
    line_comment,
    block_comment,
    preproc,
    keyword,
    ident,
    number,
    char_lit,
    string_lit,
    op,
    punct,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case preproc:    return "PREPROC";
      case keyword:    return "KEYWORD";
      case ident:      return "NAME";
      case number:     return "NUMBER";
      case char_lit:   return "CHAR";
      case string_lit: return "STRING";
      case op:         return "OP";
      case punct:      return "PUNCT";
      default:         return "?";
    }
  }

  //! \brief Builds the C++-like lexer. Keyword rules precede \ref ident (munch
  //!        tie-break); the operator rule lists multi-byte forms before the
  //!        single-byte class so the longest operator always wins.
  inline std::vector<scilex::rule> make_rules()
  {
    // An encoding prefix and a user-defined suffix, shared by the character and string literals.
    constexpr std::string_view prefix    {"(u8|u|U|L)?"};
    constexpr std::string_view suffix    {"([A-Za-z_][A-Za-z0-9_]*)?"};
    // A string or character literal inside a directive, its closing quote optional (`#error don't`).
    constexpr std::string_view quoted    {R"re("(\\[\s\S]|[^"\\\n])*"?|'(\\[\s\S]|[^'\\\n])*'?)re"};
    constexpr std::string_view block     {R"re(/\*([^*]|\*+[^*/])*\*+/)re"};
    const std::string          directive {
      // `#`, then to the end of the logical line: a backslash splices the next line in, and a string, a character
      // literal or a comment is read whole (a block comment may run over lines). A `/` opens a comment only when
      // a `*` or a `/` follows, so the other kinds of element each start with something else.
      std::string {R"re(#([^\n\\/"']|\\[\s\S]|)re"} + std::string {quoted} + "|" + std::string {block} +
      R"re(|//([^\n\\]|\\[\s\S])*|/([^*/\n\\"']|\\[\s\S]|)re" + std::string {quoted} + "))*/?"};
    std::vector<scilex::rule> rules;
    // Blanks, and a backslash that splices the next line on.
    rules.push_back({ws, real::regex(R"re((\s|\\\r?\n)+)re", real::flags::ascii), true});
    // A comment, and so a directive, holds any byte, as Clang and GCC read them (a Latin-1 `©` in a licence).
    rules.push_back({line_comment, real::regex(R"re(//([^\n\\]|\\[\s\S])*)re", real::flags::bytes), true});
    rules.push_back({block_comment, real::regex(std::string {block}, real::flags::bytes), true});
    rules.push_back({preproc, real::regex(directive, real::flags::bytes), false});
    for (const char* word : {"alignas", "alignof", "asm", "auto", "bool", "break", "case", "catch", "char",
                             "char8_t", "char16_t", "char32_t", "class", "co_await", "co_return", "co_yield",
                             "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
                             "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
                             "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
                             "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr",
                             "operator", "private", "protected", "public", "register", "reinterpret_cast",
                             "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
                             "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
                             "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
                             "virtual", "void", "volatile", "wchar_t", "while"}) {
      rules.push_back({keyword, real::regex(word), false});
    }
    // As Clang and GCC read them: XID letters and digits, and `$`.
    rules.push_back({ident, real::regex(R"re([\p{XID_Start}_$][\p{XID_Continue}$]*)re"), false});
    // A preprocessing number, the lexical form of every C++ number: `1'000`, `0x1p-3`, `.5f`, `12_km`. The
    // first alternative that matches is taken, so an exponent's sign is tried before the letter alone.
    rules.push_back({number, real::regex(R"re(\.?[0-9]([eEpP][+-]|[0-9A-Za-z_.]|'[0-9A-Za-z_])*)re"), false});
    rules.push_back({char_lit, real::regex(std::string {prefix} + R"re('(\\[\s\S]|[^'\\\n])*')re" + std::string {suffix}), false});
    rules.push_back({string_lit, real::regex(std::string {prefix} + R"re("(\\[\s\S]|[^"\\\n])*")re" + std::string {suffix}), false});
    // A raw string with an empty delimiter, R"( … )": a delimiter needs the closing sequence to repeat it.
    rules.push_back({string_lit, real::regex(std::string {prefix} + R"re(R"\(([^)]|\)+[^)"])*\)+")re" + std::string {suffix}), false});
    // Every punctuator; munch takes the longest. Of the digraphs, `<:` and `:>` are left out: `vector<::T>` is
    // `<` then `::` because of the character after them, which munch does not consult, and code uses them rarely.
    rules.push_back({op, real::regex(R"re(%:%:|\.\.\.|<=>|->\*|<<=|>>=|::|\.\*|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|##|<%|%>|%:|[-+*/%<>=&|^~!?.:#])re"), false});
    rules.push_back({punct, real::regex(R"re([{}()\[\];,])re"), false});
    return rules;
  }

  //! \brief make_lexer() under the error policy and column unit given, its modes and DFA unchanged.
  //! \param[in] errors  What to do at a byte no rule can lex (see \ref scilex::lexer).
  //! \param[in] columns The unit token columns are counted in (see \ref scilex::lexer).
  //! \return The lexer.
  inline scilex::lexer make_lexer_with(scilex::error_policy errors,
                                       scilex::column_unit  columns)
  {
    return scilex::lexer(make_rules(), {}, {}, errors, columns);
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return make_lexer_with(scilex::error_policy::raise, scilex::column_unit::bytes);
  }

  //! \brief A snippet exercising scope (`::`), shift-assign (`<<=`),
  //!        pointer-to-member (`->*`), a conditional's `:`, comments, literals,
  //!        and a `#` line.
  inline constexpr std::string_view sample {
    R"cpp(#include <cstdint>
// scope, shift-assign and pointer-to-member operators
namespace demo {
  struct P { int m; };
  int f(P* p, int P::* pm, int x) {
    /* a block comment */
    x <<= 2;
    return x > 1'000 ? (p->*pm) + x : 0;
  }
}
)cpp"};

  //! \brief Self-check (so `make example` gates): the distinctive invariant is
  //!        that the dense multi-byte operators munch whole — `<<=`, `->*` and
  //!        `::` each appear as one OP token, not split. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex          {make_lexer()};
    const std::vector<scilex::token> toks         {lex.tokenize(sample)};
    bool                             shift_assign {false};
    bool                             arrow_star   {false};
    bool                             scope        {false};
    for (const scilex::token& tok : toks) {
      if (tok.kind != op) {
        continue;
      }
      shift_assign = shift_assign || (tok.lexeme == "<<=");
      arrow_star   = arrow_star || (tok.lexeme == "->*");
      scope        = scope || (tok.lexeme == "::");
    }
    return shift_assign && arrow_star && scope;
  }
} // namespace scilex::examples::cpp

#endif // SCILEX_EXAMPLE_CPP_HPP
