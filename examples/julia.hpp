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
 * It reads Julia as the tokenizer of Julia's own parser does (`make julia-oracle` compares the two over
 * the sources of Julia's standard library and test suite). Covered: identifiers as Julia defines them (`α`,
 * `∂f`, `x̂`, emoji, primes, and the `!` a mutating name ends in, but not before `=`), `var"…"` names,
 * macros (`@time`, `@.`), `0x` / `0b` / `0o` integers with `_` separators, hexadecimal floats (`0x1p3`),
 * floats with `e` / `f` exponents (`2im` is a number times `im`, as Julia reads it), broadcast operators
 * (`.+`, `.*=`, `.|>`), every Unicode operator Julia parses (`≈`, `⊻`, `∪`, …) and the suffixes mathematics
 * writes after one (`==ₜ`, `+̂`, `A'ᵀ`), `"…"` and `"""…"""` strings with escapes and both interpolations,
 * string macros (`r"…"i`, `raw"…"`, `b"…"`) and commands (`` `ls $d` ``), whose literals are raw, and line
 * breaks as statement separators (NEWLINE tokens).
 *
 * Honest scope: an operator suffix from a combining block of another script (Hebrew, Arabic, Devanagari…),
 * which Julia accepts and no source writes; and the `2.+x` ambiguity Julia itself rejects.
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
    raw_close,    //!< the quote closing a raw literal or a command, the flags glued to it: `"i` (STRING_CLOSE)
    var_open,     //!< the `var"` that opens a name written as a string
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
      case raw_close:     return "STRING_CLOSE";
      case var_open:      return "VAR_OPEN";
      default:            return "?";
    }
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action, in text mode (Unicode `\p{…}`).
  inline scilex::rule rule(int                                kind,
                           const std::string&                 pattern,
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
    // String bodies that interpolate, and the raw ones: a string macro's, and a command's (whose `$` the `@cmd`
    // macro reads, not the lexer).
    const std::vector<std::string> bodies {"str", "tstr"};
    const std::vector<std::string> raw    {"rstr", "rtstr", "rcmd", "rtcmd"};
    // A name, as Julia reads one (Base.is_id_start_char, Base.is_id_char): beyond ASCII letters, digits and `_`,
    // the classes below (emoji, `∂`, primes); `!` is one of its characters, except before `=` (`x!=y` is
    // `x != y`).
    const std::string         id_start    {R"re(A-Za-z_\x{A2}-\x{A5}\x{A9}-\x{AA}\x{AE}\x{B0}\x{B5}\x{BA}\x{C0}-\x{D6}\x{D8}-\x{F6})re"
                                           R"re(\x{F8}-\x{2C1}\x{2C6}-\x{2D1}\x{2E0}-\x{2E4}\x{2EC}\x{2EE}\x{370}-\x{374}\x{376}-\x{377})re"
                                           R"re(\x{37A}-\x{37D}\x{37F}\x{386}\x{388}-\x{38A}\x{38C}\x{38E}-\x{3A1}\x{3A3}-\x{3F5}\x{3F7}-\x{482})re"
                                           R"re(\x{48A}-\x{52F}\x{531}-\x{556}\x{559}\x{560}-\x{588}\x{58D}-\x{58F}\x{5D0}-\x{5EA})re"
                                           R"re(\x{5EF}-\x{5F2}\x{60B}\x{60E}-\x{60F}\x{620}-\x{64A}\x{66E}-\x{66F}\x{671}-\x{6D3}\x{6D5}\x{6DE})re"
                                           R"re(\x{6E5}-\x{6E6}\x{6E9}\x{6EE}-\x{6EF}\x{6FA}-\x{6FF}\x{710}\x{712}-\x{72F}\x{74D}-\x{7A5}\x{7B1})re"
                                           R"re(\x{7CA}-\x{7EA}\x{7F4}-\x{7F6}\x{7FA}\x{7FE}-\x{815}\x{81A}\x{824}\x{828}\x{840}-\x{858})re"
                                           R"re(\x{860}-\x{86A}\x{870}-\x{887}\x{889}-\x{88E}\x{8A0}-\x{8C9}\x{904}-\x{939}\x{93D}\x{950})re"
                                           R"re(\x{958}-\x{961}\x{971}-\x{980}\x{985}-\x{98C}\x{98F}-\x{990}\x{993}-\x{9A8}\x{9AA}-\x{9B0})re"
                                           R"re(\x{9B2}\x{9B6}-\x{9B9}\x{9BD}\x{9CE}\x{9DC}-\x{9DD}\x{9DF}-\x{9E1}\x{9F0}-\x{9F3}\x{9FA}-\x{9FC})re"
                                           R"re(\x{A05}-\x{A0A}\x{A0F}-\x{A10}\x{A13}-\x{A28}\x{A2A}-\x{A30}\x{A32}-\x{A33}\x{A35}-\x{A36})re"
                                           R"re(\x{A38}-\x{A39}\x{A59}-\x{A5C}\x{A5E}\x{A72}-\x{A74}\x{A85}-\x{A8D}\x{A8F}-\x{A91})re"
                                           R"re(\x{A93}-\x{AA8}\x{AAA}-\x{AB0}\x{AB2}-\x{AB3}\x{AB5}-\x{AB9}\x{ABD}\x{AD0}\x{AE0}-\x{AE1}\x{AF1})re"
                                           R"re(\x{AF9}\x{B05}-\x{B0C}\x{B0F}-\x{B10}\x{B13}-\x{B28}\x{B2A}-\x{B30}\x{B32}-\x{B33})re"
                                           R"re(\x{B35}-\x{B39}\x{B3D}\x{B5C}-\x{B5D}\x{B5F}-\x{B61}\x{B70}-\x{B71}\x{B83}\x{B85}-\x{B8A})re"
                                           R"re(\x{B8E}-\x{B90}\x{B92}-\x{B95}\x{B99}-\x{B9A}\x{B9C}\x{B9E}-\x{B9F}\x{BA3}-\x{BA4})re"
                                           R"re(\x{BA8}-\x{BAA}\x{BAE}-\x{BB9}\x{BD0}\x{BF3}-\x{BFA}\x{C05}-\x{C0C}\x{C0E}-\x{C10})re"
                                           R"re(\x{C12}-\x{C28}\x{C2A}-\x{C39}\x{C3D}\x{C58}-\x{C5A}\x{C5D}\x{C60}-\x{C61}\x{C7F}-\x{C80})re"
                                           R"re(\x{C85}-\x{C8C}\x{C8E}-\x{C90}\x{C92}-\x{CA8}\x{CAA}-\x{CB3}\x{CB5}-\x{CB9}\x{CBD})re"
                                           R"re(\x{CDD}-\x{CDE}\x{CE0}-\x{CE1}\x{CF1}-\x{CF2}\x{D04}-\x{D0C}\x{D0E}-\x{D10}\x{D12}-\x{D3A})re"
                                           R"re(\x{D3D}\x{D4E}-\x{D4F}\x{D54}-\x{D56}\x{D5F}-\x{D61}\x{D79}-\x{D7F}\x{D85}-\x{D96})re"
                                           R"re(\x{D9A}-\x{DB1}\x{DB3}-\x{DBB}\x{DBD}\x{DC0}-\x{DC6}\x{E01}-\x{E30}\x{E32}-\x{E33})re"
                                           R"re(\x{E3F}-\x{E46}\x{E81}-\x{E82}\x{E84}\x{E86}-\x{E8A}\x{E8C}-\x{EA3}\x{EA5}\x{EA7}-\x{EB0})re"
                                           R"re(\x{EB2}-\x{EB3}\x{EBD}\x{EC0}-\x{EC4}\x{EC6}\x{EDC}-\x{EDF}\x{F00}-\x{F03}\x{F13}\x{F15}-\x{F17})re"
                                           R"re(\x{F1A}-\x{F1F}\x{F34}\x{F36}\x{F38}\x{F40}-\x{F47}\x{F49}-\x{F6C}\x{F88}-\x{F8C}\x{FBE}-\x{FC5})re"
                                           R"re(\x{FC7}-\x{FCC}\x{FCE}-\x{FCF}\x{FD5}-\x{FD8}\x{1000}-\x{102A}\x{103F}\x{1050}-\x{1055})re"
                                           R"re(\x{105A}-\x{105D}\x{1061}\x{1065}-\x{1066}\x{106E}-\x{1070}\x{1075}-\x{1081}\x{108E})re"
                                           R"re(\x{109E}-\x{10C5}\x{10C7}\x{10CD}\x{10D0}-\x{10FA}\x{10FC}-\x{1248}\x{124A}-\x{124D})re"
                                           R"re(\x{1250}-\x{1256}\x{1258}\x{125A}-\x{125D}\x{1260}-\x{1288}\x{128A}-\x{128D}\x{1290}-\x{12B0})re"
                                           R"re(\x{12B2}-\x{12B5}\x{12B8}-\x{12BE}\x{12C0}\x{12C2}-\x{12C5}\x{12C8}-\x{12D6}\x{12D8}-\x{1310})re"
                                           R"re(\x{1312}-\x{1315}\x{1318}-\x{135A}\x{1380}-\x{1399}\x{13A0}-\x{13F5}\x{13F8}-\x{13FD})re"
                                           R"re(\x{1401}-\x{166D}\x{166F}-\x{167F}\x{1681}-\x{169A}\x{16A0}-\x{16EA}\x{16EE}-\x{16F8})re"
                                           R"re(\x{1700}-\x{1711}\x{171F}-\x{1731}\x{1740}-\x{1751}\x{1760}-\x{176C}\x{176E}-\x{1770})re"
                                           R"re(\x{1780}-\x{17B3}\x{17D7}\x{17DB}-\x{17DC}\x{1820}-\x{1878}\x{1880}-\x{1884}\x{1887}-\x{18A8})re"
                                           R"re(\x{18AA}\x{18B0}-\x{18F5}\x{1900}-\x{191E}\x{1940}\x{1950}-\x{196D}\x{1970}-\x{1974})re"
                                           R"re(\x{1980}-\x{19AB}\x{19B0}-\x{19C9}\x{19DE}-\x{1A16}\x{1A20}-\x{1A54}\x{1AA7}\x{1B05}-\x{1B33})re"
                                           R"re(\x{1B45}-\x{1B4C}\x{1B61}-\x{1B6A}\x{1B74}-\x{1B7C}\x{1B83}-\x{1BA0}\x{1BAE}-\x{1BAF})re"
                                           R"re(\x{1BBA}-\x{1BE5}\x{1C00}-\x{1C23}\x{1C4D}-\x{1C4F}\x{1C5A}-\x{1C7D}\x{1C80}-\x{1C88})re"
                                           R"re(\x{1C90}-\x{1CBA}\x{1CBD}-\x{1CBF}\x{1CE9}-\x{1CEC}\x{1CEE}-\x{1CF3}\x{1CF5}-\x{1CF6}\x{1CFA})re"
                                           R"re(\x{1D00}-\x{1DBF}\x{1E00}-\x{1F15}\x{1F18}-\x{1F1D}\x{1F20}-\x{1F45}\x{1F48}-\x{1F4D})re"
                                           R"re(\x{1F50}-\x{1F57}\x{1F59}\x{1F5B}\x{1F5D}\x{1F5F}-\x{1F7D}\x{1F80}-\x{1FB4}\x{1FB6}-\x{1FBC})re"
                                           R"re(\x{1FBE}\x{1FC2}-\x{1FC4}\x{1FC6}-\x{1FCC}\x{1FD0}-\x{1FD3}\x{1FD6}-\x{1FDB}\x{1FE0}-\x{1FEC})re"
                                           R"re(\x{1FF2}-\x{1FF4}\x{1FF6}-\x{1FFC}\x{2071}\x{207A}-\x{207F}\x{208A}-\x{208E}\x{2090}-\x{209C})re"
                                           R"re(\x{20A0}-\x{20C0}\x{2100}-\x{214A}\x{214C}-\x{214F}\x{2160}-\x{2188}\x{218A}-\x{218B}\x{2200})re"
                                           R"re(\x{2202}-\x{2207}\x{220E}-\x{2211}\x{221E}-\x{2222}\x{222B}-\x{2233}\x{223F}\x{22A4}-\x{22A5})re"
                                           R"re(\x{22BE}-\x{22C3}\x{2300}-\x{2307}\x{230C}-\x{231F}\x{2322}-\x{2328}\x{232B}-\x{233E})re"
                                           R"re(\x{2340}-\x{237B}\x{237D}-\x{239A}\x{23B4}-\x{23DB}\x{23E2}-\x{2426}\x{2440}-\x{244A})re"
                                           R"re(\x{249C}-\x{24E9}\x{2500}-\x{25B6}\x{25B8}-\x{25C0}\x{25C2}-\x{2767}\x{2794}-\x{27C1})re"
                                           R"re(\x{27D8}-\x{27D9}\x{2800}-\x{28FF}\x{299B}-\x{29B4}\x{2A00}-\x{2A06}\x{2A09}-\x{2A16})re"
                                           R"re(\x{2A1B}-\x{2A1C}\x{2B00}-\x{2B2F}\x{2B45}-\x{2B46}\x{2B4D}-\x{2B73}\x{2B76}-\x{2B95})re"
                                           R"re(\x{2B97}-\x{2CEE}\x{2CF2}-\x{2CF3}\x{2D00}-\x{2D25}\x{2D27}\x{2D2D}\x{2D30}-\x{2D67}\x{2D6F})re"
                                           R"re(\x{2D80}-\x{2D96}\x{2DA0}-\x{2DA6}\x{2DA8}-\x{2DAE}\x{2DB0}-\x{2DB6}\x{2DB8}-\x{2DBE})re"
                                           R"re(\x{2DC0}-\x{2DC6}\x{2DC8}-\x{2DCE}\x{2DD0}-\x{2DD6}\x{2DD8}-\x{2DDE}\x{2E2F}\x{2E50}-\x{2E51})re"
                                           R"re(\x{2E80}-\x{2E99}\x{2E9B}-\x{2EF3}\x{2F00}-\x{2FD5}\x{2FF0}-\x{2FFF}\x{3004}-\x{3007})re"
                                           R"re(\x{3012}-\x{3013}\x{3020}-\x{3029}\x{3031}-\x{303C}\x{303E}-\x{303F}\x{3041}-\x{3096})re"
                                           R"re(\x{309B}-\x{309F}\x{30A1}-\x{30FA}\x{30FC}-\x{30FF}\x{3105}-\x{312F}\x{3131}-\x{318E})re"
                                           R"re(\x{3190}-\x{3191}\x{3196}-\x{31E3}\x{31EF}-\x{321E}\x{322A}-\x{3247}\x{3250}\x{3260}-\x{327F})re"
                                           R"re(\x{328A}-\x{32B0}\x{32C0}-\x{A48C}\x{A490}-\x{A4C6}\x{A4D0}-\x{A4FD}\x{A500}-\x{A60C})re"
                                           R"re(\x{A610}-\x{A61F}\x{A62A}-\x{A62B}\x{A640}-\x{A66E}\x{A67F}-\x{A69D}\x{A6A0}-\x{A6EF})re"
                                           R"re(\x{A717}-\x{A71F}\x{A722}-\x{A788}\x{A78B}-\x{A7CA}\x{A7D0}-\x{A7D1}\x{A7D3}\x{A7D5}-\x{A7D9})re"
                                           R"re(\x{A7F2}-\x{A801}\x{A803}-\x{A805}\x{A807}-\x{A80A}\x{A80C}-\x{A822}\x{A828}-\x{A82B})re"
                                           R"re(\x{A836}-\x{A839}\x{A840}-\x{A873}\x{A882}-\x{A8B3}\x{A8F2}-\x{A8F7}\x{A8FB}\x{A8FD}-\x{A8FE})re"
                                           R"re(\x{A90A}-\x{A925}\x{A930}-\x{A946}\x{A960}-\x{A97C}\x{A984}-\x{A9B2}\x{A9CF}\x{A9E0}-\x{A9E4})re"
                                           R"re(\x{A9E6}-\x{A9EF}\x{A9FA}-\x{A9FE}\x{AA00}-\x{AA28}\x{AA40}-\x{AA42}\x{AA44}-\x{AA4B})re"
                                           R"re(\x{AA60}-\x{AA7A}\x{AA7E}-\x{AAAF}\x{AAB1}\x{AAB5}-\x{AAB6}\x{AAB9}-\x{AABD}\x{AAC0}\x{AAC2})re"
                                           R"re(\x{AADB}-\x{AADD}\x{AAE0}-\x{AAEA}\x{AAF2}-\x{AAF4}\x{AB01}-\x{AB06}\x{AB09}-\x{AB0E})re"
                                           R"re(\x{AB11}-\x{AB16}\x{AB20}-\x{AB26}\x{AB28}-\x{AB2E}\x{AB30}-\x{AB5A}\x{AB5C}-\x{AB69})re"
                                           R"re(\x{AB70}-\x{ABE2}\x{AC00}-\x{D7A3}\x{D7B0}-\x{D7C6}\x{D7CB}-\x{D7FB}\x{F900}-\x{FA6D})re"
                                           R"re(\x{FA70}-\x{FAD9}\x{FB00}-\x{FB06}\x{FB13}-\x{FB17}\x{FB1D}\x{FB1F}-\x{FB28}\x{FB2A}-\x{FB36})re"
                                           R"re(\x{FB38}-\x{FB3C}\x{FB3E}\x{FB40}-\x{FB41}\x{FB43}-\x{FB44}\x{FB46}-\x{FBB1}\x{FBD3}-\x{FD3D})re"
                                           R"re(\x{FD40}-\x{FD8F}\x{FD92}-\x{FDC7}\x{FDCF}\x{FDF0}-\x{FDFF}\x{FE69}\x{FE70}-\x{FE74})re"
                                           R"re(\x{FE76}-\x{FEFC}\x{FF04}\x{FF21}-\x{FF3A}\x{FF41}-\x{FF5A}\x{FF66}-\x{FFBE}\x{FFC2}-\x{FFC7})re"
                                           R"re(\x{FFCA}-\x{FFCF}\x{FFD2}-\x{FFD7}\x{FFDA}-\x{FFDC}\x{FFE0}-\x{FFE1}\x{FFE4}-\x{FFE6}\x{FFE8})re"
                                           R"re(\x{FFED}-\x{FFEE}\x{10000}-\x{1000B}\x{1000D}-\x{10026}\x{10028}-\x{1003A}\x{1003C}-\x{1003D})re"
                                           R"re(\x{1003F}-\x{1004D}\x{10050}-\x{1005D}\x{10080}-\x{100FA}\x{10137}-\x{10174}\x{10179}-\x{10189})re"
                                           R"re(\x{1018C}-\x{1018E}\x{10190}-\x{1019C}\x{101A0}\x{101D0}-\x{101FC}\x{10280}-\x{1029C})re"
                                           R"re(\x{102A0}-\x{102D0}\x{10300}-\x{1031F}\x{1032D}-\x{1034A}\x{10350}-\x{10375}\x{10380}-\x{1039D})re"
                                           R"re(\x{103A0}-\x{103C3}\x{103C8}-\x{103CF}\x{103D1}-\x{103D5}\x{10400}-\x{1049D}\x{104B0}-\x{104D3})re"
                                           R"re(\x{104D8}-\x{104FB}\x{10500}-\x{10527}\x{10530}-\x{10563}\x{10570}-\x{1057A}\x{1057C}-\x{1058A})re"
                                           R"re(\x{1058C}-\x{10592}\x{10594}-\x{10595}\x{10597}-\x{105A1}\x{105A3}-\x{105B1}\x{105B3}-\x{105B9})re"
                                           R"re(\x{105BB}-\x{105BC}\x{10600}-\x{10736}\x{10740}-\x{10755}\x{10760}-\x{10767}\x{10780}-\x{10785})re"
                                           R"re(\x{10787}-\x{107B0}\x{107B2}-\x{107BA}\x{10800}-\x{10805}\x{10808}\x{1080A}-\x{10835})re"
                                           R"re(\x{10837}-\x{10838}\x{1083C}\x{1083F}-\x{10855}\x{10860}-\x{10878}\x{10880}-\x{1089E})re"
                                           R"re(\x{108E0}-\x{108F2}\x{108F4}-\x{108F5}\x{10900}-\x{10915}\x{10920}-\x{10939}\x{10980}-\x{109B7})re"
                                           R"re(\x{109BE}-\x{109BF}\x{10A00}\x{10A10}-\x{10A13}\x{10A15}-\x{10A17}\x{10A19}-\x{10A35})re"
                                           R"re(\x{10A60}-\x{10A7C}\x{10A80}-\x{10A9C}\x{10AC0}-\x{10AE4}\x{10B00}-\x{10B35}\x{10B40}-\x{10B55})re"
                                           R"re(\x{10B60}-\x{10B72}\x{10B80}-\x{10B91}\x{10C00}-\x{10C48}\x{10C80}-\x{10CB2}\x{10CC0}-\x{10CF2})re"
                                           R"re(\x{10D00}-\x{10D23}\x{10E80}-\x{10EA9}\x{10EB0}-\x{10EB1}\x{10F00}-\x{10F1C}\x{10F27})re"
                                           R"re(\x{10F30}-\x{10F45}\x{10F70}-\x{10F81}\x{10FB0}-\x{10FC4}\x{10FE0}-\x{10FF6}\x{11003}-\x{11037})re"
                                           R"re(\x{11071}-\x{11072}\x{11075}\x{11083}-\x{110AF}\x{110D0}-\x{110E8}\x{11103}-\x{11126}\x{11144})re"
                                           R"re(\x{11147}\x{11150}-\x{11172}\x{11176}\x{11183}-\x{111B2}\x{111C1}-\x{111C4}\x{111DA}\x{111DC})re"
                                           R"re(\x{11200}-\x{11211}\x{11213}-\x{1122B}\x{1123F}-\x{11240}\x{11280}-\x{11286}\x{11288})re"
                                           R"re(\x{1128A}-\x{1128D}\x{1128F}-\x{1129D}\x{1129F}-\x{112A8}\x{112B0}-\x{112DE}\x{11305}-\x{1130C})re"
                                           R"re(\x{1130F}-\x{11310}\x{11313}-\x{11328}\x{1132A}-\x{11330}\x{11332}-\x{11333}\x{11335}-\x{11339})re"
                                           R"re(\x{1133D}\x{11350}\x{1135D}-\x{11361}\x{11400}-\x{11434}\x{11447}-\x{1144A}\x{1145F}-\x{11461})re"
                                           R"re(\x{11480}-\x{114AF}\x{114C4}-\x{114C5}\x{114C7}\x{11580}-\x{115AE}\x{115D8}-\x{115DB})re"
                                           R"re(\x{11600}-\x{1162F}\x{11644}\x{11680}-\x{116AA}\x{116B8}\x{11700}-\x{1171A}\x{1173F}-\x{11746})re"
                                           R"re(\x{11800}-\x{1182B}\x{118A0}-\x{118DF}\x{118FF}-\x{11906}\x{11909}\x{1190C}-\x{11913})re"
                                           R"re(\x{11915}-\x{11916}\x{11918}-\x{1192F}\x{1193F}\x{11941}\x{119A0}-\x{119A7}\x{119AA}-\x{119D0})re"
                                           R"re(\x{119E1}\x{119E3}\x{11A00}\x{11A0B}-\x{11A32}\x{11A3A}\x{11A50}\x{11A5C}-\x{11A89}\x{11A9D})re"
                                           R"re(\x{11AB0}-\x{11AF8}\x{11C00}-\x{11C08}\x{11C0A}-\x{11C2E}\x{11C40}\x{11C72}-\x{11C8F})re"
                                           R"re(\x{11D00}-\x{11D06}\x{11D08}-\x{11D09}\x{11D0B}-\x{11D30}\x{11D46}\x{11D60}-\x{11D65})re"
                                           R"re(\x{11D67}-\x{11D68}\x{11D6A}-\x{11D89}\x{11D98}\x{11EE0}-\x{11EF2}\x{11F02}\x{11F04}-\x{11F10})re"
                                           R"re(\x{11F12}-\x{11F33}\x{11FB0}\x{11FD5}-\x{11FF1}\x{12000}-\x{12399}\x{12400}-\x{1246E})re"
                                           R"re(\x{12480}-\x{12543}\x{12F90}-\x{12FF0}\x{13000}-\x{1342F}\x{13441}-\x{13446}\x{14400}-\x{14646})re"
                                           R"re(\x{16800}-\x{16A38}\x{16A40}-\x{16A5E}\x{16A70}-\x{16ABE}\x{16AD0}-\x{16AED}\x{16B00}-\x{16B2F})re"
                                           R"re(\x{16B3C}-\x{16B43}\x{16B45}\x{16B63}-\x{16B77}\x{16B7D}-\x{16B8F}\x{16E40}-\x{16E7F})re"
                                           R"re(\x{16F00}-\x{16F4A}\x{16F50}\x{16F93}-\x{16F9F}\x{16FE0}-\x{16FE1}\x{16FE3}\x{17000}-\x{187F7})re"
                                           R"re(\x{18800}-\x{18CD5}\x{18D00}-\x{18D08}\x{1AFF0}-\x{1AFF3}\x{1AFF5}-\x{1AFFB}\x{1AFFD}-\x{1AFFE})re"
                                           R"re(\x{1B000}-\x{1B122}\x{1B132}\x{1B150}-\x{1B152}\x{1B155}\x{1B164}-\x{1B167}\x{1B170}-\x{1B2FB})re"
                                           R"re(\x{1BC00}-\x{1BC6A}\x{1BC70}-\x{1BC7C}\x{1BC80}-\x{1BC88}\x{1BC90}-\x{1BC99}\x{1BC9C})re"
                                           R"re(\x{1CF50}-\x{1CFC3}\x{1D000}-\x{1D0F5}\x{1D100}-\x{1D126}\x{1D129}-\x{1D164}\x{1D16A}-\x{1D16C})re"
                                           R"re(\x{1D183}-\x{1D184}\x{1D18C}-\x{1D1A9}\x{1D1AE}-\x{1D1EA}\x{1D200}-\x{1D241}\x{1D245})re"
                                           R"re(\x{1D300}-\x{1D356}\x{1D400}-\x{1D454}\x{1D456}-\x{1D49C}\x{1D49E}-\x{1D49F}\x{1D4A2})re"
                                           R"re(\x{1D4A5}-\x{1D4A6}\x{1D4A9}-\x{1D4AC}\x{1D4AE}-\x{1D4B9}\x{1D4BB}\x{1D4BD}-\x{1D4C3})re"
                                           R"re(\x{1D4C5}-\x{1D505}\x{1D507}-\x{1D50A}\x{1D50D}-\x{1D514}\x{1D516}-\x{1D51C}\x{1D51E}-\x{1D539})re"
                                           R"re(\x{1D53B}-\x{1D53E}\x{1D540}-\x{1D544}\x{1D546}\x{1D54A}-\x{1D550}\x{1D552}-\x{1D6A5})re"
                                           R"re(\x{1D6A8}-\x{1D7CB}\x{1D7CE}-\x{1D7E1}\x{1D800}-\x{1D9FF}\x{1DA37}-\x{1DA3A}\x{1DA6D}-\x{1DA74})re"
                                           R"re(\x{1DA76}-\x{1DA83}\x{1DA85}-\x{1DA86}\x{1DF00}-\x{1DF1E}\x{1DF25}-\x{1DF2A}\x{1E030}-\x{1E06D})re"
                                           R"re(\x{1E100}-\x{1E12C}\x{1E137}-\x{1E13D}\x{1E14E}-\x{1E14F}\x{1E290}-\x{1E2AD}\x{1E2C0}-\x{1E2EB})re"
                                           R"re(\x{1E2FF}\x{1E4D0}-\x{1E4EB}\x{1E7E0}-\x{1E7E6}\x{1E7E8}-\x{1E7EB}\x{1E7ED}-\x{1E7EE})re"
                                           R"re(\x{1E7F0}-\x{1E7FE}\x{1E800}-\x{1E8C4}\x{1E900}-\x{1E943}\x{1E94B}\x{1ECAC}\x{1ECB0}\x{1ED2E})re"
                                           R"re(\x{1EE00}-\x{1EE03}\x{1EE05}-\x{1EE1F}\x{1EE21}-\x{1EE22}\x{1EE24}\x{1EE27}\x{1EE29}-\x{1EE32})re"
                                           R"re(\x{1EE34}-\x{1EE37}\x{1EE39}\x{1EE3B}\x{1EE42}\x{1EE47}\x{1EE49}\x{1EE4B}\x{1EE4D}-\x{1EE4F})re"
                                           R"re(\x{1EE51}-\x{1EE52}\x{1EE54}\x{1EE57}\x{1EE59}\x{1EE5B}\x{1EE5D}\x{1EE5F}\x{1EE61}-\x{1EE62})re"
                                           R"re(\x{1EE64}\x{1EE67}-\x{1EE6A}\x{1EE6C}-\x{1EE72}\x{1EE74}-\x{1EE77}\x{1EE79}-\x{1EE7C}\x{1EE7E})re"
                                           R"re(\x{1EE80}-\x{1EE89}\x{1EE8B}-\x{1EE9B}\x{1EEA1}-\x{1EEA3}\x{1EEA5}-\x{1EEA9}\x{1EEAB}-\x{1EEBB})re"
                                           R"re(\x{1F000}-\x{1F02B}\x{1F030}-\x{1F093}\x{1F0A0}-\x{1F0AE}\x{1F0B1}-\x{1F0BF}\x{1F0C1}-\x{1F0CF})re"
                                           R"re(\x{1F0D1}-\x{1F0F5}\x{1F10D}-\x{1F1AD}\x{1F1E6}-\x{1F202}\x{1F210}-\x{1F23B}\x{1F240}-\x{1F248})re"
                                           R"re(\x{1F250}-\x{1F251}\x{1F260}-\x{1F265}\x{1F300}-\x{1F3FA}\x{1F400}-\x{1F6D7}\x{1F6DC}-\x{1F6EC})re"
                                           R"re(\x{1F6F0}-\x{1F6FC}\x{1F700}-\x{1F776}\x{1F77B}-\x{1F7D9}\x{1F7E0}-\x{1F7EB}\x{1F7F0})re"
                                           R"re(\x{1F800}-\x{1F80B}\x{1F810}-\x{1F847}\x{1F850}-\x{1F859}\x{1F860}-\x{1F887}\x{1F890}-\x{1F8AD})re"
                                           R"re(\x{1F8B0}-\x{1F8B1}\x{1F900}-\x{1FA53}\x{1FA60}-\x{1FA6D}\x{1FA70}-\x{1FA7C}\x{1FA80}-\x{1FA88})re"
                                           R"re(\x{1FA90}-\x{1FABD}\x{1FABF}-\x{1FAC5}\x{1FACE}-\x{1FADB}\x{1FAE0}-\x{1FAE8}\x{1FAF0}-\x{1FAF8})re"
                                           R"re(\x{1FB00}-\x{1FB92}\x{1FB94}-\x{1FBCA}\x{20000}-\x{2A6DF}\x{2A700}-\x{2B739}\x{2B740}-\x{2B81D})re"
                                           R"re(\x{2B820}-\x{2CEA1}\x{2CEB0}-\x{2EBE0}\x{2EBF0}-\x{2EE5D}\x{2F800}-\x{2FA1D}\x{30000}-\x{3134A})re"
                                           R"re(\x{31350}-\x{323AF})re"};
    const std::string         id_more     {R"re(0-9\x{A8}\x{AF}\x{B2}-\x{B4}\x{B8}-\x{B9}\x{BC}-\x{BE}\x{2C2}-\x{2C5}\x{2D2}-\x{2DF})re"
                                           R"re(\x{2E5}-\x{2EB}\x{2ED}\x{2EF}-\x{36F}\x{375}\x{384}-\x{385}\x{483}-\x{489}\x{591}-\x{5BD}\x{5BF})re"
                                           R"re(\x{5C1}-\x{5C2}\x{5C4}-\x{5C5}\x{5C7}\x{610}-\x{61A}\x{64B}-\x{669}\x{670}\x{6D6}-\x{6DC})re"
                                           R"re(\x{6DF}-\x{6E4}\x{6E7}-\x{6E8}\x{6EA}-\x{6ED}\x{6F0}-\x{6F9}\x{711}\x{730}-\x{74A})re"
                                           R"re(\x{7A6}-\x{7B0}\x{7C0}-\x{7C9}\x{7EB}-\x{7F3}\x{7FD}\x{816}-\x{819}\x{81B}-\x{823})re"
                                           R"re(\x{825}-\x{827}\x{829}-\x{82D}\x{859}-\x{85B}\x{888}\x{898}-\x{89F}\x{8CA}-\x{8E1})re"
                                           R"re(\x{8E3}-\x{903}\x{93A}-\x{93C}\x{93E}-\x{94F}\x{951}-\x{957}\x{962}-\x{963}\x{966}-\x{96F})re"
                                           R"re(\x{981}-\x{983}\x{9BC}\x{9BE}-\x{9C4}\x{9C7}-\x{9C8}\x{9CB}-\x{9CD}\x{9D7}\x{9E2}-\x{9E3})re"
                                           R"re(\x{9E6}-\x{9EF}\x{9F4}-\x{9F9}\x{9FE}\x{A01}-\x{A03}\x{A3C}\x{A3E}-\x{A42}\x{A47}-\x{A48})re"
                                           R"re(\x{A4B}-\x{A4D}\x{A51}\x{A66}-\x{A71}\x{A75}\x{A81}-\x{A83}\x{ABC}\x{ABE}-\x{AC5}\x{AC7}-\x{AC9})re"
                                           R"re(\x{ACB}-\x{ACD}\x{AE2}-\x{AE3}\x{AE6}-\x{AEF}\x{AFA}-\x{AFF}\x{B01}-\x{B03}\x{B3C})re"
                                           R"re(\x{B3E}-\x{B44}\x{B47}-\x{B48}\x{B4B}-\x{B4D}\x{B55}-\x{B57}\x{B62}-\x{B63}\x{B66}-\x{B6F})re"
                                           R"re(\x{B72}-\x{B77}\x{B82}\x{BBE}-\x{BC2}\x{BC6}-\x{BC8}\x{BCA}-\x{BCD}\x{BD7}\x{BE6}-\x{BF2})re"
                                           R"re(\x{C00}-\x{C04}\x{C3C}\x{C3E}-\x{C44}\x{C46}-\x{C48}\x{C4A}-\x{C4D}\x{C55}-\x{C56})re"
                                           R"re(\x{C62}-\x{C63}\x{C66}-\x{C6F}\x{C78}-\x{C7E}\x{C81}-\x{C83}\x{CBC}\x{CBE}-\x{CC4})re"
                                           R"re(\x{CC6}-\x{CC8}\x{CCA}-\x{CCD}\x{CD5}-\x{CD6}\x{CE2}-\x{CE3}\x{CE6}-\x{CEF}\x{CF3})re"
                                           R"re(\x{D00}-\x{D03}\x{D3B}-\x{D3C}\x{D3E}-\x{D44}\x{D46}-\x{D48}\x{D4A}-\x{D4D}\x{D57}-\x{D5E})re"
                                           R"re(\x{D62}-\x{D63}\x{D66}-\x{D78}\x{D81}-\x{D83}\x{DCA}\x{DCF}-\x{DD4}\x{DD6}\x{DD8}-\x{DDF})re"
                                           R"re(\x{DE6}-\x{DEF}\x{DF2}-\x{DF3}\x{E31}\x{E34}-\x{E3A}\x{E47}-\x{E4E}\x{E50}-\x{E59}\x{EB1})re"
                                           R"re(\x{EB4}-\x{EBC}\x{EC8}-\x{ECE}\x{ED0}-\x{ED9}\x{F18}-\x{F19}\x{F20}-\x{F33}\x{F35}\x{F37}\x{F39})re"
                                           R"re(\x{F3E}-\x{F3F}\x{F71}-\x{F84}\x{F86}-\x{F87}\x{F8D}-\x{F97}\x{F99}-\x{FBC}\x{FC6})re"
                                           R"re(\x{102B}-\x{103E}\x{1040}-\x{1049}\x{1056}-\x{1059}\x{105E}-\x{1060}\x{1062}-\x{1064})re"
                                           R"re(\x{1067}-\x{106D}\x{1071}-\x{1074}\x{1082}-\x{108D}\x{108F}-\x{109D}\x{135D}-\x{135F})re"
                                           R"re(\x{1369}-\x{137C}\x{1712}-\x{1715}\x{1732}-\x{1734}\x{1752}-\x{1753}\x{1772}-\x{1773})re"
                                           R"re(\x{17B4}-\x{17D3}\x{17DD}\x{17E0}-\x{17E9}\x{17F0}-\x{17F9}\x{180B}-\x{180D}\x{180F}-\x{1819})re"
                                           R"re(\x{1885}-\x{1886}\x{18A9}\x{1920}-\x{192B}\x{1930}-\x{193B}\x{1946}-\x{194F}\x{19D0}-\x{19DA})re"
                                           R"re(\x{1A17}-\x{1A1B}\x{1A55}-\x{1A5E}\x{1A60}-\x{1A7C}\x{1A7F}-\x{1A89}\x{1A90}-\x{1A99})re"
                                           R"re(\x{1AB0}-\x{1ACE}\x{1B00}-\x{1B04}\x{1B34}-\x{1B44}\x{1B50}-\x{1B59}\x{1B6B}-\x{1B73})re"
                                           R"re(\x{1B80}-\x{1B82}\x{1BA1}-\x{1BAD}\x{1BB0}-\x{1BB9}\x{1BE6}-\x{1BF3}\x{1C24}-\x{1C37})re"
                                           R"re(\x{1C40}-\x{1C49}\x{1C50}-\x{1C59}\x{1CD0}-\x{1CD2}\x{1CD4}-\x{1CE8}\x{1CED}\x{1CF4})re"
                                           R"re(\x{1CF7}-\x{1CF9}\x{1DC0}-\x{1DFF}\x{1FBD}\x{1FBF}-\x{1FC1}\x{1FCD}-\x{1FCF}\x{1FDD}-\x{1FDF})re"
                                           R"re(\x{1FED}-\x{1FEF}\x{1FFD}-\x{1FFE}\x{2032}-\x{2037}\x{203F}-\x{2040}\x{2054}\x{2057}\x{2070})re"
                                           R"re(\x{2074}-\x{2079}\x{2080}-\x{2089}\x{20D0}-\x{20F0}\x{2150}-\x{215F}\x{2189}\x{2460}-\x{249B})re"
                                           R"re(\x{24EA}-\x{24FF}\x{2776}-\x{2793}\x{2CEF}-\x{2CF1}\x{2CFD}\x{2D7F}\x{2DE0}-\x{2DFF})re"
                                           R"re(\x{302A}-\x{302F}\x{3099}-\x{309A}\x{3192}-\x{3195}\x{3220}-\x{3229}\x{3248}-\x{324F})re"
                                           R"re(\x{3251}-\x{325F}\x{3280}-\x{3289}\x{32B1}-\x{32BF}\x{A620}-\x{A629}\x{A66F}-\x{A672})re"
                                           R"re(\x{A674}-\x{A67D}\x{A69E}-\x{A69F}\x{A6F0}-\x{A6F1}\x{A700}-\x{A716}\x{A720}-\x{A721})re"
                                           R"re(\x{A789}-\x{A78A}\x{A802}\x{A806}\x{A80B}\x{A823}-\x{A827}\x{A82C}\x{A830}-\x{A835})re"
                                           R"re(\x{A880}-\x{A881}\x{A8B4}-\x{A8C5}\x{A8D0}-\x{A8D9}\x{A8E0}-\x{A8F1}\x{A8FF}-\x{A909})re"
                                           R"re(\x{A926}-\x{A92D}\x{A947}-\x{A953}\x{A980}-\x{A983}\x{A9B3}-\x{A9C0}\x{A9D0}-\x{A9D9}\x{A9E5})re"
                                           R"re(\x{A9F0}-\x{A9F9}\x{AA29}-\x{AA36}\x{AA43}\x{AA4C}-\x{AA4D}\x{AA50}-\x{AA59}\x{AA7B}-\x{AA7D})re"
                                           R"re(\x{AAB0}\x{AAB2}-\x{AAB4}\x{AAB7}-\x{AAB8}\x{AABE}-\x{AABF}\x{AAC1}\x{AAEB}-\x{AAEF})re"
                                           R"re(\x{AAF5}-\x{AAF6}\x{AB5B}\x{AB6A}-\x{AB6B}\x{ABE3}-\x{ABEA}\x{ABEC}-\x{ABED}\x{ABF0}-\x{ABF9})re"
                                           R"re(\x{FB1E}\x{FBB2}-\x{FBC2}\x{FE00}-\x{FE0F}\x{FE20}-\x{FE2F}\x{FE33}-\x{FE34}\x{FE4D}-\x{FE4F})re"
                                           R"re(\x{FF10}-\x{FF19}\x{FF3E}-\x{FF40}\x{FFE3}\x{10107}-\x{10133}\x{10175}-\x{10178})re"
                                           R"re(\x{1018A}-\x{1018B}\x{101FD}\x{102E0}-\x{102FB}\x{10320}-\x{10323}\x{10376}-\x{1037A})re"
                                           R"re(\x{104A0}-\x{104A9}\x{10858}-\x{1085F}\x{10879}-\x{1087F}\x{108A7}-\x{108AF}\x{108FB}-\x{108FF})re"
                                           R"re(\x{10916}-\x{1091B}\x{109BC}-\x{109BD}\x{109C0}-\x{109CF}\x{109D2}-\x{109FF}\x{10A01}-\x{10A03})re"
                                           R"re(\x{10A05}-\x{10A06}\x{10A0C}-\x{10A0F}\x{10A38}-\x{10A3A}\x{10A3F}-\x{10A48}\x{10A7D}-\x{10A7E})re"
                                           R"re(\x{10A9D}-\x{10A9F}\x{10AE5}-\x{10AE6}\x{10AEB}-\x{10AEF}\x{10B58}-\x{10B5F}\x{10B78}-\x{10B7F})re"
                                           R"re(\x{10BA9}-\x{10BAF}\x{10CFA}-\x{10CFF}\x{10D24}-\x{10D27}\x{10D30}-\x{10D39}\x{10E60}-\x{10E7E})re"
                                           R"re(\x{10EAB}-\x{10EAC}\x{10EFD}-\x{10EFF}\x{10F1D}-\x{10F26}\x{10F46}-\x{10F54}\x{10F82}-\x{10F85})re"
                                           R"re(\x{10FC5}-\x{10FCB}\x{11000}-\x{11002}\x{11038}-\x{11046}\x{11052}-\x{11070}\x{11073}-\x{11074})re"
                                           R"re(\x{1107F}-\x{11082}\x{110B0}-\x{110BA}\x{110C2}\x{110F0}-\x{110F9}\x{11100}-\x{11102})re"
                                           R"re(\x{11127}-\x{11134}\x{11136}-\x{1113F}\x{11145}-\x{11146}\x{11173}\x{11180}-\x{11182})re"
                                           R"re(\x{111B3}-\x{111C0}\x{111C9}-\x{111CC}\x{111CE}-\x{111D9}\x{111E1}-\x{111F4}\x{1122C}-\x{11237})re"
                                           R"re(\x{1123E}\x{11241}\x{112DF}-\x{112EA}\x{112F0}-\x{112F9}\x{11300}-\x{11303}\x{1133B}-\x{1133C})re"
                                           R"re(\x{1133E}-\x{11344}\x{11347}-\x{11348}\x{1134B}-\x{1134D}\x{11357}\x{11362}-\x{11363})re"
                                           R"re(\x{11366}-\x{1136C}\x{11370}-\x{11374}\x{11435}-\x{11446}\x{11450}-\x{11459}\x{1145E})re"
                                           R"re(\x{114B0}-\x{114C3}\x{114D0}-\x{114D9}\x{115AF}-\x{115B5}\x{115B8}-\x{115C0}\x{115DC}-\x{115DD})re"
                                           R"re(\x{11630}-\x{11640}\x{11650}-\x{11659}\x{116AB}-\x{116B7}\x{116C0}-\x{116C9}\x{1171D}-\x{1172B})re"
                                           R"re(\x{11730}-\x{1173B}\x{1182C}-\x{1183A}\x{118E0}-\x{118F2}\x{11930}-\x{11935}\x{11937}-\x{11938})re"
                                           R"re(\x{1193B}-\x{1193E}\x{11940}\x{11942}-\x{11943}\x{11950}-\x{11959}\x{119D1}-\x{119D7})re"
                                           R"re(\x{119DA}-\x{119E0}\x{119E4}\x{11A01}-\x{11A0A}\x{11A33}-\x{11A39}\x{11A3B}-\x{11A3E}\x{11A47})re"
                                           R"re(\x{11A51}-\x{11A5B}\x{11A8A}-\x{11A99}\x{11C2F}-\x{11C36}\x{11C38}-\x{11C3F}\x{11C50}-\x{11C6C})re"
                                           R"re(\x{11C92}-\x{11CA7}\x{11CA9}-\x{11CB6}\x{11D31}-\x{11D36}\x{11D3A}\x{11D3C}-\x{11D3D})re"
                                           R"re(\x{11D3F}-\x{11D45}\x{11D47}\x{11D50}-\x{11D59}\x{11D8A}-\x{11D8E}\x{11D90}-\x{11D91})re"
                                           R"re(\x{11D93}-\x{11D97}\x{11DA0}-\x{11DA9}\x{11EF3}-\x{11EF6}\x{11F00}-\x{11F01}\x{11F03})re"
                                           R"re(\x{11F34}-\x{11F3A}\x{11F3E}-\x{11F42}\x{11F50}-\x{11F59}\x{11FC0}-\x{11FD4}\x{13440})re"
                                           R"re(\x{13447}-\x{13455}\x{16A60}-\x{16A69}\x{16AC0}-\x{16AC9}\x{16AF0}-\x{16AF4}\x{16B30}-\x{16B36})re"
                                           R"re(\x{16B50}-\x{16B59}\x{16B5B}-\x{16B61}\x{16E80}-\x{16E96}\x{16F4F}\x{16F51}-\x{16F87})re"
                                           R"re(\x{16F8F}-\x{16F92}\x{16FE4}\x{16FF0}-\x{16FF1}\x{1BC9D}-\x{1BC9E}\x{1CF00}-\x{1CF2D})re"
                                           R"re(\x{1CF30}-\x{1CF46}\x{1D165}-\x{1D169}\x{1D16D}-\x{1D172}\x{1D17B}-\x{1D182}\x{1D185}-\x{1D18B})re"
                                           R"re(\x{1D1AA}-\x{1D1AD}\x{1D242}-\x{1D244}\x{1D2C0}-\x{1D2D3}\x{1D2E0}-\x{1D2F3}\x{1D360}-\x{1D378})re"
                                           R"re(\x{1D7E2}-\x{1D7FF}\x{1DA00}-\x{1DA36}\x{1DA3B}-\x{1DA6C}\x{1DA75}\x{1DA84}\x{1DA9B}-\x{1DA9F})re"
                                           R"re(\x{1DAA1}-\x{1DAAF}\x{1E000}-\x{1E006}\x{1E008}-\x{1E018}\x{1E01B}-\x{1E021}\x{1E023}-\x{1E024})re"
                                           R"re(\x{1E026}-\x{1E02A}\x{1E08F}\x{1E130}-\x{1E136}\x{1E140}-\x{1E149}\x{1E2AE}\x{1E2EC}-\x{1E2F9})re"
                                           R"re(\x{1E4EC}-\x{1E4F9}\x{1E8C7}-\x{1E8D6}\x{1E944}-\x{1E94A}\x{1E950}-\x{1E959}\x{1EC71}-\x{1ECAB})re"
                                           R"re(\x{1ECAD}-\x{1ECAF}\x{1ECB1}-\x{1ECB4}\x{1ED01}-\x{1ED2D}\x{1ED2F}-\x{1ED3D}\x{1F100}-\x{1F10C})re"
                                           R"re(\x{1F3FB}-\x{1F3FF}\x{1FBF0}-\x{1FBF9}\x{E0100}-\x{E01EF})re"};
    const std::string         name        {"[" + id_start + "](?:[" + id_start + id_more + "]|!(?!=))*"};
    // An operand ends in a name character, a closing bracket or an adjoint: a quote after one is the adjoint
    // and a colon a range; any other quote opens a character literal and any other colon a symbol.
    const std::string         operand_end {"[" + id_start + id_more + R"re(!)\]}'])re"};
    const std::string         after       {"(?<=" + operand_end + ")"};
    const std::string         not_after   {"(?<!" + operand_end + ")"};
    // The characters an operator may end in (`==ₜ`, `+̂`, `*′`) that mathematics writes: Julia's suffixes
    // (Tokenize.isopsuffix) in the superscript and subscript, prime, modifier-letter and Latin and symbol
    // combining blocks (the combining marks of other scripts are suffixes too, but none is written after an
    // operator, and the whole class would keep the rule off the DFA). Then every character Julia parses as an
    // operator (Base.isoperator), beyond ASCII.
    const std::string         suffixes    {R"re([\x{B2}-\x{B3}\x{B9}\x{2B0}\x{2B2}-\x{2B3}\x{2B7}-\x{2B8}\x{2E1}-\x{2E3}\x{300}-\x{36F}\x{1D2C})re"
                                           R"re(\x{1D2E}\x{1D30}-\x{1D31}\x{1D33}-\x{1D3A}\x{1D3C}\x{1D3E}-\x{1D43}\x{1D47}-\x{1D49}\x{1D4D})re"
                                           R"re(\x{1D4F}-\x{1D50}\x{1D52}\x{1D56}-\x{1D58}\x{1D5B}\x{1D5D}-\x{1D6A}\x{1DC0}-\x{1DFF})re"
                                           R"re(\x{2032}-\x{2037}\x{2057}\x{2070}-\x{2071}\x{2074}-\x{208E}\x{2090}-\x{2093}\x{2095}-\x{209C})re"
                                           R"re(\x{20D0}-\x{20F0}\x{2C7C}-\x{2C7D}\x{A71B}-\x{A71D}]*)re"};
    const std::string         operators   {R"re([\x{A6}\x{AC}\x{B1}\x{B7}\x{D7}\x{F7}\x{387}\x{2026}\x{205D}\x{214B}\x{2190}-\x{2194})re"
                                           R"re(\x{219A}-\x{219E}\x{21A0}\x{21A2}-\x{21A4}\x{21A6}\x{21A9}-\x{21AC}\x{21AE}\x{21B6}-\x{21B7})re"
                                           R"re(\x{21BA}-\x{21BD}\x{21C0}-\x{21C1}\x{21C4}\x{21C6}-\x{21C7}\x{21C9}\x{21CB}-\x{21D0}\x{21D2})re"
                                           R"re(\x{21D4}\x{21DA}-\x{21DD}\x{21E0}\x{21E2}\x{21F4}-\x{21FF}\x{2208}-\x{220D}\x{2212}-\x{2214})re"
                                           R"re(\x{2217}-\x{221D}\x{2224}-\x{222A}\x{2237}-\x{2238}\x{223A}-\x{223B}\x{223D}-\x{223E})re"
                                           R"re(\x{2240}-\x{228B}\x{228D}-\x{229C}\x{229E}-\x{22A3}\x{22A9}\x{22AC}\x{22AE}\x{22B0}-\x{22B7})re"
                                           R"re(\x{22BB}-\x{22BD}\x{22C4}-\x{22C7}\x{22C9}-\x{22D3}\x{22D5}-\x{22FF}\x{233F}\x{25B7}\x{27C2})re"
                                           R"re(\x{27C7}-\x{27C9}\x{27D1}-\x{27D2}\x{27D5}-\x{27D7}\x{27F0}-\x{27F1}\x{27F5}-\x{27F7})re"
                                           R"re(\x{27F9}-\x{27FF}\x{2900}-\x{2918}\x{291D}-\x{2920}\x{2944}-\x{2970}\x{2977}\x{297A})re"
                                           R"re(\x{29B7}-\x{29B8}\x{29BC}\x{29BE}-\x{29C1}\x{29E1}\x{29E3}-\x{29E5}\x{29F4}\x{29F6}-\x{29F7})re"
                                           R"re(\x{29FA}-\x{29FB}\x{2A07}-\x{2A08}\x{2A1D}\x{2A1F}\x{2A22}-\x{2A2E}\x{2A30}-\x{2A3D})re"
                                           R"re(\x{2A40}-\x{2A45}\x{2A4A}-\x{2A58}\x{2A5A}-\x{2A63}\x{2A66}-\x{2A67}\x{2A6A}-\x{2AD9}\x{2ADB})re"
                                           R"re(\x{2AEA}-\x{2AEB}\x{2AF7}-\x{2AFA}\x{2B30}-\x{2B44}\x{2B47}-\x{2B4C}])re"};
    std::vector<scilex::rule> rules;
    // --- trivia ---------------------------------------------------------------------------------------
    rules.push_back(rule(ws, R"re([ \t\x{A0}]+)re", code, true)); // a no-break space too, as Julia reads it
    rules.push_back(rule(comment, R"re(#=)re", code, true, push("block")));
    rules.push_back(rule(comment, R"re(#(?!=)[^\n]*)re", code, true));
    rules.push_back(rule(newline, R"re(\r?\n)re", code));
    // --- words -----------------------------------------------------------------------------------------
    // A name right before a quote or a backquote is a string macro (`r"…"`, `raw"…"`, `cmd`), whose literal is
    // raw: no interpolation, no escapes but the quote's. It comes first, to win the tie with the name.
    // `var"…"` is a name written as a string.
    rules.push_back(rule(var_open, R"re(var")re", code, false, push("varname")));
    rules.push_back(rule(keyword,
                         "function|end|if|elseif|else|for|while|begin|let|module|baremodule|struct|mutable"
                         "|abstract|type|primitive|quote|macro|return|break|continue|do|try|catch|finally|global"
                         "|local|const|import|using|export|where|in|isa|true|false|nothing",
                         code));
    rules.push_back(rule(ident, name, code));
    rules.push_back(rule(macro, "@(?:" + name + R"re(|\.))re", code));
    // A symbol is a whole name not opening a literal: `:var"a b"` is `:` then a name written as a string.
    rules.push_back(rule(symbol, not_after + ":" + name + "(?![" + id_start + id_more + R"re(!"`]))re", code));
    // --- literals --------------------------------------------------------------------------------------
    // A dot before a broadcast operator belongs to the operator: `2.*x` is `2 .* x`. A hexadecimal float
    // has a binary exponent (`0x1p3`); `im` after a number is a name the number multiplies (`2im`).
    rules.push_back(rule(number,
                         R"re(0x[0-9a-fA-F_]*(?:\.[0-9a-fA-F_]*)?[pP][+-]?[0-9]+|0x[0-9a-fA-F_]+|0b[01_]+|0o[0-7_]+|(?:[0-9](?:_?[0-9])*(?:\.(?![.*/\\^+\-<>=!&|%÷])[0-9_]*)?|\.[0-9][0-9_]*)(?:[eEf][+-]?[0-9]+)?)re",
                         code));
    rules.push_back(rule(adjoint, after + "'" + suffixes, code)); // `A'ᵀ` too
    // `'''` is the quote character.
    rules.push_back(rule(character, not_after + R"re((?:'''|'(?:[^'\\\r\n]|\\[^\r\n][^'\r\n]*)'))re", code));
    // A quote or a backquote glued to a name opens a string macro's literal (`r"…"`, `raw"…"`, `cmd`), which is
    // raw: no interpolation, no escapes but the quote's. The name is a name; only the opener looks behind it, so
    // the lookbehind runs at quotes alone and the name rule keeps its speed.
    for (const auto& [opener, body] : {std::pair {R"re(""")re", "rtstr"}, std::pair {R"re(")re", "rstr"},
                                       std::pair {"```", "rtcmd"}, std::pair {"`", "rcmd"}}) {
      rules.push_back(rule(string_open, "(?<=[" + id_start + id_more + "!])" + opener, code, false, push(body)));
    }
    rules.push_back(rule(string_open, R"re(""")re", code, false, push("tstr")));
    rules.push_back(rule(string_open, R"re(")re", code, false, push("str")));
    rules.push_back(rule(string_open, "```", code, false, push("rtcmd")));
    rules.push_back(rule(string_open, "`", code, false, push("rcmd")));
    // --- string and command bodies ---------------------------------------------------------------------
    rules.push_back(rule(string_text, R"re([^"\\$]+)re", {"str"}));
    rules.push_back(rule(string_text, R"re((?:[^"\\$]|"(?!""))+)re", {"tstr"}));
    rules.push_back(rule(string_text, R"re([^"\\]+)re", {"rstr"}));
    rules.push_back(rule(string_text, R"re((?:[^"\\]|"(?!""))+)re", {"rtstr"}));
    rules.push_back(rule(string_text, R"re([^`\\]+)re", {"rcmd"}));
    rules.push_back(rule(string_text, R"re((?:[^`\\]|`(?!``))+)re", {"rtcmd"}));
    rules.push_back(rule(string_escape, R"re(\\[\s\S])re", bodies));
    rules.push_back(rule(string_escape, R"re(\\[\s\S])re", raw));
    rules.push_back(rule(interp_name, R"re(\$)re" + name, bodies));
    rules.push_back(rule(interp_open, R"re(\$\()re", bodies, false, push("interp")));
    rules.push_back(rule(ident, R"re((?:[^"\\]|\\[\s\S])+)re", {"varname"}));
    rules.push_back(rule(string_close, R"re(")re", {"str", "varname"}, false, pop()));
    rules.push_back(rule(string_close, R"re(""")re", {"tstr"}, false, pop()));
    // Flags glued to a raw literal's closing quote (`r"…"i`) are part of it.
    const std::string flags {"([" + id_start + "][" + id_start + id_more + "]*)?"};
    rules.push_back(rule(raw_close, R"re(")re" + flags, {"rstr"}, false, pop()));
    rules.push_back(rule(raw_close, R"re(""")re" + flags, {"rtstr"}, false, pop()));
    rules.push_back(rule(raw_close, "`" + flags, {"rcmd"}, false, pop()));
    rules.push_back(rule(raw_close, "```" + flags, {"rtcmd"}, false, pop()));
    // --- operators and punctuation ---------------------------------------------------------------------
    rules.push_back(rule(op,
                         R"re((?:\.\.\.|\.\.|::|->|=>|\|>|<\||===|!==|==|!=|<=|>=|&&|\|\||<:|>:|\.(?:\|>|<\||===|!==|==|!=|<=|>=|&&|\|\||=>)|\.[-+*/\\^%<>=!&|]=?|[-+*/\\^%|&]=|[÷−⊻]=|[-+*/\\^%<>=!&|~?$]|\.?)re" + operators +
                         R"re(|\.|:))re",
                         code));
    // An ASCII operator with a suffix (`==ₜ`, `+ₚ`), a rule of its own: the operator class is written once in
    // the rule above, and with the suffixes beside it a rule's byte expansion outgrows what a DFA takes.
    rules.push_back(rule(op, R"re(\.?(?:==|[-+*/\\^%<>=!&|~]))re" + suffixes.substr(0, suffixes.size() - 1) + "+",
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
    // A string macro's literal is raw (its `$` is text, its flags close it), `≈` is an operator, and `2im`
    // is a number times a name, as Julia reads them.
    const std::vector<scilex::token> raw      {lex.tokenize("r\"^a$\"i ≈ 2im")};
    const bool                       literals {raw.size() == 7 && raw[1].kind == string_open && raw[2].kind == string_text && raw[2].lexeme == "^a$"
                                               && raw[3].kind == raw_close && raw[3].lexeme == "\"i" && raw[4].kind == op && raw[5].kind == number
                                               && raw[6].kind == ident};
    // [..]' and A' · 'a' and '\n' · :done only (1:end, 1:length are ranges) · $( three times
    return adjoints == 2 && characters == 2 && symbols == 1 && opens == 3 && closes == 3 && hidden && unicode
           && in_code && clean_head && literals;
  }
} // namespace scilex::examples::julia

#endif // SCILEX_EXAMPLE_JULIA_HPP
