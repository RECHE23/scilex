/*!
 * \file matlab.hpp
 * \brief A MATLAB / Octave lexer grammar — context decided by the text just read.
 *
 * MATLAB's lexical surprise is the apostrophe: after an operand (`a'`, `x(2)'`, `[1 2]'`, `s.f'`, `2'`,
 * `a''`) it is the transpose operator, anywhere else (`disp('hi')`, `x = 'it''s'`) it opens a character
 * array. The grammar says so with a lookbehind on the byte before the quote, the same rule MATLAB's own
 * lexer applies; both rules stay DFA-representable (a fixed one-byte lookbehind over an ASCII class).
 *
 * Covered: `%` comments (and Octave's `#`), `%{ … %}` block comments on lines of their own, nested (a
 * mode pushed per opener), `...` continuations (the rest of the line is a comment and the line break is
 * joined), numbers with `e`/`d` exponents, `i`/`j` imaginary suffixes and `0x` hex, element-wise operators
 * (`.*` `./` `.\` `.^` `.'`) with `2.^x` read as `2 .^ x` as MATLAB reads it, `~=` / `!=`, `&&` / `||`,
 * `@` handles, char arrays with `''` escapes and double-quoted strings with `""` escapes, and line breaks as
 * statement separators (NEWLINE tokens).
 *
 * Honest scope: whitespace inside `[ ]` and `{ }` separates elements in MATLAB (`[1 -2]` has two, `[1 - 2]`
 * one); the lexer skips it, so a parser that needs the rule reads the token columns. Command syntax
 * (`hold on`, `format long`) lexes as identifiers; Octave's backslash escapes in double-quoted strings are
 * not decoded (MATLAB's `"` strings have none).
 */
#ifndef SCILEX_EXAMPLE_MATLAB_HPP
#define SCILEX_EXAMPLE_MATLAB_HPP

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::matlab {

  //! \brief MATLAB token kinds (whitespace, comments and continuations are skip rules).
  enum kind
  {
    ws,
    comment,
    continuation,
    newline,
    keyword,
    ident,
    number,
    char_array, //!< `'…'`, a character array (a quote that does not follow an operand)
    string,     //!< `"…"`, a string scalar
    transpose,  //!< `'` after an operand
    op,
    lparen,
    rparen,
    lbracket,
    rbracket,
    lbrace,
    rbrace,
    comma,
    semicolon,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case newline:    return "NEWLINE";
      case keyword:    return "KEYWORD";
      case ident:      return "IDENT";
      case number:     return "NUMBER";
      case char_array: return "CHARS";
      case string:     return "STRING";
      case transpose:  return "TRANSPOSE";
      case op:         return "OP";
      case lparen:     return "(";
      case rparen:     return ")";
      case lbracket:   return "[";
      case rbracket:   return "]";
      case lbrace:     return "{";
      case rbrace:     return "}";
      case comma:      return ",";
      case semicolon:  return ";";
      default:         return "?";
    }
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action.
  inline scilex::rule rule(int                                kind,
                           const char*                        pattern,
                           std::vector<std::string>           modes,
                           bool                               skip   = false,
                           std::optional<scilex::mode_action> action = std::nullopt)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern, real::flags::ascii), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = action;
    return r;
  }

  //! \brief The keywords. An alternation takes its first alternative that matches, so a keyword precedes
  //!        every shorter one it begins with (`endif` before `end`), or the longer one would lose to IDENT.
  constexpr const char* keywords {
    "function|endfunction|if|elseif|else|endif|for|parfor|endfor|while|endwhile|do|until|switch|case"
    "|otherwise|endswitch|try|catch|end_try_catch|end|return|break|continue|global|persistent|classdef"
    "|properties|methods|events|enumeration|spmd"};

  //! \brief Builds the MATLAB rule list. A keyword precedes IDENT, so it wins an equal-length match and a
  //!        longer identifier (`endx`) stays an identifier; the block-comment opener precedes the line
  //!        comment for the same reason.
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;
    const std::vector<std::string> code  {"default"};
    const std::vector<std::string> block {"block"};
    // An operand ends in a word byte, a closing bracket, a dot (`a.'`) or a quote (`a''`): a quote after one
    // is the transpose, any other quote opens a character array.
    constexpr const char    * after_operand {R"re((?<=[A-Za-z0-9_)\]}.']))re"};
    std::vector<scilex::rule> rules;
    rules.push_back(rule(ws, R"re([ \t]+)re", code, true));
    rules.push_back(rule(continuation, R"re(\.\.\.[^\n]*\n)re", code, true));
    rules.push_back(rule(comment, R"re((?m)^[ \t]*%\{[ \t]*\r?$)re", code, true, scilex::mode_action {.operation = op_t::push, .target = "block"}));
    rules.push_back(rule(comment, R"re([%#][^\n]*)re", code, true));
    rules.push_back(rule(newline, R"re(\r?\n)re", code));
    rules.push_back(rule(keyword, keywords, code));
    rules.push_back(rule(ident, R"re([A-Za-z][A-Za-z0-9_]*)re", code));
    // `2.^x` is `2 .^ x`: a dot before an element-wise operator belongs to the operator, not the number.
    rules.push_back(rule(number, R"re(0[xX][0-9A-Fa-f]+|(?:[0-9]+(?:\.(?![*/\\^'])[0-9]*)?|\.[0-9]+)(?:[eEdD][+-]?[0-9]+)?[ij]?)re", code));
    rules.push_back(rule(transpose, (std::string {after_operand} + "'").c_str(), code));
    rules.push_back(rule(char_array, R"re((?<![A-Za-z0-9_)\]}.'])'(?:[^'\r\n]|'')*')re", code));
    rules.push_back(rule(string, R"re("(?:[^"\r\n]|"")*")re", code));
    rules.push_back(rule(op, R"re(\.\^|\.\*|\./|\.\\|\.'|==|~=|!=|<=|>=|&&|\|\||[-+*/\\^<>=&|~!:@.])re", code));
    rules.push_back(rule(lparen, R"re(\()re", code));
    rules.push_back(rule(rparen, R"re(\))re", code));
    rules.push_back(rule(lbracket, R"re(\[)re", code));
    rules.push_back(rule(rbracket, R"re(\])re", code));
    rules.push_back(rule(lbrace, R"re(\{)re", code));
    rules.push_back(rule(rbrace, R"re(\})re", code));
    rules.push_back(rule(comma, R"re(,)re", code));
    rules.push_back(rule(semicolon, R"re(;)re", code));
    // --- inside %{ … %}: a nested opener pushes again, a closer pops; anything else is comment text ---
    rules.push_back(rule(comment, R"re((?m)^[ \t]*%\{[ \t]*\r?$)re", block, true, scilex::mode_action {.operation = op_t::push, .target = "block"}));
    rules.push_back(rule(comment, R"re((?m)^[ \t]*%\}[ \t]*\r?$)re", block, true, scilex::mode_action {.operation = op_t::pop}));
    rules.push_back(rule(comment, R"re([^\n]+|\n)re", block, true));
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

  //! \brief A function with every construct the grammar distinguishes: transposes after an identifier, a
  //!        call, a bracket, a dot and a quote; character arrays with an escaped quote; a string; nested
  //!        block comments; a continuation; element-wise operators after a number; an anonymous function.
  inline constexpr std::string_view sample {
    R"matlab(function [U, S] = scaled_svd(A, tol)
% Decompose A, dropping singular values below tol.
%{
  Block comments nest:
  %{
    an inner block, not code: x = 1'
  %}
%}
if nargin < 2, tol = 1e-12; end
[U, S, V] = svd(A');           # Octave comment
S = S .* (S > tol) + 0.5i * eye(size(S, 1))';
w = 2.^(0:3)' ./ norm(V(:, 1)');
name = 'it''s';  label = "say ""hi""";
f = @(x) x.' * x ...  continued here
    + 1e+3;
msg = {'a', 'b'}'';
end
)matlab"};

  //! \brief Self-check (so `make example` gates): the transposes and the character arrays are told apart,
  //!        the nested block comment hides its code, `2.^` is a number then an element-wise operator, and
  //!        every keyword alone is one KEYWORD.
  //!        \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex        {make_lexer()};
    const std::vector<scilex::token> toks       {lex.tokenize(sample)};
    int                              transposes {0};
    int                              chars      {0};
    bool                             escaped    {false};
    bool                             power      {false};
    bool                             hidden     {true};
    for (std::size_t i {0}; i < toks.size(); ++i) {
      const scilex::token& tok {toks[i]};
      if (tok.kind == transpose) {
        ++transposes;
      }
      if (tok.kind == char_array) {
        ++chars;
        escaped = escaped || tok.lexeme == "'it''s'";
      }
      if (tok.kind == number && tok.lexeme == "2" && i + 1 < toks.size() && toks[i + 1].lexeme == ".^") {
        power = true;
      }
      if (tok.lexeme == "inner") {
        hidden = false;
      }
    }
    // A' · (…)' · (0:3)' · V(:,1)' · x.' is `.'` (an operator) · {…}'' is two
    std::string words                      {keywords};
    std::replace(words.begin(), words.end(), '|', ' ');
    const std::vector<scilex::token> alone {lex.tokenize(words)};
    const bool                       whole {std::all_of(alone.begin(), alone.end(), [](const scilex::token& t) { return t.kind == keyword; })};
    return transposes == 6 && chars == 3 && escaped && power && hidden && whole;
  }
} // namespace scilex::examples::matlab

#endif // SCILEX_EXAMPLE_MATLAB_HPP
