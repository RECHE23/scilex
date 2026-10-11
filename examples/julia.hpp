/*!
 * \file julia.hpp
 * \brief A Julia lexer grammar — nested comments, string interpolation and Unicode operators.
 *
 * Julia combines three things a flat token table cannot do. `#= … =#` comments nest (a mode pushed per
 * opener). A string interpolates `$name` and `$(expr)`, and the expression holds parentheses and strings of
 * its own: a `(` inside an interpolation pushes a mode, so the `)` that closes the interpolation is told
 * from the ones inside it — the mode stack does what a parser's bracket count would. And an apostrophe is
 * the adjoint after an operand (`A'`, `f(x)'`, `α'`) and a character literal anywhere else (`'a'`, `'\n'`),
 * a choice made by a one-code-point lookbehind; a colon is a range operator after an operand (`1:n`,
 * `a[i:j]`) and opens a symbol anywhere else (`:sym`, `f(:x)`).
 *
 * Covered: Unicode identifiers (`α`, `∂f`, `x̂`, and the `!` a mutating name ends in), macros (`@time`,
 * `@.`), `0x` / `0b` / `0o` integers with `_` separators, floats with `e` / `f` exponents and the `im`
 * suffix, broadcast operators (`.+`, `.*=`, `.≤`), `->`, `=>`, `|>`, `::`, `...`, Unicode operators
 * (`∈`, `≤`, `≠`, `⊗`, `√`, `÷`), `"…"` and `"""…"""` strings with escapes and both interpolations, and line
 * breaks as statement separators (NEWLINE tokens).
 *
 * Honest scope: string macros (`r"…"`, `raw"…"`) lex as a macro-less identifier then a string, so their
 * `$` is read as interpolation; command literals (`` `ls` ``) and the `2.+x` ambiguity Julia itself rejects
 * are out of this sample.
 */
#ifndef SCILEX_EXAMPLE_JULIA_HPP
#define SCILEX_EXAMPLE_JULIA_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::julia {

  //! \brief Julia token kinds (whitespace and comments are skip rules).
  enum kind
  {
    ws,
    comment,
    newline,
    keyword,
    ident,
    macro,
    symbol,
    number,
    character,
    adjoint,       //!< `'` after an operand
    string_open,   //!< `"` or `"""` — pushes a string body mode
    string_text,   //!< literal text of a string body
    string_escape, //!< `\n`, `\"`, `\$`, …
    interp_name,   //!< `$name` inside a string
    interp_open,   //!< `$(` — pushes an interpolation (code) mode
    interp_close,  //!< the `)` that closes an interpolation — pops it
    string_close,  //!< the `"` / `"""` that closes a string body — pops it
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
      case newline:       return "NEWLINE";
      case keyword:       return "KEYWORD";
      case ident:         return "IDENT";
      case macro:         return "MACRO";
      case symbol:        return "SYMBOL";
      case number:        return "NUMBER";
      case character:     return "CHAR";
      case adjoint:       return "ADJOINT";
      case string_open:   return "STRING_OPEN";
      case string_text:   return "STRING_TEXT";
      case string_escape: return "STRING_ESCAPE";
      case interp_name:   return "INTERP_NAME";
      case interp_open:   return "INTERP_OPEN";
      case interp_close:  return "INTERP_CLOSE";
      case string_close:  return "STRING_CLOSE";
      case op:            return "OP";
      case lparen:        return "(";
      case rparen:        return ")";
      case lbracket:      return "[";
      case rbracket:      return "]";
      case lbrace:        return "{";
      case rbrace:        return "}";
      case comma:         return ",";
      case semicolon:     return ";";
      default:            return "?";
    }
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action, in text mode (Unicode `\p{…}`).
  inline scilex::rule rule(int                                kind,
                           const char*                        pattern,
                           std::vector<std::string>           modes,
                           bool                               skip   = false,
                           std::optional<scilex::mode_action> action = std::nullopt)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = action;
    return r;
  }

  //! \brief A push to \p target.
  inline scilex::mode_action push(const char* target)
  {
    return {.operation = scilex::mode_action::op::push, .target = target};
  }

  //! \brief A pop.
  inline scilex::mode_action pop()
  {
    return {.operation = scilex::mode_action::op::pop};
  }

  //! \brief Builds the Julia rule list. A keyword precedes IDENT (an equal-length match is a keyword, a
  //!        longer name an identifier); `#=` precedes the line comment, which excludes it.
  inline std::vector<scilex::rule> make_rules()
  {
    // Code runs at the top level, inside an interpolation and inside a parenthesis an interpolation holds.
    const std::vector<std::string> code   {"default", "interp", "iparen"};
    const std::vector<std::string> bodies {"str", "tstr"};
    // An operand ends in a name character, a closing bracket or an adjoint: a quote after one is the adjoint
    // and a colon a range; any other quote opens a character literal and any other colon a symbol.
    constexpr const char    * operand_end {R"re([\p{XID_Continue}!)\]}'])re"};
    const std::string         after       {std::string {"(?<="} + operand_end + ")"};
    const std::string         not_after   {std::string {"(?<!"} + operand_end + ")"};
    std::vector<scilex::rule> rules;
    // --- trivia ---------------------------------------------------------------------------------------
    rules.push_back(rule(ws, R"re([ \t]+)re", code, true));
    rules.push_back(rule(comment, R"re(#=)re", code, true, push("block")));
    rules.push_back(rule(comment, R"re(#(?!=)[^\n]*)re", code, true));
    rules.push_back(rule(newline, R"re(\r?\n)re", code));
    // --- words -----------------------------------------------------------------------------------------
    rules.push_back(rule(keyword,
                         "function|end|if|elseif|else|for|while|begin|let|module|baremodule|struct|mutable"
                         "|abstract|type|primitive|quote|macro|return|break|continue|do|try|catch|finally|global"
                         "|local|const|import|using|export|where|in|isa|true|false|nothing",
                         code));
    rules.push_back(rule(ident, R"re([\p{XID_Start}_][\p{XID_Continue}!]*)re", code));
    rules.push_back(rule(macro, R"re(@(?:[\p{XID_Start}_][\p{XID_Continue}!]*|\.))re", code));
    rules.push_back(rule(symbol, (not_after + R"re(:[\p{XID_Start}_][\p{XID_Continue}!]*)re").c_str(), code));
    // --- literals --------------------------------------------------------------------------------------
    // A dot before a broadcast operator belongs to the operator: `2.*x` is `2 .* x`.
    rules.push_back(rule(number,
                         R"re(0x[0-9a-fA-F_]+|0b[01_]+|0o[0-7_]+|(?:[0-9](?:_?[0-9])*(?:\.(?![.*/\\^+\-<>=!&|%÷])[0-9_]*)?|\.[0-9][0-9_]*)(?:[eEf][+-]?[0-9]+)?(?:im)?)re",
                         code));
    rules.push_back(rule(adjoint, (after + "'").c_str(), code));
    rules.push_back(rule(character, (not_after + R"re('(?:[^'\\\r\n]|\\[^\r\n][^'\r\n]*)')re").c_str(), code));
    rules.push_back(rule(string_open, R"re(""")re", code, false, push("tstr")));
    rules.push_back(rule(string_open, R"re(")re", code, false, push("str")));
    // --- string bodies ---------------------------------------------------------------------------------
    rules.push_back(rule(string_text, R"re([^"\\$]+)re", {"str"}));
    rules.push_back(rule(string_text, R"re((?:[^"\\$]|"(?!""))+)re", {"tstr"}));
    rules.push_back(rule(string_escape, R"re(\\.)re", bodies));
    rules.push_back(rule(interp_name, R"re(\$[\p{XID_Start}_][\p{XID_Continue}!]*)re", bodies));
    rules.push_back(rule(interp_open, R"re(\$\()re", bodies, false, push("interp")));
    rules.push_back(rule(string_close, R"re(")re", {"str"}, false, pop()));
    rules.push_back(rule(string_close, R"re(""")re", {"tstr"}, false, pop()));
    // --- operators and punctuation ---------------------------------------------------------------------
    rules.push_back(rule(op,
                         R"re(\.\.\.|::|->|=>|\|>|<\||===|!==|==|!=|<=|>=|&&|\|\||<:|>:|\.(?:[-+*/\\^%<>=!&|÷∈≤≥≠⊗]=?)|[-+*/\\^%÷|&]=|[-+*/\\^%<>=!&|~÷∈∉⊆⊗≤≥≠√∘?$]|\.|:)re",
                         code));
    rules.push_back(rule(lparen, R"re(\()re", {"default"}));
    rules.push_back(rule(rparen, R"re(\))re", {"default"}));
    rules.push_back(rule(lparen, R"re(\()re", {"interp", "iparen"}, false, push("iparen")));
    rules.push_back(rule(rparen, R"re(\))re", {"iparen"}, false, pop()));
    rules.push_back(rule(interp_close, R"re(\))re", {"interp"}, false, pop()));
    rules.push_back(rule(lbracket, R"re(\[)re", code));
    rules.push_back(rule(rbracket, R"re(\])re", code));
    rules.push_back(rule(lbrace, R"re(\{)re", code));
    rules.push_back(rule(rbrace, R"re(\})re", code));
    rules.push_back(rule(comma, R"re(,)re", code));
    rules.push_back(rule(semicolon, R"re(;)re", code));
    // --- #= … =#: a nested opener pushes again, a closer pops; anything else is comment text ------------
    rules.push_back(rule(comment, R"re(#=)re", {"block"}, true, push("block")));
    rules.push_back(rule(comment, R"re(=#)re", {"block"}, true, pop()));
    rules.push_back(rule(comment, R"re((?:[^#=]|#(?!=)|=(?!#))+)re", {"block"}, true));
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

  //! \brief A function with every construct the grammar distinguishes: nested block comments, Unicode names
  //!        and operators, adjoints beside character literals, ranges beside symbols, broadcasting, and a
  //!        string whose interpolation holds parentheses and a string of its own.
  inline constexpr std::string_view sample {
    R"jl(#= Kinetic energy.
   #= nested: not code, x' =#
=#
function energy!(E, m::Vector{Float64}, v; α = 0.5f0)
    @. E = α * m * v^2           # broadcast
    s = sum(E[1:end]) + 2.5e-3im
    A = [1 2; 3 4]'; B = A' * A .+ 0x1F
    c = 'a'; nl = '\n'; ok = :done
    for i ∈ 1:length(m), j in eachindex(v)
        i ≤ j && continue
    end
    msg = "E[$(i)] = $(round(f("x$(1+2)"), digits=3)) for $α\n"
    doc = """total "energy" $s"""
    return s ÷ 2 |> abs
end
)jl"};

  //! \brief Self-check (so `make example` gates): the nested comment hides its code, adjoints and
  //!        character literals are told apart, a range is not a symbol, and the interpolation holding
  //!        parentheses and a string closes where it should. \return `true` on success.
  inline bool self_check()
  {
    const scilex::lexer              lex        {make_lexer()};
    const std::vector<scilex::token> toks       {lex.tokenize(sample)};
    int                              adjoints   {0};
    int                              characters {0};
    int                              symbols    {0};
    int                              opens      {0};
    int                              closes     {0};
    bool                             hidden     {true};
    bool                             unicode    {false};
    bool                             in_code    {false}; // `digits` after the interpolation's inner parentheses
    bool                             clean_head {true};  // the block comment leaves nothing before `function`
    bool                             body       {false};
    for (const scilex::token& tok : toks) {
      adjoints   += tok.kind == adjoint ? 1 : 0;
      characters += tok.kind == character ? 1 : 0;
      symbols    += tok.kind == symbol ? 1 : 0;
      opens      += tok.kind == interp_open ? 1 : 0;
      closes     += tok.kind == interp_close ? 1 : 0;
      hidden      = hidden && tok.lexeme != "nested:";
      unicode     = unicode || (tok.kind == ident && tok.lexeme == "α");
      in_code     = in_code || (tok.kind == ident && tok.lexeme == "digits");
      body        = body || (tok.kind == keyword && tok.lexeme == "function");
      clean_head  = clean_head && (body || tok.kind == newline);
    }
    // [..]' and A' · 'a' and '\n' · :done only (1:end, 1:length are ranges) · $( three times
    return adjoints == 2 && characters == 2 && symbols == 1 && opens == 3 && closes == 3 && hidden && unicode
           && in_code && clean_head;
  }
} // namespace scilex::examples::julia

#endif // SCILEX_EXAMPLE_JULIA_HPP
