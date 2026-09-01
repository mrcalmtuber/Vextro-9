/*
 * xmltest — libxml2 in ring 3.
 *
 * libxml2 is the largest port in this tree after ICU — 47 sources,
 * 2,539 public entry points — and almost none of that is what this file
 * checks. What it checks is the *shape of the port*: a feature set
 * chosen by substituting 37 tokens into upstream's xmlversion.h, a
 * hand-written config.h underneath it, and four things switched off
 * that a distribution build has on. So every section below is aimed at
 * one of those decisions, or at the exact way WebKit drives the
 * library.
 *
 * ---- WebKit's driving pattern, copied rather than approximated ----
 *
 * Source/WebCore/xml/parser/XMLDocumentParserLibxml2.cpp is the only
 * consumer, and it does three specific things this file repeats:
 *
 *   xmlCreatePushParserCtxt(handlers, 0, 0, 0, 0)  — a *push* parser
 *     with a SAX handler and no initial chunk, fed as bytes arrive off
 *     the network. Not xmlReadMemory; that is the XSLT path.
 *
 *   xmlCtxtUseOptions(parser, XML_PARSE_NOENT | XML_PARSE_HUGE)
 *     — those two flags exactly (line 582).
 *
 *   xmlSetExternalEntityLoader(...) — its own loader, installed so that
 *     an external entity in a document can never become a file read or
 *     a network fetch. That is the XXE defence, and it is the reason
 *     FTP and HTTP are compiled out of this build rather than merely
 *     unused. Section 5 proves the hook works here.
 *
 * ---- where the expected values come from ----
 *
 * Hand-computed, from documents written in this file. An XML parser
 * tested by serialising its own tree and reparsing it agrees with
 * itself; the counts, strings and line numbers below were worked out by
 * reading the document, not by running the library.
 */

#include "vextro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlerror.h>
#include <libxml/dict.h>
#include <libxml/xmlsave.h>
#include <libxml/HTMLparser.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

/* The document most sections parse. Written out here so the expected
 * counts below can be read against it. */
static const char doc_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<library xmlns:m=\"urn:vextro:meta\">\n"
    "  <book id=\"1\" lang=\"en\">\n"
    "    <title>The Structure of Scientific Revolutions</title>\n"
    "    <pages>264</pages>\n"
    "  </book>\n"
    "  <book id=\"2\" lang=\"de\">\n"
    "    <title>Die Verwandlung</title>\n"
    "    <pages>72</pages>\n"
    "  </book>\n"
    "  <m:note>a namespaced element</m:note>\n"
    "</library>\n";

/* ---- the structured error handler, as WebKit installs one ---- */
static int   err_count;
static int   err_line;
static int   err_code;
static char  err_msg[256];

static void on_error(void *user, const xmlError *e) {
    (void)user;
    err_count++;
    err_line = e->line;
    err_code = e->code;
    if (e->message) {
        size_t n = strlen(e->message);
        if (n >= sizeof err_msg) n = sizeof err_msg - 1;
        memcpy(err_msg, e->message, n);
        err_msg[n] = 0;
    }
}

/* ---- an entity loader that refuses, as WebKit installs one ---- */
static int loader_calls;
static xmlParserInputPtr refuse_everything(const char *url, const char *id,
                                           xmlParserCtxtPtr ctxt) {
    (void)url; (void)id; (void)ctxt;
    loader_calls++;
    return NULL;
}

/* Count elements in a subtree, by name if one is given. */
static int count_elements(xmlNodePtr node, const char *name) {
    int n = 0;
    for (; node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE &&
            (!name || xmlStrEqual(node->name, BAD_CAST name)))
            n++;
        n += count_elements(node->children, name);
    }
    return n;
}

int main(void) {
    printf("xmltest: libxml2 %s in ring 3\n", LIBXML_DOTTED_VERSION);

    /* ================================================================
     * 1. the feature set this port was configured with
     * ================================================================
     *
     * These are compile-time assertions about the generated
     * xmlversion.h, and they are the cheapest possible check that the
     * 37-token substitution in the Makefile did what it says. A missing
     * LIBXML_XPATH_ENABLED would otherwise present as section 4 failing
     * to link.
     */
#ifdef LIBXML_PUSH_ENABLED
    check("push parser compiled in — WebKit uses no other", 1);
#else
    check("push parser compiled in — WebKit uses no other", 0);
#endif
#ifdef LIBXML_XPATH_ENABLED
    check("XPath compiled in", 1);
#else
    check("XPath compiled in", 0);
#endif
#ifdef LIBXML_SAX1_ENABLED
    check("SAX1 compiled in", 1);
#else
    check("SAX1 compiled in", 0);
#endif
#ifdef LIBXML_OUTPUT_ENABLED
    check("serialisation compiled in", 1);
#else
    check("serialisation compiled in", 0);
#endif
#ifdef LIBXML_THREAD_ENABLED
    check("threads compiled in", 1);
#else
    check("threads compiled in", 0);
#endif
    /* And the four that are deliberately absent. Asserting an absence
     * is worth as much as asserting a presence here: each of these
     * would have dragged in a library this system does not have, and
     * the build would have failed at link rather than here. */
#ifdef LIBXML_ZLIB_ENABLED
    check("zlib is NOT compiled in (not ported yet)", 0);
#else
    check("zlib is NOT compiled in (not ported yet)", 1);
#endif
#ifdef LIBXML_ICONV_ENABLED
    check("iconv is NOT compiled in (this libc has none)", 0);
#else
    check("iconv is NOT compiled in (this libc has none)", 1);
#endif
#ifdef LIBXML_MODULES_ENABLED
    check("dynamic modules are NOT compiled in (no loader)", 0);
#else
    check("dynamic modules are NOT compiled in (no loader)", 1);
#endif
#ifdef LIBXML_HTTP_ENABLED
    check("nanohttp is NOT compiled in", 0);
#else
    check("nanohttp is NOT compiled in", 1);
#endif

    xmlInitParser();
    check("xmlInitParser returns", 1);
    /* xmlParserVersion is the archive's own idea of its version;
     * LIBXML_VERSION_STRING is the header's. They are generated from the
     * same substitution, so disagreeing means the staged header and the
     * staged archive came from different builds — which is precisely
     * the mismatch CMake's FindLibXml2 would not notice, since it reads
     * only the header. */
    check("the archive and the header agree on the version",
          xmlParserVersion &&
          strcmp(xmlParserVersion, LIBXML_VERSION_STRING) == 0);
    check("LIBXML_VERSION is 21206", LIBXML_VERSION == 21206);

    /* ================================================================
     * 2. a document, parsed whole
     * ================================================================ */
    {
        xmlDocPtr doc = xmlReadMemory(doc_xml, (int)strlen(doc_xml),
                                      "test.xml", NULL,
                                      XML_PARSE_NOENT | XML_PARSE_HUGE);
        check("xmlReadMemory parses the document", doc != NULL);
        if (!doc) {
            printf("xmltest: %d checks, %d failures (nothing parsed)\n",
                   checks, failures + 1);
            return 1;
        }

        xmlNodePtr root = xmlDocGetRootElement(doc);
        check("the root element is <library>",
              root && xmlStrEqual(root->name, BAD_CAST "library"));
        check("there are two <book> elements",
              count_elements(root, "book") == 2);
        check("and two <title> elements",
              count_elements(root, "title") == 2);
        /* library, two books, two titles, two pages, one note. Counted
         * from the document above rather than from a first run — which
         * is how this started at nine and had to be corrected. */
        check("eight elements in all",
              count_elements(root, NULL) == 8);

        /* Attributes, by name. */
        xmlChar *lang = NULL;
        for (xmlNodePtr n = root->children; n; n = n->next)
            if (n->type == XML_ELEMENT_NODE) {
                lang = xmlGetProp(n, BAD_CAST "lang");
                break;
            }
        check("the first book's lang attribute is \"en\"",
              lang && xmlStrEqual(lang, BAD_CAST "en"));
        if (lang) xmlFree(lang);

        /*
         * The namespace. `m:note` is in urn:vextro:meta, and the prefix
         * is declared on the root — so resolving it is the namespace
         * machinery working across the tree rather than a string
         * compare on the name.
         */
        int found_ns = 0;
        for (xmlNodePtr n = root->children; n; n = n->next)
            if (n->type == XML_ELEMENT_NODE &&
                xmlStrEqual(n->name, BAD_CAST "note"))
                found_ns = n->ns && xmlStrEqual(n->ns->href,
                                                BAD_CAST "urn:vextro:meta");
        check("the m: prefix resolves to urn:vextro:meta", found_ns);

        check("the document reports encoding UTF-8",
              doc->encoding && xmlStrEqual(doc->encoding, BAD_CAST "UTF-8"));

        /* Serialise, and check a fact about the output rather than
         * round-tripping it: the declaration comes back and the
         * namespace declaration survives. */
        xmlChar *out = NULL;
        int outlen = 0;
        xmlDocDumpMemory(doc, &out, &outlen);
        check("the tree serialises", out != NULL && outlen > 0);
        check("with an XML declaration",
              out && strncmp((char *)out, "<?xml ", 6) == 0);
        check("and the namespace declaration is still there",
              out && strstr((char *)out, "urn:vextro:meta") != NULL);
        if (out) xmlFree(out);

        xmlFreeDoc(doc);
    }

    /* ================================================================
     * 3. the push parser, fed the way bytes actually arrive
     * ================================================================
     *
     * This is the interface WebKit uses and the only one it uses. The
     * document is fed in four chunks with the split points chosen to be
     * awkward: one lands **inside a tag name**, one **inside an
     * attribute value**, and one **in the middle of a multi-byte UTF-8
     * sequence**. Those are the states the parser has to be able to
     * suspend in, and a buffering bug shows up as a lost or doubled
     * character rather than as a failure to parse.
     */
    {
        /* "Fahrvergnügen" — the ü is two bytes, 0xC3 0xBC, and the
         * third chunk boundary falls between them. */
        static const char push_doc[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<root attr=\"a value with spaces\">"
            "<child>Fahrvergn\xC3\xBCgen</child>"
            "</root>";
        const size_t total = sizeof push_doc - 1;

        /* Split inside "<ro|ot", inside the attribute value, and
         * between the two bytes of the ü. */
        const char *mid_tag = strstr(push_doc, "<root") + 3;
        const char *mid_attr = strstr(push_doc, "with spaces") + 4;
        const char *mid_utf8 = strstr(push_doc, "\xC3\xBC") + 1;

        size_t c1 = (size_t)(mid_tag  - push_doc);
        size_t c2 = (size_t)(mid_attr - push_doc);
        size_t c3 = (size_t)(mid_utf8 - push_doc);
        check("the chunk boundaries are in the awkward places",
              c1 > 0 && c1 < c2 && c2 < c3 && c3 < total);

        xmlParserCtxtPtr ctxt = xmlCreatePushParserCtxt(NULL, NULL,
                                                        NULL, 0, "push.xml");
        check("xmlCreatePushParserCtxt with no initial chunk",
              ctxt != NULL);
        if (ctxt) {
            xmlCtxtUseOptions(ctxt, XML_PARSE_NOENT | XML_PARSE_HUGE);

            int rc = 0;
            rc |= xmlParseChunk(ctxt, push_doc, (int)c1, 0);
            rc |= xmlParseChunk(ctxt, push_doc + c1, (int)(c2 - c1), 0);
            rc |= xmlParseChunk(ctxt, push_doc + c2, (int)(c3 - c2), 0);
            rc |= xmlParseChunk(ctxt, push_doc + c3,
                                (int)(total - c3), 1);
            check("four chunks parse with no error", rc == 0);
            check("and the parser says the document is well-formed",
                  ctxt->wellFormed);

            xmlDocPtr doc = ctxt->myDoc;
            check("a document came out", doc != NULL);
            if (doc) {
                xmlNodePtr root = xmlDocGetRootElement(doc);
                check("whose root survived the mid-tag split",
                      root && xmlStrEqual(root->name, BAD_CAST "root"));

                xmlChar *a = root ? xmlGetProp(root, BAD_CAST "attr") : NULL;
                check("and whose attribute survived the mid-value split",
                      a && xmlStrEqual(a, BAD_CAST "a value with spaces"));
                if (a) xmlFree(a);

                xmlNodePtr child = root ? root->children : NULL;
                while (child && child->type != XML_ELEMENT_NODE)
                    child = child->next;
                xmlChar *text = child ? xmlNodeGetContent(child) : NULL;
                check("and the ü survived a split between its two bytes",
                      text && xmlStrEqual(text,
                                  BAD_CAST "Fahrvergn\xC3\xBCgen"));
                check("with the byte length UTF-8 gives it",
                      text && xmlStrlen(text) == 14);
                if (text) xmlFree(text);
                xmlFreeDoc(doc);
            }
            xmlFreeParserCtxt(ctxt);
        }
    }

    /* ================================================================
     * 4. XPath
     * ================================================================ */
    {
        xmlDocPtr doc = xmlReadMemory(doc_xml, (int)strlen(doc_xml),
                                      "test.xml", NULL, XML_PARSE_NOENT);
        xmlXPathContextPtr xp = doc ? xmlXPathNewContext(doc) : NULL;
        check("an XPath context", xp != NULL);

        if (xp) {
            xmlXPathObjectPtr r =
                xmlXPathEvalExpression(BAD_CAST "//book", xp);
            check("//book is a node set of two",
                  r && r->type == XPATH_NODESET &&
                  r->nodesetval && r->nodesetval->nodeNr == 2);
            if (r) xmlXPathFreeObject(r);

            r = xmlXPathEvalExpression(BAD_CAST "count(//title)", xp);
            check("count(//title) is the number 2",
                  r && r->type == XPATH_NUMBER && r->floatval == 2.0);
            if (r) xmlXPathFreeObject(r);

            /* Sum over element text, which makes XPath convert strings
             * to numbers — 264 + 72. */
            r = xmlXPathEvalExpression(BAD_CAST "sum(//pages)", xp);
            check("sum(//pages) is 336", r && r->type == XPATH_NUMBER &&
                  r->floatval == 336.0);
            if (r) xmlXPathFreeObject(r);

            /* A predicate on an attribute. */
            r = xmlXPathEvalExpression(
                    BAD_CAST "string(//book[@lang='de']/title)", xp);
            check("the German book's title comes back",
                  r && r->type == XPATH_STRING && r->stringval &&
                  xmlStrEqual(r->stringval, BAD_CAST "Die Verwandlung"));
            if (r) xmlXPathFreeObject(r);

            /* A registered namespace prefix — XPath cannot use the
             * document's own, which is a rule people get wrong. */
            xmlXPathRegisterNs(xp, BAD_CAST "meta", BAD_CAST "urn:vextro:meta");
            r = xmlXPathEvalExpression(BAD_CAST "//meta:note", xp);
            check("a registered prefix selects the namespaced element",
                  r && r->type == XPATH_NODESET &&
                  r->nodesetval && r->nodesetval->nodeNr == 1);
            if (r) xmlXPathFreeObject(r);

            /* A syntactically invalid expression must return NULL
             * rather than an empty result. */
            r = xmlXPathEvalExpression(BAD_CAST "//[", xp);
            check("a malformed expression gives NULL", r == NULL);
            if (r) xmlXPathFreeObject(r);

            xmlXPathFreeContext(xp);
        }
        if (doc) xmlFreeDoc(doc);
    }

    /* ================================================================
     * 5. the entity loader hook — WebKit's XXE defence
     * ================================================================
     *
     * A document that declares an external entity and then uses it. The
     * loader installed here refuses every request, exactly as WebKit's
     * does. Two things must be true afterwards: the loader was
     * *consulted*, and the parse did not go looking for the file on its
     * own.
     *
     * This is the check that makes "FTP and HTTP are compiled out"
     * meaningful rather than incidental — it establishes that the hook
     * they would have been reached through is under the caller's
     * control on this build.
     */
    {
        static const char xxe[] =
            "<?xml version=\"1.0\"?>\n"
            "<!DOCTYPE root [\n"
            "  <!ENTITY secret SYSTEM \"file:///etc/ca-bundle.crt\">\n"
            "]>\n"
            "<root>&secret;</root>\n";

        xmlExternalEntityLoader saved = xmlGetExternalEntityLoader();
        check("the current entity loader can be read", saved != NULL);

        loader_calls = 0;
        xmlSetExternalEntityLoader(refuse_everything);
        check("and replaced",
              xmlGetExternalEntityLoader() == refuse_everything);

        xmlDocPtr doc = xmlReadMemory(xxe, (int)strlen(xxe), "xxe.xml",
                                      NULL, XML_PARSE_NOENT);
        check("the loader was consulted for the external entity",
              loader_calls > 0);
        if (doc) {
            xmlNodePtr root = xmlDocGetRootElement(doc);
            xmlChar *text = root ? xmlNodeGetContent(root) : NULL;
            /* Whatever comes back, it must not be the file. */
            check("and no part of the file reached the document",
                  !text || strstr((char *)text, "BEGIN CERTIFICATE") == NULL);
            if (text) xmlFree(text);
            xmlFreeDoc(doc);
        } else {
            check("and no part of the file reached the document", 1);
        }

        xmlSetExternalEntityLoader(saved);
        check("the original loader goes back",
              xmlGetExternalEntityLoader() == saved);
    }

    /* ================================================================
     * 6. errors, through the structured handler WebKit installs
     * ================================================================
     *
     * WebKit reads `xmlError::line` to place a parse error in the
     * source, so the line number is part of the contract and not just
     * diagnostic text. The document below is well-formed for four lines
     * and then closes the wrong tag on the fifth.
     */
    {
        static const char bad[] =
            "<?xml version=\"1.0\"?>\n"   /* 1 */
            "<a>\n"                       /* 2 */
            "  <b>text</b>\n"             /* 3 */
            "  <c>more\n"                 /* 4 */
            "</a>\n";                     /* 5 — closes <a>, not <c> */

        err_count = 0; err_line = 0; err_code = 0; err_msg[0] = 0;
        xmlSetStructuredErrorFunc(NULL, on_error);

        xmlDocPtr doc = xmlReadMemory(bad, (int)strlen(bad), "bad.xml",
                                      NULL, XML_PARSE_NOENT);
        check("a mismatched end tag is refused", doc == NULL);
        check("the structured handler fired", err_count > 0);
        check("on line 5, where the wrong tag is", err_line == 5);
        check("with XML_ERR_TAG_NAME_MISMATCH",
              err_code == XML_ERR_TAG_NAME_MISMATCH);
        check("and a message that names both tags",
              strstr(err_msg, "c") != NULL && strstr(err_msg, "a") != NULL);
        if (doc) xmlFreeDoc(doc);

        /* Well-formed input must leave the handler alone. */
        err_count = 0;
        doc = xmlReadMemory(doc_xml, (int)strlen(doc_xml), "ok.xml",
                            NULL, XML_PARSE_NOENT);
        check("a good document raises nothing", err_count == 0);
        if (doc) xmlFreeDoc(doc);

        xmlSetStructuredErrorFunc(NULL, NULL);
    }

    /* ================================================================
     * 7. encodings, without iconv and without ICU
     * ================================================================
     *
     * The section that checks the decision in
     * third_party/libxml2-port/config.h. Both converters below are
     * *built in* — they are the ones libxml2 implements itself, and
     * they are all this build has.
     */
    {
        /* UTF-16LE with a byte-order mark. Written as bytes so the
         * document really is UTF-16 rather than a UTF-8 string that
         * says so. */
        static const unsigned char utf16le[] = {
            0xFF, 0xFE,
            '<',0, '?',0, 'x',0, 'm',0, 'l',0, ' ',0,
            'v',0, 'e',0, 'r',0, 's',0, 'i',0, 'o',0, 'n',0,
            '=',0, '"',0, '1',0, '.',0, '0',0, '"',0, '?',0, '>',0,
            '<',0, 'a',0, '>',0,
            0xE9, 0x00,                     /* é, U+00E9 */
            '<',0, '/',0, 'a',0, '>',0
        };
        xmlDocPtr doc = xmlReadMemory((const char *)utf16le,
                                      (int)sizeof utf16le, "u16.xml",
                                      NULL, XML_PARSE_NOENT);
        check("a UTF-16LE document with a BOM parses", doc != NULL);
        if (doc) {
            xmlNodePtr root = xmlDocGetRootElement(doc);
            xmlChar *t = root ? xmlNodeGetContent(root) : NULL;
            /* The tree is always UTF-8, whatever went in: é is C3 A9. */
            check("and its content is transcoded to UTF-8",
                  t && t[0] == 0xC3 && t[1] == 0xA9 && t[2] == 0);
            if (t) xmlFree(t);
            xmlFreeDoc(doc);
        }

        /* ISO-8859-1, declared. This is the path WITH_ISO8859X provides
         * and the reason it is on. */
        static const unsigned char latin1[] =
            "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>"
            "<a>caf\xE9</a>";
        doc = xmlReadMemory((const char *)latin1,
                            (int)sizeof latin1 - 1, "l1.xml",
                            NULL, XML_PARSE_NOENT);
        check("an ISO-8859-1 document parses", doc != NULL);
        if (doc) {
            xmlNodePtr root = xmlDocGetRootElement(doc);
            xmlChar *t = root ? xmlNodeGetContent(root) : NULL;
            check("and \"caf\\xE9\" becomes \"caf\\xC3\\xA9\"",
                  t && xmlStrEqual(t, BAD_CAST "caf\xC3\xA9"));
            if (t) xmlFree(t);
            xmlFreeDoc(doc);
        }

        /* An encoding nothing here implements must be *refused*, not
         * silently treated as bytes. Shift_JIS is iconv's or ICU's
         * job, and this build has neither. */
        err_count = 0;
        xmlSetStructuredErrorFunc(NULL, on_error);
        static const char sjis[] =
            "<?xml version=\"1.0\" encoding=\"Shift_JIS\"?><a>x</a>";
        doc = xmlReadMemory(sjis, (int)strlen(sjis), "sj.xml", NULL,
                            XML_PARSE_NOENT);
        check("an encoding this build cannot convert is reported",
              doc == NULL || err_count > 0);
        if (doc) xmlFreeDoc(doc);
        xmlSetStructuredErrorFunc(NULL, NULL);
    }

    /* ================================================================
     * 8. the dictionary and the string helpers
     * ================================================================
     *
     * xmlDictLookup and xmlStrdup/xmlFree are on WebKit's list of
     * directly-called functions. The dictionary is an interning table:
     * the same name looked up twice must give the *same pointer*, which
     * is what makes name comparison in the parser a pointer compare.
     */
    {
        xmlDictPtr dict = xmlDictCreate();
        check("a dictionary is created", dict != NULL);
        if (dict) {
            const xmlChar *a = xmlDictLookup(dict, BAD_CAST "element", -1);
            const xmlChar *b = xmlDictLookup(dict, BAD_CAST "element", -1);
            const xmlChar *c = xmlDictLookup(dict, BAD_CAST "different", -1);
            check("the same name interns to the same pointer", a == b);
            check("a different name to a different pointer", a != c);
            check("and the string is intact",
                  a && xmlStrEqual(a, BAD_CAST "element"));
            check("xmlDictOwns recognises its own", xmlDictOwns(dict, a) == 1);
            xmlDictFree(dict);
        }

        xmlChar *s = xmlStrdup(BAD_CAST "vextro");
        check("xmlStrdup copies", s && xmlStrEqual(s, BAD_CAST "vextro"));
        check("xmlStrlen agrees", s && xmlStrlen(s) == 6);
        xmlChar *cat = xmlStrcat(s, BAD_CAST " 9");
        check("xmlStrcat appends",
              cat && xmlStrEqual(cat, BAD_CAST "vextro 9"));
        if (cat) xmlFree(cat);

        xmlChar *n = xmlStrndup(BAD_CAST "truncate me", 8);
        check("xmlStrndup stops where told",
              n && xmlStrEqual(n, BAD_CAST "truncate"));
        if (n) xmlFree(n);
    }

    /* ================================================================
     * 9. HTML, which is on because upstream has it on
     * ================================================================
     *
     * WebKit has its own HTML parser and never calls this one. It is
     * compiled because WITH_HTML is upstream's default and turning it
     * off would be a divergence to maintain — so one check that it
     * works, rather than none, and no more than that.
     */
    {
        static const char html[] =
            "<html><body><p>unclosed paragraph<p>second</body></html>";
        htmlDocPtr doc = htmlReadMemory(html, (int)strlen(html), "t.html",
                                        NULL, HTML_PARSE_NOWARNING |
                                              HTML_PARSE_NOERROR);
        check("the HTML parser recovers unclosed tags", doc != NULL);
        if (doc) {
            xmlNodePtr root = xmlDocGetRootElement(doc);
            check("and produces two <p> elements",
                  count_elements(root, "p") == 2);
            xmlFreeDoc(doc);
        }
    }

    xmlCleanupParser();
    printf("xmltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
