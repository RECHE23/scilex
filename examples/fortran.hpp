/*!
 * \file fortran.hpp
 * \brief A free-form Fortran (F90 to F2018) lexer grammar — case-insensitive words and dotted operators.
 *
 * Fortran's lexical traps are the dot and the case. `1.eq.2` is the number `1`, the operator `.eq.` and
 * the number `2`, where `1.e5` is one number: the decimal point stops before a dotted word (a bounded
 * lookahead over the 31 letters a Fortran name may have). Keywords, dotted operators (`.AND.`, `.Eq.`, a
 * user-defined `.cross.`) and `.TRUE.` / `.false.` match in any case.
 *
 * Covered: `!` comments, `&` continuations (an optional comment after the `&`, an optional `&` leading the
 * next line), line breaks and `;` as statement separators, preprocessor lines (`#ifdef`), real literals
 * with `e`/`d`/`q` exponents and kind suffixes (`3.0_dp`, `1_int64`), BOZ literals (`z'1F'`, `b"101"`),
 * strings in either quote with doubled-quote escapes, `**` `//` `/=` `=>` `::`, `%` component access and
 * `(/ … /)` array constructors.
 *
 * Honest scope: fixed form (column 6 continuation, column 1 comments) is a different grammar; a string
 * continued across lines with `&` is not joined; keywords are words, not statements (`end do` is two
 * keywords, `enddo` one), which is what a parser wants.
 */
#ifndef SCILEX_EXAMPLE_FORTRAN_HPP
#define SCILEX_EXAMPLE_FORTRAN_HPP

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::fortran {

  //! \brief Fortran token kinds (whitespace, comments and continuations are skip rules).
  enum kind
  {
    ws,
    comment,
    continuation,
    newline,
    preprocessor,
    keyword,
    ident,
    number,
    logical, //!< `.true.` / `.false.`, any case, optionally with a kind
    dot_op,  //!< `.and.`, `.eq.`, … or a user-defined `.name.`
    string,
    op,
    lparen,
    rparen,
    lbracket,
    rbracket,
    comma,
    semicolon,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case newline:      return "NEWLINE";
      case preprocessor: return "PREPROCESSOR";
      case keyword:      return "KEYWORD";
      case ident:        return "IDENT";
      case number:       return "NUMBER";
      case logical:      return "LOGICAL";
      case dot_op:       return "DOT_OP";
      case string:       return "STRING";
      case op:           return "OP";
      case lparen:       return "(";
      case rparen:       return ")";
      case lbracket:     return "[";
      case rbracket:     return "]";
      case comma:        return ",";
      case semicolon:    return ";";
      default:           return "?";
    }
  }

  //! \brief The keywords. An alternation takes its first alternative that matches, so a keyword precedes
  //!        every shorter one it begins with (`inout` before `in`), or the longer one would lose to IDENT.
  constexpr const char* keywords {
    "program|module|submodule|contains|subroutine|function|use|only|implicit|none|integer|real|double"
    "|precision|complex|logical|character|type|class|intent|inout|out|parameter|allocatable|dimension"
    "|pointer|target|optional|save|if|then|elseif|else|endif|enddo|end|do|while|select|case|default|where"
    "|forall|call|return|stop|exit|cycle|allocate|deallocate|print|write|read|open|close|interface"
    "|procedure|pure|elemental|recursive|result|associate|block|concurrent|public|private|abstract|extends"
    "|go|to|in"};

  //! \brief Builds the Fortran rule list. KEYWORD precedes IDENT (an equal-length match is a keyword, a
  //!        longer name an identifier) and LOGICAL precedes DOT_OP (`.true.` is a constant, not an operator).
  inline std::vector<scilex::rule> make_rules()
  {
    const auto r {[](int k, const char* pattern, bool skip = false) {
                    return scilex::rule {.kind = k, .pattern = real::regex(pattern, real::flags::ascii), .skip = skip};
                  }};
    std::vector<scilex::rule> rules;
    rules.push_back(r(ws, R"re([ \t]+)re", true));
    rules.push_back(r(continuation, R"re(&[ \t]*(?:![^\n]*)?\r?\n(?:[ \t]*&)?)re", true));
    rules.push_back(r(comment, R"re(![^\n]*)re", true));
    rules.push_back(r(preprocessor, R"re((?m)^#[^\n]*)re"));
    rules.push_back(r(newline, R"re(\r?\n)re"));
    rules.push_back(r(keyword, (std::string {"(?i)"} + keywords).c_str()));
    rules.push_back(r(ident, R"re([A-Za-z][A-Za-z0-9_]*)re"));
    // The decimal point stops before a dotted word: `1.eq.2` is a number, an operator and a number.
    rules.push_back(r(number,
                      R"re((?i)[boz](?:'[0-9a-f]+'|"[0-9a-f]+")|(?:[0-9]+(?:\.(?![A-Za-z]{1,31}\.)[0-9]*)?|\.[0-9]+)(?:[eEdDqQ][+-]?[0-9]+)?(?:_[A-Za-z0-9_]+)?)re"));
    rules.push_back(r(logical, R"re((?i)\.(?:true|false)\.(?:_[A-Za-z0-9_]+)?)re"));
    rules.push_back(r(dot_op, R"re(\.[A-Za-z]{1,31}\.)re"));
    rules.push_back(r(string, R"re('(?:[^'\r\n]|'')*'|"(?:[^"\r\n]|"")*")re"));
    rules.push_back(r(op, R"re(\*\*|//|==|/=|<=|>=|=>|::|\(/|/\)|[-+*/=<>%:])re"));
    rules.push_back(r(lparen, R"re(\()re"));
    rules.push_back(r(rparen, R"re(\))re"));
    rules.push_back(r(lbracket, R"re(\[)re"));
    rules.push_back(r(rbracket, R"re(\])re"));
    rules.push_back(r(comma, R"re(,)re"));
    rules.push_back(r(semicolon, R"re(;)re"));
    return rules;
  }

  //! \brief make_lexer() under the error policy and column unit given, its DFA unchanged.
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

  //! \brief A module with every construct the grammar distinguishes: kinds, a continued statement with a
  //!        comment after its `&`, dotted operators in three cases, `1.eq.2` beside `1.e5`, BOZ literals,
  //!        logical constants, components, an array constructor and a preprocessor line.
  inline constexpr std::string_view sample {
    R"f90(#ifdef USE_MPI
module dynamics
  use, intrinsic :: iso_fortran_env, only: dp => real64
  implicit none
  integer, parameter :: mask = z'1F', bits = b"101"
contains
  pure function energy(p, n) result(e)  ! kinetic energy
    type(particle), intent(in) :: p(:)
    integer, intent(in) :: n
    real(dp) :: e, v(3) = (/ 1.0_dp, 2.5d-3, .5e+2 /)
    integer :: i
    e = 0.0_dp; i = 1
    Do While (i <= n .AND. p(i)%alive .Eq. .TRUE.)
      e = e + 0.5_dp * p(i)%m * &   ! the mass
          & sum(p(i)%v**2) / 1.e5
      if (1.eq.2 .or. .not. p(i)%alive) cycle
      i = i + 1
    End Do
    print *, 'it''s ', "done", e
  end function energy
end module dynamics
)f90"};

  //! \brief Self-check (so `make example` gates): `1.eq.2` is a number, an operator and a number while
  //!        `1.e5` is one number, dotted operators and keywords match in any case, and the continued
  //!        statement holds no NEWLINE, and every keyword alone is one KEYWORD. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex       {make_lexer()};
    const std::vector<scilex::token> toks      {lex.tokenize(sample)};
    bool                             eq_split  {false};
    bool                             exponent  {false};
    bool                             mixed_ops {false};
    bool                             mixed_kw  {false};
    bool                             joined    {false};
    for (std::size_t i {0}; i + 2 < toks.size(); ++i) {
      const scilex::token& tok {toks[i]};
      eq_split  = eq_split || (tok.lexeme == "1" && toks[i + 1].lexeme == ".eq." && toks[i + 2].lexeme == "2");
      exponent  = exponent || (tok.kind == number && tok.lexeme == "1.e5");
      mixed_ops = mixed_ops || (tok.kind == dot_op && tok.lexeme == ".AND.");
      mixed_kw  = mixed_kw || (tok.kind == keyword && tok.lexeme == "Do");
      // `* &` continues the statement: the `sum` after it follows the `*` with no NEWLINE between.
      joined = joined || (tok.lexeme == "*" && toks[i + 1].lexeme == "sum");
    }
    std::string words                      {keywords};
    std::replace(words.begin(), words.end(), '|', ' ');
    const std::vector<scilex::token> alone {lex.tokenize(words)};
    const bool                       whole {std::all_of(alone.begin(), alone.end(), [](const scilex::token& t) { return t.kind == keyword; })};
    return eq_split && exponent && mixed_ops && mixed_kw && joined && whole;
  }
} // namespace scilex::examples::fortran

#endif // SCILEX_EXAMPLE_FORTRAN_HPP
