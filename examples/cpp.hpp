/*!
 * \file cpp.hpp
 * \brief A C++-like lexer grammar — the operator-dense maximal-munch example.
 *
 * C++ has the richest operator set of the suite, so it is the stress test for
 * maximal munch: multi-byte operators (`<<=`, `->*`, `::`, `...`, `<=>`) must be
 * recognized whole rather than split into their single-byte pieces. It also
 * exercises line and block comments, character and string
 * literals with escapes, numbers, and `#` preprocessor lines.
 *
 * Honest scope: this is a lexer-grain C++ subset. Raw string literals
 * (`R"delim( … )delim"`) are *not* covered — their delimiter makes the closing
 * sequence context-dependent, which needs mode-based lexing (a future SciLex
 * capability). Template `<…>` versus less-than is a parser concern, not the
 * lexer's; both are simply the `<` / `>` operators here.
 */
#ifndef SCILEX_EXAMPLE_CPP_HPP
#define SCILEX_EXAMPLE_CPP_HPP

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
  inline scilex::lexer make_lexer()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({line_comment, real::regex("//.*"), true});
    rules.push_back({block_comment, real::regex(R"re(/\*([^*]|\*+[^*/])*\*+/)re"), true});
    rules.push_back({preproc, real::regex("#.*"), false});
    for (const char* word : {"alignas", "auto", "bool", "break", "case", "char", "class", "const",
                             "constexpr", "continue", "double", "else", "enum", "float", "for",
                             "if", "int", "namespace", "new", "nullptr", "private", "public",
                             "return", "struct", "switch", "template", "typename", "using",
                             "virtual", "void", "while"}) {
      rules.push_back({keyword, real::regex(word), false});
    }
    rules.push_back({ident, real::regex("[A-Za-z_][A-Za-z0-9_]*"), false});
    // hex, or decimal with optional fraction / exponent and integer/float suffixes
    rules.push_back({number, real::regex("0[xX][0-9a-fA-F]+|[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?[fFuUlL]*"), false});
    rules.push_back({char_lit, real::regex(R"re('(\\.|[^'\\])*')re"), false});
    rules.push_back({string_lit, real::regex(R"re("(\\.|[^"\\])*")re"), false});
    // The munch stress: every multi-byte operator before the single-byte class.
    rules.push_back({op, real::regex(R"re(<<=|>>=|->\*|\.\.\.|<=>|::|->|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|[-+*/%<>=&|^~!?.])re"), false});
    rules.push_back({punct, real::regex(R"re([{}()\[\];,])re"), false});
    return scilex::lexer(std::move(rules));
  }

  //! \brief A snippet exercising scope (`::`), shift-assign (`<<=`),
  //!        pointer-to-member (`->*`), comments, literals, and a `#` line.
  inline constexpr std::string_view sample {
    R"cpp(#include <cstdint>
// scope, shift-assign and pointer-to-member operators
namespace demo {
  struct P { int m; };
  int f(P* p, int P::* pm, int x) {
    /* a block comment */
    x <<= 2;
    return (p->*pm) + x;
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
