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
#include <string_view>

namespace scilex {

  /*!
   * \brief A location in the source text.
   *
   * Columns are counted in bytes within the line, consistent with REAL's
   * byte-level UTF-8 model (an ASCII column equals a character column; a
   * multibyte codepoint spans several byte columns). Lines and columns are
   * 1-based; \ref offset is a 0-based byte index from the start of the source.
   */
  struct position
  {
    std::size_t offset; //!< 0-based byte offset from the start of the source.
    std::size_t line;   //!< 1-based line number.
    std::size_t column; //!< 1-based byte column within the line.
  };

  /*!
   * \brief One lexical token: a typed slice of the source.
   */
  struct token
  {
    int              kind;   //!< Caller-defined token kind (e.g. an enum value).
    std::string_view lexeme; //!< The matched text, viewing into the source.
    position         start;  //!< Position of the token's first byte.
  };
} // namespace scilex

#endif // SCILEX_TOKEN_HPP
