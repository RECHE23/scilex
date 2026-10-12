/*!
 * \file xml.hpp
 * \brief An XML lexer — the shallow content<->tag modes example.
 *
 * A different shape of contextual lexing from Python's f-strings: just TWO modes,
 * `content` and `tag`, with a shallow push/pop and NO stack nesting. Element
 * nesting (`<a><b/></a>`) is a parser's job, not the lexer's — the mode stack only
 * flips between content (text, entities, the tag openers) and tag (names,
 * attributes, the closers), and is at most one deep. Modes are not always
 * deep-nesting; this is the counter-example to f-strings.
 *
 * Comments and CDATA are single regex tokens, so a `<` or `&` inside them is
 * literal — they neither open a tag nor start an entity. That is exactly why they
 * are regex rules, not modes. Each is written so that its match() is its longest
 * match (no lazy `.*?`): the body may not contain the terminator, which is what
 * the lazy form means, and in that form the rule runs on the mode's DFA instead
 * of on Pike, where an unterminated `<!--` rescanned the rest of the input.
 *
 * What this grammar covers (`make xml-oracle` holds it to expat, the parser Python
 * ships, over the XML files of Python's test suite)
 *   - elements: start `<a …>`, end `</a>`, self-closing `<a/>`;
 *   - names as XML 1.0 defines them, Unicode letters included (a namespaced name
 *     lexes as one name);
 *   - attributes `name="…"` / `name='…'`;
 *   - content text, entities `&amp;` / `&#123;` / `&#x1F;`;
 *   - comments `<!--…-->`, CDATA `<![CDATA[…]]>`, processing instructions and the
 *     XML declaration `<?…?>`, and the document type `<!DOCTYPE … [ … ]>` with its
 *     internal subset (one token each);
 *   - text, comments and values of any byte, as in a document in ISO-8859-1.
 *
 * What it does not cover (beyond this example, not excluded in principle):
 *   - namespace semantics (a `:` just lexes as part of the name);
 *   - the declarations inside a document type, which are one token with it;
 *   - entity validation, and entities inside attribute values (an attribute value
 *     is one opaque token);
 *   - a non-ASCII name in an encoding other than UTF-8.
 *
 * The modes
 *   `content` is the default (root) mode — a document starts in content. A `<` or
 *   `</` pushes `tag`; a `>` or `/>` pops back to content. SciLex's initial mode is
 *   always the default one, so content lives there (an empty `in_mode`) and `tag`
 *   is the single pushed mode. The munch order resolves the `<…` openers: `<!--`
 *   and `<![CDATA[` are longer than `<` (so they win), and `</` beats `<`.
 */
#ifndef SCILEX_EXAMPLE_XML_HPP
#define SCILEX_EXAMPLE_XML_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <scilex/scilex.hpp>

namespace scilex::examples::xml {

  //! \brief XML token kinds. Whitespace inside a tag is a skip rule.
  enum kind
  {
    text,           //!< content text (between markup)
    entity,         //!< `&amp;` / `&#123;`
    comment,        //!< `<!--…-->` (one token)
    cdata,          //!< `<![CDATA[…]]>` (one token; inner `<` `&` are literal)
    tag_open,       //!< `<` starting a start-tag — pushes `tag`
    close_tag_open, //!< `</` starting an end-tag — pushes `tag`
    name,           //!< element / attribute name
    eq,             //!< `=`
    attr_value,     //!< `"…"` / `'…'`
    tag_close,      //!< `>` — pops back to content
    self_close,     //!< `/>` — pops back to content
    ws,             //!< whitespace inside a tag (skipped)
    pi,             //!< `<?…?>`, the XML declaration included (one token)
    doctype,        //!< `<!DOCTYPE …>`, its internal subset included (one token)
  };

  //! \brief A printable name for each kind.
  inline const char* kind_name(int k)
  {
    switch (k) {
      case text:           return "TEXT";
      case entity:         return "ENTITY";
      case comment:        return "COMMENT";
      case cdata:          return "CDATA";
      case tag_open:       return "TAG<";
      case close_tag_open: return "CLOSE</";
      case name:           return "NAME";
      case eq:             return "=";
      case attr_value:     return "ATTR";
      case tag_close:      return ">";
      case self_close:     return "/>";
      case ws:             return "WS";
      case pi:             return "PI";
      case doctype:        return "DOCTYPE";
      default:             return "?";
    }
  }

  //! \brief A push/pop action targeting \p target (target ignored for pop).
  inline scilex::mode_action go(scilex::mode_action::op operation,
                                const char*             target = "")
  {
    return {.operation = operation, .target = target};
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action, its pattern read under \p flags.
  inline scilex::rule rule(int                                kind,
                           const std::string&                 pattern,
                           std::vector<std::string>           modes,
                           bool                               skip   = false,
                           std::optional<scilex::mode_action> action = std::nullopt,
                           real::flags                        flags  = real::flags::none)
  {
    scilex::rule r {.kind = kind, .pattern = real::regex(pattern, flags), .skip = skip};
    r.in_mode = std::move(modes);
    r.action  = action;
    return r;
  }

  //! \brief Builds the XML lexer (content is the default mode; tag is pushed).
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;
    const std::vector<std::string> content {};      // content = the default (root) mode (empty in_mode)
    const std::vector<std::string> in_tag  {"tag"};
    // XML 1.0's NameStartChar and NameChar.
    const std::string start                {R"re(:A-Z_a-z\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02FF\u0370-\u037D\u037F-\u1FFF\u200C\u200D)re"
                                            R"re(\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF\uFDF0-\uFFFD\U00010000-\U000EFFFF)re"};
    const std::string xml_name             {"[" + start + "][" + start + R"re(.0-9\u00B7\u0300-\u036F\u203F\u2040-]*)re"};
    // Equal to (?s)<!--.*?--> and (?s)<!\[CDATA\[.*?\]\]> on every input (checked exhaustively on every
    // suffix of up to ten bytes over the delimiters' alphabet), and DFA-faithful where the lazy forms are not.
    const std::string comment_pattern {R"re(<!--(?:[^-]|-[^-]|--+[^->])*-*-->)re"};
    const std::string pi_pattern      {R"re(<\?(?:[^?]|\?+[^?>])*\?+>)re"};
    const std::string quoted          {R"re("[^"]*"|'[^']*')re"};
    // The internal subset: declarations, comments and processing instructions, up to its `]`; a comment, an
    // instruction or a quoted value is read whole, so a `]` inside one does not end it. A `<` is taken on its
    // own only where it opens neither a comment nor an instruction, so that each element has one reading: the
    // first match is then the longest, and the mode stays on its DFA.
    const std::string subset {R"re(\[(?:)re" + comment_pattern + "|" + pi_pattern + "|" + quoted +
                              R"re(|[^\]"'<]|<[^!?\]"'<]|<![^-\]"'<]|<!-[^-\]"'<])*\])re"};
    // Text, comments, instructions, declarations and values hold any byte, as in a document declared in
    // ISO-8859-1; names stay UTF-8.
    const real::flags any_byte {real::flags::bytes};

    std::vector<scilex::rule> rules;
    // --- content: comments / CDATA / instructions / the document type (one token each; inner `<` `&`
    //     literal), the tag openers, entities, then text. Order = munch priority for the `<…`. ------------
    rules.push_back(rule(comment, comment_pattern, content, false, std::nullopt, any_byte));
    rules.push_back(rule(cdata, R"re(<!\[CDATA\[(?:[^\]]|\][^\]]|\]\]+[^\]>])*\]*\]\]>)re", content, false, std::nullopt, any_byte));
    rules.push_back(rule(pi, pi_pattern, content, false, std::nullopt, any_byte));
    rules.push_back(rule(doctype, "<!DOCTYPE(?:" + quoted + R"re(|[^\["'>])*(?:)re" + subset + R"re(\s*)?>)re", content, false,
                         std::nullopt, any_byte));
    rules.push_back(rule(close_tag_open, R"re(</)re", content, false, go(op_t::push, "tag")));
    rules.push_back(rule(tag_open, R"re(<)re", content, false, go(op_t::push, "tag")));
    rules.push_back(rule(entity, "&(?:#[0-9]+|#x[0-9A-Fa-f]+|" + xml_name + ");", content));
    rules.push_back(rule(text, R"re([^<&]+)re", content, false, std::nullopt, any_byte));
    // --- tag: names, attributes, and the closers (which pop back to content) -----
    rules.push_back(rule(ws, R"re(\s+)re", in_tag, /*skip=*/ true));
    rules.push_back(rule(name, xml_name, in_tag));
    rules.push_back(rule(eq, R"re(=)re", in_tag));
    rules.push_back(rule(attr_value, quoted, in_tag, false, std::nullopt, any_byte));
    rules.push_back(rule(self_close, R"re(/>)re", in_tag, false, go(op_t::pop)));
    rules.push_back(rule(tag_close, R"re(>)re", in_tag, false, go(op_t::pop)));
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

  //! \brief A small configuration document exercising every covered context.
  inline constexpr std::string_view sample {
    R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE config [ <!ENTITY product "SciLex"> ]>
<!-- example configuration -->
<config env="prod" debug='off'>
  <name>SciLex</name>
  <version major="2026" minor="6"/>
  <paths>
    <path>/usr/local</path>
    <path>./build</path>
  </paths>
  <note>Use &amp; and &#38; with care; raw: <![CDATA[ <tag> & </nope> ]]> done.</note>
  <empty/>
</config>
)xml"};

  //! \brief True iff \p toks has exactly the kinds \p want, in order.
  inline bool kinds_are(const std::vector<scilex::token>& toks,
                        const std::vector<int>&           want)
  {
    if (toks.size() != want.size()) {
      return false;
    }
    for (std::size_t i {0}; i < want.size(); ++i) {
      if (toks[i].kind != want[i]) {
        return false;
      }
    }
    return true;
  }

  //! \brief Self-check (so `make example` gates, not just builds). \return true on success.
  inline bool self_check()
  {
    const scilex::lexer lex {make_lexer()};

    // (1) a start tag with an attribute, content, and an end tag.
    if (!kinds_are(lex.tokenize(R"(<a x="1">hi</a>)"),
                   {tag_open, name, name, eq, attr_value, tag_close,
                    text, close_tag_open, name, tag_close})) {
      return false;
    }
    // (2) a self-closing tag.
    if (!kinds_are(lex.tokenize("<br/>"), {tag_open, name, self_close})) {
      return false;
    }
    // (3) a comment is one token (its inner content is not interpreted).
    if (!kinds_are(lex.tokenize("<!-- c -->"), {comment})) {
      return false;
    }
    // (4) CDATA is one token: the inner `<`, `&`, `</` are literal, no tag/entity.
    if (!kinds_are(lex.tokenize("<![CDATA[ <raw> & </x> ]]>"), {cdata})) {
      return false;
    }
    // (5) entities, by name and by hexadecimal code.
    if (!kinds_are(lex.tokenize("&amp;&#x1F;"), {entity, entity})) {
      return false;
    }
    // (5b) the XML declaration and a document type with an internal subset, one token each (a `]` inside a
    //      quoted value does not end the subset).
    if (!kinds_are(lex.tokenize(R"(<?xml version="1.0"?><!DOCTYPE a [<!ENTITY e "]">]>)"), {pi, doctype})) {
      return false;
    }
    // (6) nested elements cycle content<->tag — NOT stack depth: <a><b/></a> pops
    //     back to content after each tag (the element nesting is a parser concern).
    if (!kinds_are(lex.tokenize("<a><b/></a>"),
                   {tag_open, name, tag_close, tag_open, name, self_close,
                    close_tag_open, name, tag_close})) {
      return false;
    }
    // (7) an unterminated tag reports the opening `<` (error #3).
    try {
      (void)lex.tokenize("<a");
      return false;
    }
    catch (const scilex::lex_error& error) {
      if (error.where().offset != 0) {
        return false;
      }
    }
    return true;
  }
} // namespace scilex::examples::xml

#endif // SCILEX_EXAMPLE_XML_HPP
