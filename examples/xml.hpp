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
 * Comments and CDATA are single regex tokens (dotall + lazy, reusing the triple-
 * quote trick), so a `<` or `&` inside them is literal — they neither open a tag
 * nor start an entity. That is exactly why they are regex rules, not modes.
 *
 * What this grammar covers
 *   - elements: start `<a …>`, end `</a>`, self-closing `<a/>`;
 *   - names with `:` `.` `-` (so a namespaced name lexes as one name);
 *   - attributes `name="…"` / `name='…'`;
 *   - content text, entities `&amp;` / `&#123;`;
 *   - comments `<!--…-->` and CDATA `<![CDATA[…]]>` (one token each).
 *
 * What it does not cover (a realistic example of modes, not a full XML lexer —
 * none of these is excluded in principle, just beyond this sample):
 *   - processing instructions `<?…?>` and `<!DOCTYPE …>` / DTDs;
 *   - namespace semantics (a `:` just lexes as part of the name);
 *   - entity validation, and entities inside attribute values (an attribute value
 *     is one opaque token).
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
      default:             return "?";
    }
  }

  //! \brief A push/pop action targeting \p target (target ignored for pop).
  inline scilex::mode_action go(scilex::mode_action::op operation,
                                const char*             target = "")
  {
    return {.operation = operation, .target = target};
  }

  //! \brief A rule active in \p modes, optionally skipped / with an action.
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

  //! \brief Builds the XML lexer (content is the default mode; tag is pushed).
  inline std::vector<scilex::rule> make_rules()
  {
    using op_t = scilex::mode_action::op;
    const std::vector<std::string> content {};      // content = the default (root) mode (empty in_mode)
    const std::vector<std::string> in_tag  {"tag"};

    std::vector<scilex::rule> rules;
    // --- content: comments / CDATA (one token; inner `<` `&` literal), the tag
    //     openers, entities, then text. Order = munch priority for the `<…`. ------
    rules.push_back(rule(comment, R"re((?s)<!--.*?-->)re", content));
    rules.push_back(rule(cdata, R"re((?s)<!\[CDATA\[.*?\]\]>)re", content));
    rules.push_back(rule(close_tag_open, R"re(</)re", content, false, go(op_t::push, "tag")));
    rules.push_back(rule(tag_open, R"re(<)re", content, false, go(op_t::push, "tag")));
    rules.push_back(rule(entity, R"re(&[A-Za-z#][A-Za-z0-9]*;)re", content));
    rules.push_back(rule(text, R"re([^<&]+)re", content));
    // --- tag: names, attributes, and the closers (which pop back to content) -----
    rules.push_back(rule(ws, R"re(\s+)re", in_tag, /*skip=*/ true));
    rules.push_back(rule(name, R"re([A-Za-z_:][A-Za-z0-9_:.-]*)re", in_tag));
    rules.push_back(rule(eq, R"re(=)re", in_tag));
    rules.push_back(rule(attr_value, R"re("[^"]*"|'[^']*')re", in_tag));
    rules.push_back(rule(self_close, R"re(/>)re", in_tag, false, go(op_t::pop)));
    rules.push_back(rule(tag_close, R"re(>)re", in_tag, false, go(op_t::pop)));
    return rules;
  }

  //! \brief Builds the lexer from its rule list (see \ref make_rules).
  inline scilex::lexer make_lexer()
  {
    return scilex::lexer(make_rules());
  }

  //! \brief A small configuration document exercising every covered context.
  inline constexpr std::string_view sample {
    R"xml(<!-- example configuration -->
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
    // (5) an entity.
    if (!kinds_are(lex.tokenize("&amp;"), {entity})) {
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
