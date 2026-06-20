/*!
 * \file sql.hpp
 * \brief A SQL-like lexer grammar — the case-insensitive + alt-escape example.
 *
 * SQL stresses two needs the others do not: keywords are **case-insensitive**
 * (`SELECT` = `select` = `SeLeCt`), exercising REAL's `icase` flag on a rule;
 * and string literals escape an embedded quote by **doubling** it (`'it''s'`),
 * a different convention from the backslash escapes of JSON / C++. Line (`--`)
 * and block comments are skipped.
 *
 * Honest scope: a representative keyword set and the common operators; this is
 * not a full SQL dialect (no dollar-quoting, no delimited `"identifiers"`,
 * no dialect-specific syntax). Single-mode maximal munch covers what is here.
 */
#ifndef SCILEX_EXAMPLE_SQL_HPP
#define SCILEX_EXAMPLE_SQL_HPP

#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::sql {

  //! \brief SQL-like token kinds (whitespace and comments are skip rules).
  enum kind
  {
    ws,
    line_comment,
    block_comment,
    keyword,
    ident,
    number,
    str,
    op,
    punct,
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case keyword: return "KEYWORD";
      case ident:   return "NAME";
      case number:  return "NUMBER";
      case str:     return "STRING";
      case op:      return "OP";
      case punct:   return "PUNCT";
      default:      return "?";
    }
  }

  //! \brief Builds the SQL-like lexer. Each keyword rule carries the `icase`
  //!        flag, so it matches in any letter case; keyword rules precede
  //!        \ref ident so an equal-length match resolves to the keyword.
  inline std::vector<scilex::rule> make_rules()
  {
    std::vector<scilex::rule> rules;
    rules.push_back({ws, real::regex("\\s+"), true});
    rules.push_back({line_comment, real::regex("--.*"), true});
    rules.push_back({block_comment, real::regex(R"re(/\*([^*]|\*+[^*/])*\*+/)re"), true});
    for (const char* word : {"select", "from", "where", "insert", "into", "values", "update",
                             "set", "delete", "create", "table", "drop", "join", "inner",
                             "left", "right", "on", "and", "or", "not", "null", "as", "order",
                             "by", "group", "having", "distinct", "in", "is", "like", "limit"}) {
      rules.push_back({keyword, real::regex(word, real::flags::icase), false});
    }
    rules.push_back({ident, real::regex("[A-Za-z_][A-Za-z0-9_]*"), false});
    rules.push_back({number, real::regex("[0-9]+(\\.[0-9]+)?"), false});
    // single-quoted string; an embedded quote is escaped by doubling it ('')
    rules.push_back({str, real::regex(R"re('([^']|'')*')re"), false});
    rules.push_back({op, real::regex(R"re(<>|<=|>=|!=|\|\||[-+*/%<>=])re"), false});
    rules.push_back({punct, real::regex(R"re([(),.;])re"), false});
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A query with a mixed-case keyword and a doubled-quote escape.
  inline constexpr std::string_view sample {
    R"sql(-- case-insensitive keywords and '' escapes
SeLeCt id, 'it''s' AS note
FROM users
WHERE id >= 10 AND name <> 'x';
)sql"};

  //! \brief Self-check (so `make example` gates): the distinctive invariants —
  //!        a mixed-case keyword (`SeLeCt`) is recognized as a KEYWORD via the
  //!        `icase` flag, and a doubled-quote string (`'it''s'`) munches into a
  //!        single STRING token. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex           {make_lexer()};
    const std::vector<scilex::token> toks          {lex.tokenize(sample)};
    bool                             icase_keyword {false};
    bool                             doubled_quote {false};
    for (const scilex::token& tok : toks) {
      icase_keyword = icase_keyword || (tok.kind == keyword && tok.lexeme == "SeLeCt");
      doubled_quote = doubled_quote || (tok.kind == str && tok.lexeme == "'it''s'");
    }
    return icase_keyword && doubled_quote;
  }
} // namespace scilex::examples::sql

#endif // SCILEX_EXAMPLE_SQL_HPP
