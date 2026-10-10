/*!
 * \file token.hpp
 * \brief The token produced by the lexer and its source position.
 *
 * A token is a non-owning view into the source text (a \ref scilex::token
 * borrows its lexeme), keeping tokenization allocation-light: the only
 * allocation is the token vector itself.
 */
#ifndef SCILEX_TOKEN_HPP
#define SCILEX_TOKEN_HPP

#include <cstddef>
#include <limits>
#include <string_view>

namespace scilex {

  /*!
   * \brief Reserved token kind for the synthetic end-of-input token.
   *
   * Emitted at the end of the input when tokenizing with
   * \ref scilex::eof_policy::append (the parser-friendly mode: there is always
   * a current token, including a terminal one to match). SciLex reserves this
   * value; user-defined token kinds must not use it.
   */
  inline constexpr int end_of_input {std::numeric_limits<int>::min()};

  /*!
   * \brief Reserved token kind for a lexical-error run under \ref scilex::error_policy::token.
   *
   * When error recovery is enabled, a maximal run of bytes that no rule in the active mode can begin
   * is emitted as one token of this kind (its \ref token::lexeme is the exact offending bytes), instead
   * of throwing. Part of the reserved family: \ref end_of_input is `min()`, the layout kinds
   * (`scilex::newline` / `indent` / `dedent` in layout.hpp) are `min()+1..+3`, so this takes the next
   * free slot, `min()+4`. User-defined token kinds must not use it.
   */
  inline constexpr int error {std::numeric_limits<int>::min() + 4};

  /*!
   * \brief A location in the source text.
   *
   * Columns are counted within the line in the unit the lexer's \ref column_unit
   * names, bytes by default (REAL's byte-level UTF-8 model: a multibyte code point
   * spans several columns), or code points, or UTF-16 code units. Lines and columns are 1-based;
   * a line ends at `\n` only (a lone `\r` does not end one). \ref offset is a
   * 0-based byte index from the start of the source, whatever the column unit.
   */
  struct position
  {
    std::size_t offset; //!< 0-based byte offset from the start of the source.
    std::size_t line;   //!< 1-based line number.
    std::size_t column; //!< 1-based column within the line, in the lexer's \ref column_unit.
  };

  /*!
   * \brief One lexical token: a typed slice of the source.
   */
  struct token
  {
    int              kind;        //!< Caller-defined token kind (e.g. an enum value).
    std::string_view lexeme;      //!< The matched text, viewing into the source.
    position         start;       //!< Position of the token's first byte.
    std::size_t      mode_id {0}; //!< The mode this token was lexed in (0 = the default/root mode).
  };
} // namespace scilex

#endif // SCILEX_TOKEN_HPP
