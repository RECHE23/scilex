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
 * Also: strings continued over a line with `&`, OpenMP / OpenACC / compiler directives (`!$omp`, `!$acc`,
 * `!dir$`) as tokens rather than comments, the assumed rank `(..)`, and the statement keywords of Fortran
 * 2018, compact forms included (`endprogram`, `goto`, `elsewhere`). Fortran reserves no word: the names
 * programs commonly give their own variables and intrinsics (`len`, `kind`, `data`, `error`) stay names.
 *
 * Honest scope: fixed form (column 6 continuation, column 1 comments) is a different grammar; keywords are
 * words, not statements (`end do` is two keywords, `enddo` one), which is what a parser wants.
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
    directive, //!< `!$omp …`, `!$acc …`, `!$ …`, `!dir$ …`: the rest of the line
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
      case directive:    return "DIRECTIVE";
      default:           return "?";
    }
  }

  //! \brief The keywords. An alternation takes its first alternative that matches, so a keyword precedes
  //!        every shorter one it begins with (`inout` before `in`), or the longer one would lose to IDENT.
  constexpr const char* keywords {
    "doubleprecision|non_overridable|endsubroutine|non_recursive|asynchronous|endassociate|endblockdata"
    "|endinterface|endprocedure|endsubmodule|allocatable|codimension|endcritical|endfunction|equivalence"
    "|concurrent|contiguous|deallocate|endprogram|enumerator|subroutine|associate|backspace|blockdata"
    "|character|dimension|elemental|elsewhere|endforall|endmodule|endselect|interface|intrinsic|parameter"
    "|precision|procedure|protected|recursive|submodule|abstract|allocate|contains|continue|critical"
    "|deferred|endblock|endwhere|external|function|implicit|namelist|operator|optional|sequence|volatile"
    "|complex|default|endenum|endfile|endtype|extends|generic|include|inquire|integer|logical|nullify"
    "|pointer|private|program|assign|common|double|elseif|forall|import|impure|intent|module|nopass"
    "|public|result|return|rewind|select|target|unlock|block|class|close|cycle|enddo|endif|entry|final"
    "|flush|inout|pause|print|where|while|write|bind|call|case|else|enum|exit|goto|none|only|open|pass"
    "|pure|read|real|save|stop|sync|then|type|wait|end|out|use|do|go|if|in|to"};

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
    // A directive (OpenMP `!$omp`, OpenACC `!$acc`, a conditional `!$` line, a compiler's `!dir$`) is a token,
    // the rest of its line; any other `!` starts a comment. The directive comes first, to win the tie.
    rules.push_back(r(directive, R"re((?i)!(?:\$(?:omp|acc)?|dir\$|dec\$|gcc\$|ibm\*)[^\n]*)re"));
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
    // A string may go on over a line break: `&` ending the line, an optional `&` leading the next.
    rules.push_back(r(string, R"re('(?:[^'\r\n&]|''|&[ \t]*\r?\n[ \t]*&?|&)*'|"(?:[^"\r\n&]|""|&[ \t]*\r?\n[ \t]*&?|&)*")re"));
    // `..` is the assumed rank of `dimension(..)`.
    rules.push_back(r(op, R"re(\*\*|//|==|/=|<=|>=|=>|::|\.\.|\(/|/\)|[-+*/=<>%:])re"));
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
    // Each keyword lexes alone as one keyword: an alternation that took a shorter word first would split it.
    const auto count                       {static_cast<std::size_t>(std::count(words.begin(), words.end(), ' ')) + 1};
    const bool whole                       {alone.size() == count
                                            && std::all_of(alone.begin(), alone.end(), [](const scilex::token& t) { return t.kind == keyword; })};
    // A directive is a token, a string goes on over `&`, and `(..)` is an assumed rank.
    const std::vector<scilex::token> more     {lex.tokenize("!$omp parallel\ns = 'a&\n  &b'(..)")};
    const bool                       extended {more.size() == 8 && more[0].kind == directive && more[4].kind == string && more[6].lexeme == ".."};
    return eq_split && exponent && mixed_ops && mixed_kw && joined && whole && extended;
  }
} // namespace scilex::examples::fortran

#endif // SCILEX_EXAMPLE_FORTRAN_HPP
