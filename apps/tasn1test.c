/*
 * tasn1test — libtasn1 in ring 3.
 *
 * libtasn1 is a DER encoder and decoder. Upstream's own test suite
 * proves it encodes DER correctly and is not in question here; what is
 * in question is this *port* of it — a hand-written config.h, a gnulib
 * slice cut down to two source files, and a freestanding C library
 * underneath that had no WORD_BIT and no strverscmp until this week.
 * The checks below are chosen for what each would catch if one of those
 * were subtly wrong.
 *
 * ---- the definitions are WebKit's, not mine ----
 *
 * The ASN.1 module compiled into this file is a byte-for-byte copy of
 * s_WebCryptoASN1 from
 *
 *     Source/WebCore/PAL/pal/crypto/tasn1/Utilities.cpp
 *
 * which is the only place in WPE WebKit that uses this library at all.
 * It is not a table anyone reads: asn1Parser produced it from a
 * WebCrypto.asn module, and each of those integers is a packed set of
 * type and flag bits. Copying it rather than writing a smaller module of
 * my own is the point — if this port cannot turn *that* array into a
 * tree, then finding the library was worth nothing, and a module I wrote
 * to be easy would not have told me.
 *
 * The eight functions exercised are likewise the eight Utilities.cpp
 * calls, and no others: asn1_array2tree, asn1_create_element,
 * asn1_der_decoding2, asn1_read_value, asn1_read_value_type,
 * asn1_der_coding, asn1_write_value, asn1_delete_structure.
 *
 * ---- and the bytes are OpenSSL's ----
 *
 * Every structure decoded here came out of the build machine's OpenSSL;
 * apps/tasn1_ref.h has the commands. A round trip through this library's
 * own encoder and back would agree with itself even if both halves
 * shared a wrong idea of how an OBJECT IDENTIFIER is packed, which is
 * exactly the failure a port can introduce. The re-encoding checks below
 * therefore compare against OpenSSL's bytes rather than against the
 * input to the decode.
 *
 * ---- what would break, and where it would show ----
 *
 *   A wrong SIZEOF_UNSIGNED_LONG_INT in config.h is not a compile error.
 *   It is used twenty-one times in the tag and length arithmetic, and a
 *   32-bit answer on a 64-bit build gives a decoder that is right about
 *   everything short and wrong about something long. The 1024-bit RSA
 *   modulus and the 129-byte length are here for that.
 *
 *   A missing WORD_BIT would have been a compile error, but a *wrong*
 *   one would not: _asn1_hash_name rotates by (WORD_BIT - 9), and a hash
 *   that collides differently still works — until asn1_create_element
 *   looks up a name and finds the wrong node. Every element read below
 *   is read by name through that hash.
 *
 *   strverscmp is reached by exactly one function, asn1_check_version,
 *   and the last section calls it three ways. If gnulib's source had not
 *   been compiled in, this program would not have linked; if it had been
 *   compiled and were wrong, only a comparison that must answer "no"
 *   would show it.
 */

#include "vextro.h"

#include <stdio.h>
#include <string.h>

#include <libtasn1.h>

#include "tasn1_ref.h"

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

/* The same, for checks whose interesting detail is the library's own
 * return code — an asn1_* function has thirty of them and "it did not
 * return SUCCESS" is the least useful thing a failing test can say. */
static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/*
 * The WebCrypto module, exactly as WPE WebKit carries it. Do not tidy
 * the integers: they are asn1Parser's output, and the array is a
 * verbatim copy so that "WebKit's definitions build here" is a claim
 * this file can actually make.
 */
static const asn1_static_node webcrypto_asn1[] = {
    { "WebCrypto", 536872976, NULL },
    { NULL, 1073741836, NULL },
    { "SubjectPublicKeyInfo", 1610612741, NULL },
    { "algorithm", 1073741826, "AlgorithmIdentifier"},
    { "subjectPublicKey", 6, NULL },
    { "AlgorithmIdentifier", 1610612741, NULL },
    { "algorithm", 1073741836, NULL },
    { "parameters", 541081613, NULL },
    { "algorithm", 1, NULL },
    { "PrivateKeyInfo", 1610612741, NULL },
    { "version", 1073741826, "Version"},
    { "privateKeyAlgorithm", 1073741826, "PrivateKeyAlgorithmIdentifier"},
    { "privateKey", 1073741826, "PrivateKey"},
    { "attributes", 536895490, "Attributes"},
    { NULL, 4104, "0"},
    { "Version", 1073741827, NULL },
    { "PrivateKeyAlgorithmIdentifier", 1073741826, "AlgorithmIdentifier"},
    { "PrivateKey", 1073741831, NULL },
    { "CurvePrivateKey", 1073741831, NULL },
    { "Attributes", 1610612751, NULL },
    { NULL, 2, "Attribute"},
    { "Attribute", 1610612741, NULL },
    { "type", 1073741836, NULL },
    { "values", 2, "AttributeSetValue"},
    { "AttributeSetValue", 1610612751, NULL },
    { NULL, 13, NULL },
    { "ECParameters", 1610612754, NULL },
    { "namedCurve", 12, NULL },
    { "ECPrivateKey", 1610612741, NULL },
    { "version", 1073741827, NULL },
    { "privateKey", 1073741831, NULL },
    { "parameters", 1610637314, "ECParameters"},
    { NULL, 2056, "0"},
    { "publicKey", 536895494, NULL },
    { NULL, 2056, "1"},
    { "RSAPublicKey", 1610612741, NULL },
    { "modulus", 1073741827, NULL },
    { "publicExponent", 3, NULL },
    { "RSAPrivateKey", 1610612741, NULL },
    { "version", 1073741826, "Version"},
    { "modulus", 1073741827, NULL },
    { "publicExponent", 1073741827, NULL },
    { "privateExponent", 1073741827, NULL },
    { "prime1", 1073741827, NULL },
    { "prime2", 1073741827, NULL },
    { "exponent1", 1073741827, NULL },
    { "exponent2", 1073741827, NULL },
    { "coefficient", 1073741827, NULL },
    { "otherPrimeInfos", 16386, "OtherPrimeInfos"},
    { "OtherPrimeInfos", 1612709899, NULL },
    { "MAX", 1074266122, "1"},
    { NULL, 2, "OtherPrimeInfo"},
    { "OtherPrimeInfo", 536870917, NULL },
    { "prime", 1073741827, NULL },
    { "exponent", 1073741827, NULL },
    { "coefficient", 3, NULL },
    { NULL, 0, NULL }
};

int main(void) {
    printf("tasn1test: libtasn1 %s in ring 3\n", ASN1_VERSION);

    /* ================================================================
     * 1. the module compiles into a tree
     * ================================================================
     *
     * asn1_array2tree walks 57 rows, allocates a node for each, resolves
     * every string reference in the third column against a name in the
     * first, and hashes every name. It is the single call that has to
     * work before anything else can, and it is the one that would fail
     * if the node hash were wrong.
     */
    asn1_node defs = NULL;
    char errdesc[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    errdesc[0] = 0;

    int rc = asn1_array2tree(webcrypto_asn1, &defs, errdesc);
    check("asn1_array2tree builds WebKit's WebCrypto module",
          rc == ASN1_SUCCESS);
    check("and leaves no error description behind", errdesc[0] == 0);
    check("and hands back a tree", defs != NULL);
    if (rc != ASN1_SUCCESS || !defs) {
        printf("tasn1test: %d checks, %d failures (module would not "
               "build: %s)\n", checks, failures + 1, errdesc);
        return 1;
    }

    /*
     * Every name the WebKit code creates an element from must resolve.
     * asn1_create_element is the lookup, so this is the hash table
     * exercised across the whole module rather than at one name.
     */
    static const char *const wanted[] = {
        "WebCrypto.SubjectPublicKeyInfo", "WebCrypto.PrivateKeyInfo",
        "WebCrypto.RSAPublicKey",         "WebCrypto.RSAPrivateKey",
        "WebCrypto.ECPrivateKey",         "WebCrypto.ECParameters",
        "WebCrypto.AlgorithmIdentifier",  "WebCrypto.CurvePrivateKey",
    };
    for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
        asn1_node n = NULL;
        int r = asn1_create_element(defs, wanted[i], &n);
        check(wanted[i], r == ASN1_SUCCESS && n != NULL);
        if (n) asn1_delete_structure(&n);
    }

    /* A name that is not in the module must be refused, not invented. */
    {
        asn1_node n = NULL;
        int r = asn1_create_element(defs, "WebCrypto.NoSuchThing", &n);
        check("a name not in the module gives ASN1_ELEMENT_NOT_FOUND",
              r == ASN1_ELEMENT_NOT_FOUND);
        check("and does not hand back a node", n == NULL);
    }

    /* ================================================================
     * 2. SubjectPublicKeyInfo, decoded from OpenSSL's bytes
     * ================================================================ */
    {
        asn1_node spki = NULL;
        rc = asn1_create_element(defs, "WebCrypto.SubjectPublicKeyInfo",
                                 &spki);
        check("create SubjectPublicKeyInfo", rc == ASN1_SUCCESS);

        int len = (int)sizeof(tasn1_ref_ec_spki);
        errdesc[0] = 0;
        rc = asn1_der_decoding2(&spki, tasn1_ref_ec_spki, &len,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("strict-DER decode of OpenSSL's P-256 SPKI",
              rc == ASN1_SUCCESS);
        check("and it consumed exactly the 91 bytes it was given",
              len == (int)sizeof(tasn1_ref_ec_spki));

        /*
         * The algorithm OID. libtasn1 hands an OBJECT IDENTIFIER back as
         * dotted text, having unpacked the base-128 encoding — so this
         * one string standing correct is the whole OID decoder working,
         * including the first byte that holds two arcs at once.
         */
        char oid[128];
        int oidlen = (int)sizeof(oid);
        rc = asn1_read_value(spki, "algorithm.algorithm", oid, &oidlen);
        check("read algorithm.algorithm", rc == ASN1_SUCCESS);
        check("it is id-ecPublicKey (1.2.840.10045.2.1)",
              rc == ASN1_SUCCESS && strcmp(oid, "1.2.840.10045.2.1") == 0);

        /*
         * The curve is in `parameters`, which the module declares ANY —
         * so what comes back is not a decoded OID but the raw DER of
         * one, tag and length included. That distinction is worth
         * checking rather than assuming: WebKit reads this field the
         * same way and would mis-parse a curve name if libtasn1 helpfully
         * unwrapped it.
         */
        unsigned char params[64];
        int plen = (int)sizeof(params);
        rc = asn1_read_value(spki, "algorithm.parameters", params, &plen);
        check("read algorithm.parameters (an ANY)", rc == ASN1_SUCCESS);
        check("it is raw DER: OBJECT IDENTIFIER, 8 bytes",
              rc == ASN1_SUCCESS && plen == 10 &&
              params[0] == 0x06 && params[1] == 0x08);
        check("and those bytes are prime256v1",
              rc == ASN1_SUCCESS && plen == 10 &&
              memcmp(params + 2, "\x2a\x86\x48\xce\x3d\x03\x01\x07", 8) == 0);

        /*
         * subjectPublicKey is a BIT STRING, and libtasn1 reports its
         * length in *bits* while writing whole bytes into the buffer.
         * That is upstream's documented behaviour and the sort of thing
         * a port cannot get wrong on its own — but it is also the check
         * that catches a length computed with the wrong integer width,
         * because 520 and 65 differ by a factor this test would notice.
         *
         * The call is made the way WebKit's elementData() makes it: no
         * buffer at all, which is a *query* and answers ASN1_MEM_ERROR
         * with the size in @len rather than ASN1_SUCCESS. Asserting
         * SUCCESS here would be asserting the opposite of the contract —
         * element.c returns MEM_ERROR whenever the buffer given cannot
         * hold the answer, and a buffer of zero bytes never can.
         */
        unsigned int etype = 0;
        int blen = 0;
        rc = asn1_read_value_type(spki, "subjectPublicKey", NULL, &blen,
                                  &etype);
        checkr("read_value_type with no buffer answers MEM_ERROR",
               rc == ASN1_MEM_ERROR, rc);
        check("its type is BIT STRING", etype == ASN1_ETYPE_BIT_STRING);
        check("its length is 520 bits", blen == 520);

        unsigned char point[128];
        int ptlen = (int)sizeof(point);
        rc = asn1_read_value(spki, "subjectPublicKey", point, &ptlen);
        check("read subjectPublicKey", rc == ASN1_SUCCESS);
        check("65 bytes of point came back",
              rc == ASN1_SUCCESS && ptlen == 520);
        check("uncompressed point marker 0x04", point[0] == 0x04);
        check("and the point is OpenSSL's, byte for byte",
              memcmp(point, tasn1_ref_ec_point,
                     sizeof(tasn1_ref_ec_point)) == 0);

        /*
         * Re-encode. The comparison is against the file's bytes, which
         * came from OpenSSL — not against a buffer this program produced
         * earlier — so encoder and decoder are not being allowed to
         * agree with each other.
         *
         * The length-only call first, with a NULL buffer, because that
         * is how WebKit's encodedData() sizes its Vector and it is a
         * separate code path from the one that writes.
         */
        int need = 0;
        rc = asn1_der_coding(spki, "", NULL, &need, errdesc);
        check("asn1_der_coding with no buffer answers MEM_ERROR",
              rc == ASN1_MEM_ERROR);
        check("and the size it asks for is OpenSSL's 91",
              need == (int)sizeof(tasn1_ref_ec_spki));

        unsigned char out[256];
        int outlen = (int)sizeof(out);
        rc = asn1_der_coding(spki, "", out, &outlen, errdesc);
        check("asn1_der_coding writes the structure", rc == ASN1_SUCCESS);
        check("into exactly 91 bytes",
              outlen == (int)sizeof(tasn1_ref_ec_spki));
        check("identical to OpenSSL's DER",
              outlen == (int)sizeof(tasn1_ref_ec_spki) &&
              memcmp(out, tasn1_ref_ec_spki, (size_t)outlen) == 0);

        asn1_delete_structure(&spki);
        check("asn1_delete_structure NULLs the handle", spki == NULL);
    }

    /* ================================================================
     * 3. the strict-DER flag, proved by making it change an answer
     * ================================================================
     *
     * The same 89 content bytes with an indefinite-length outer header:
     * legal BER, illegal DER. A flag that is passed and never alters an
     * outcome has not been tested, so this asserts both directions —
     * accepted without ASN1_DECODE_FLAG_STRICT_DER, refused with it.
     *
     * This is the flag WebKit passes on every decode it makes.
     */
    {
        asn1_node lax = NULL, strict = NULL;
        int len;

        rc = asn1_create_element(defs, "WebCrypto.SubjectPublicKeyInfo",
                                 &lax);
        check("create SPKI for the BER blob", rc == ASN1_SUCCESS);
        len = (int)sizeof(tasn1_ref_ec_spki_ber);
        errdesc[0] = 0;
        rc = asn1_der_decoding2(&lax, tasn1_ref_ec_spki_ber, &len, 0,
                                errdesc);
        check("indefinite length is accepted as BER", rc == ASN1_SUCCESS);

        /* and yields the same key, which is what makes the input a fair
         * test rather than merely malformed */
        if (rc == ASN1_SUCCESS) {
            unsigned char point[128];
            int ptlen = (int)sizeof(point);
            int r2 = asn1_read_value(lax, "subjectPublicKey", point,
                                     &ptlen);
            check("the BER blob holds the same point",
                  r2 == ASN1_SUCCESS &&
                  memcmp(point, tasn1_ref_ec_point,
                         sizeof(tasn1_ref_ec_point)) == 0);
        } else {
            check("the BER blob holds the same point", 0);
        }
        asn1_delete_structure(&lax);

        rc = asn1_create_element(defs, "WebCrypto.SubjectPublicKeyInfo",
                                 &strict);
        check("create SPKI again for the strict pass", rc == ASN1_SUCCESS);
        len = (int)sizeof(tasn1_ref_ec_spki_ber);
        errdesc[0] = 0;
        rc = asn1_der_decoding2(&strict, tasn1_ref_ec_spki_ber, &len,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("and the same bytes are refused under STRICT_DER",
              rc != ASN1_SUCCESS);
        asn1_delete_structure(&strict);

        /* Truncation must be caught too, and reported as its own error
         * rather than as a short read. */
        asn1_node trunc = NULL;
        rc = asn1_create_element(defs, "WebCrypto.SubjectPublicKeyInfo",
                                 &trunc);
        check("create SPKI for the truncated blob", rc == ASN1_SUCCESS);
        len = (int)sizeof(tasn1_ref_ec_spki) - 20;
        rc = asn1_der_decoding2(&trunc, tasn1_ref_ec_spki, &len,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("twenty bytes short is refused", rc != ASN1_SUCCESS);
        asn1_delete_structure(&trunc);
    }

    /* ================================================================
     * 4. PrivateKeyInfo — a structure with another one inside it
     * ================================================================ */
    {
        asn1_node p8 = NULL;
        rc = asn1_create_element(defs, "WebCrypto.PrivateKeyInfo", &p8);
        check("create PrivateKeyInfo", rc == ASN1_SUCCESS);

        int len = (int)sizeof(tasn1_ref_ec_pkcs8);
        errdesc[0] = 0;
        rc = asn1_der_decoding2(&p8, tasn1_ref_ec_pkcs8, &len,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("strict-DER decode of OpenSSL's PKCS#8", rc == ASN1_SUCCESS);

        /*
         * version is an INTEGER whose value is zero, and DER writes that
         * as one content byte 0x00 rather than as nothing. libtasn1
         * returns integers as raw content bytes, so the length is part
         * of the answer.
         */
        unsigned char ver[16];
        int vlen = (int)sizeof(ver);
        rc = asn1_read_value(p8, "version", ver, &vlen);
        check("read version", rc == ASN1_SUCCESS);
        check("version is a single 0x00 byte",
              rc == ASN1_SUCCESS && vlen == 1 && ver[0] == 0x00);

        char oid[128];
        int oidlen = (int)sizeof(oid);
        rc = asn1_read_value(p8, "privateKeyAlgorithm.algorithm", oid,
                             &oidlen);
        check("read privateKeyAlgorithm.algorithm", rc == ASN1_SUCCESS);
        check("it is id-ecPublicKey too",
              rc == ASN1_SUCCESS && strcmp(oid, "1.2.840.10045.2.1") == 0);

        /*
         * privateKey is an OCTET STRING holding a whole ECPrivateKey.
         * The outer decode must hand those bytes back untouched — this
         * is the check that a nested structure survives, and it is how
         * WebKit gets at the scalar.
         */
        unsigned char inner[256];
        int ilen = (int)sizeof(inner);
        rc = asn1_read_value(p8, "privateKey", inner, &ilen);
        check("read privateKey", rc == ASN1_SUCCESS);
        check("109 bytes of nested ECPrivateKey came back",
              rc == ASN1_SUCCESS && ilen == (int)sizeof(tasn1_ref_ec_inner));
        check("and they are OpenSSL's bytes unaltered",
              rc == ASN1_SUCCESS &&
              ilen == (int)sizeof(tasn1_ref_ec_inner) &&
              memcmp(inner, tasn1_ref_ec_inner, (size_t)ilen) == 0);

        /* The optional field OpenSSL did not write must read as absent
         * rather than as empty. */
        unsigned char attrs[64];
        int alen = (int)sizeof(attrs);
        rc = asn1_read_value(p8, "attributes", attrs, &alen);
        check("the absent OPTIONAL attributes is reported absent",
              rc == ASN1_ELEMENT_NOT_FOUND || rc == ASN1_VALUE_NOT_FOUND);

        /* Now decode the inner structure on its own, which is what
         * WebKit does next. */
        asn1_node ec = NULL;
        rc = asn1_create_element(defs, "WebCrypto.ECPrivateKey", &ec);
        check("create ECPrivateKey", rc == ASN1_SUCCESS);
        int elen = ilen;
        rc = asn1_der_decoding2(&ec, inner, &elen,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("the nested ECPrivateKey decodes", rc == ASN1_SUCCESS);

        unsigned char scalar[64];
        int slen = (int)sizeof(scalar);
        rc = asn1_read_value(ec, "privateKey", scalar, &slen);
        check("its privateKey is a 32-byte scalar",
              rc == ASN1_SUCCESS && slen == 32);

        /*
         * `parameters` is the [0] OPTIONAL, and it is *not here*.
         * RFC 5915 §3 says to leave it out when the structure is
         * carried inside a PrivateKeyInfo, because the
         * AlgorithmIdentifier one level up already names the curve, and
         * OpenSSL does exactly that — the inner bytes run
         * `30 6b | 02 01 01 | 04 20 ... | a1 44 ...`, version then
         * privateKey then [1], with no [0] in between.
         *
         * So this checks an absence, which is worth a check of its own:
         * an OPTIONAL field that was never encoded must come back as
         * missing rather than as empty, and those are different answers
         * to a caller deciding whether to trust a curve name.
         */
        unsigned char params[64];
        int plen = (int)sizeof(params);
        rc = asn1_read_value(ec, "parameters", params, &plen);
        checkr("the [0] parameters OpenSSL omitted reads as absent",
               rc == ASN1_ELEMENT_NOT_FOUND || rc == ASN1_VALUE_NOT_FOUND,
               rc);

        /*
         * `publicKey` is the [1] OPTIONAL, and it *is* here — so the
         * pair covers both halves of optional-field handling in one
         * structure.
         *
         * And it is the strongest check in this file. The point stored
         * inside the private key blob has to be the same point the
         * SubjectPublicKeyInfo in section 2 carried, because OpenSSL
         * generated both from one key — but the two arrive here down
         * completely different paths: one from a BIT STRING at the top
         * level of a 91-byte structure, the other from a BIT STRING
         * behind an explicit tag, inside an OCTET STRING, inside a
         * 138-byte structure. Agreeing is not something two wrong
         * decoders would do.
         */
        unsigned char pub[128];
        int publen = (int)sizeof(pub);
        rc = asn1_read_value(ec, "publicKey", pub, &publen);
        checkr("read the [1] publicKey that is present", rc == ASN1_SUCCESS,
               rc);
        check("it is 520 bits like the SPKI's", publen == 520);
        check("and it is the very same point, reached another way",
              rc == ASN1_SUCCESS && publen == 520 &&
              memcmp(pub, tasn1_ref_ec_point,
                     sizeof(tasn1_ref_ec_point)) == 0);

        asn1_delete_structure(&ec);
        asn1_delete_structure(&p8);
    }

    /* ================================================================
     * 5. RSAPublicKey — the long integers
     * ================================================================
     *
     * This is the section that would fail on a 32-bit
     * SIZEOF_UNSIGNED_LONG_INT. The modulus is 129 content bytes, the
     * outer SEQUENCE is 137, and both cross the 127-byte boundary where
     * DER switches from short-form to long-form lengths.
     */
    {
        asn1_node rsa = NULL;
        rc = asn1_create_element(defs, "WebCrypto.RSAPublicKey", &rsa);
        check("create RSAPublicKey", rc == ASN1_SUCCESS);

        int len = (int)sizeof(tasn1_ref_rsa_pub);
        errdesc[0] = 0;
        rc = asn1_der_decoding2(&rsa, tasn1_ref_rsa_pub, &len,
                                ASN1_DECODE_FLAG_STRICT_DER, errdesc);
        check("strict-DER decode of OpenSSL's 1024-bit RSAPublicKey",
              rc == ASN1_SUCCESS);

        unsigned char n[256];
        int nlen = (int)sizeof(n);
        rc = asn1_read_value(rsa, "modulus", n, &nlen);
        check("read modulus", rc == ASN1_SUCCESS);
        /*
         * 129, not 128. DER's INTEGER is signed and this modulus has its
         * top bit set, so the encoder prefixed a zero byte to keep it
         * positive. A decoder that stripped it, or an encoder that
         * omitted it, is the single most common ASN.1 bug there is —
         * hence an exact length rather than a range.
         */
        check("it is 129 bytes: 128 of modulus and a sign byte",
              rc == ASN1_SUCCESS && nlen == 129);
        check("the leading byte is the 0x00", n[0] == 0x00);
        check("and the modulus is OpenSSL's",
              rc == ASN1_SUCCESS && nlen == 129 &&
              memcmp(n, tasn1_ref_rsa_modulus, 129) == 0);

        unsigned char e[16];
        int elen = (int)sizeof(e);
        rc = asn1_read_value(rsa, "publicExponent", e, &elen);
        check("read publicExponent", rc == ASN1_SUCCESS);
        check("it is 65537 as 01 00 01",
              rc == ASN1_SUCCESS && elen == 3 &&
              e[0] == 0x01 && e[1] == 0x00 && e[2] == 0x01);

        int need = 0;
        rc = asn1_der_coding(rsa, "", NULL, &need, errdesc);
        check("sizing the re-encode asks for OpenSSL's 140",
              rc == ASN1_MEM_ERROR &&
              need == (int)sizeof(tasn1_ref_rsa_pub));

        unsigned char out[256];
        int outlen = (int)sizeof(out);
        rc = asn1_der_coding(rsa, "", out, &outlen, errdesc);
        check("re-encoding the RSA key succeeds", rc == ASN1_SUCCESS);
        check("and reproduces OpenSSL's 140 bytes exactly",
              rc == ASN1_SUCCESS &&
              outlen == (int)sizeof(tasn1_ref_rsa_pub) &&
              memcmp(out, tasn1_ref_rsa_pub, (size_t)outlen) == 0);

        asn1_delete_structure(&rsa);
    }

    /* ================================================================
     * 6. asn1_write_value — building a structure from nothing
     * ================================================================
     *
     * Sections 2 to 5 decoded and re-encoded, which exercises the
     * encoder only on values the decoder just put there. This builds an
     * RSAPublicKey out of two integers held in this file and encodes it
     * cold, so the encoder is on its own — and the answer is still
     * OpenSSL's bytes, because the two integers are the ones OpenSSL
     * wrote.
     */
    {
        asn1_node rsa = NULL;
        rc = asn1_create_element(defs, "WebCrypto.RSAPublicKey", &rsa);
        check("create an empty RSAPublicKey", rc == ASN1_SUCCESS);

        rc = asn1_write_value(rsa, "modulus", tasn1_ref_rsa_modulus,
                              (int)sizeof(tasn1_ref_rsa_modulus));
        check("write the modulus", rc == ASN1_SUCCESS);
        rc = asn1_write_value(rsa, "publicExponent", "\x01\x00\x01", 3);
        check("write the public exponent", rc == ASN1_SUCCESS);

        unsigned char out[256];
        int outlen = (int)sizeof(out);
        errdesc[0] = 0;
        rc = asn1_der_coding(rsa, "", out, &outlen, errdesc);
        check("encode the structure that was built by hand",
              rc == ASN1_SUCCESS);
        check("and it is byte-identical to OpenSSL's RSAPublicKey",
              rc == ASN1_SUCCESS &&
              outlen == (int)sizeof(tasn1_ref_rsa_pub) &&
              memcmp(out, tasn1_ref_rsa_pub, (size_t)outlen) == 0);

        /* A structure missing a mandatory field must refuse to encode
         * rather than emit something short. */
        asn1_node half = NULL;
        rc = asn1_create_element(defs, "WebCrypto.RSAPublicKey", &half);
        check("create a second empty RSAPublicKey", rc == ASN1_SUCCESS);
        rc = asn1_write_value(half, "modulus", tasn1_ref_rsa_modulus,
                              (int)sizeof(tasn1_ref_rsa_modulus));
        check("write only the modulus", rc == ASN1_SUCCESS);
        int hlen = (int)sizeof(out);
        errdesc[0] = 0;
        rc = asn1_der_coding(half, "", out, &hlen, errdesc);
        check("encoding without publicExponent is refused",
              rc == ASN1_VALUE_NOT_FOUND);
        check("and the error description names the missing field",
              strstr(errdesc, "publicExponent") != NULL);

        asn1_delete_structure(&half);
        asn1_delete_structure(&rsa);
    }

    /* ================================================================
     * 6b. a CHOICE, selected by name
     * ================================================================
     *
     * ECParameters is the module's only CHOICE, and the PKCS#8 blob in
     * section 4 does not carry one — OpenSSL omits it there — so it is
     * built here instead.
     *
     * A CHOICE is the one construct in this library where the *name* of
     * a branch is data: asn1_write_value takes the alternative as a
     * string and asn1_read_value hands the selected one back the same
     * way. Both directions go through the same name hash that
     * _asn1_hash_name computes with WORD_BIT, which is the macro this
     * port had to add to libc — so a wrong rotation width would surface
     * here as a branch that cannot be selected by the name it has.
     *
     * The expected encoding is not invented: it is the ten bytes
     * section 2 read out of OpenSSL's SPKI as the ANY-typed
     * `algorithm.parameters`, which is that same curve name as DER.
     */
    {
        static const unsigned char curve_der[] = {
            0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07
        };
        asn1_node ecp = NULL;
        rc = asn1_create_element(defs, "WebCrypto.ECParameters", &ecp);
        checkr("create ECParameters", rc == ASN1_SUCCESS, rc);

        rc = asn1_write_value(ecp, "", "namedCurve", 1);
        checkr("select the namedCurve branch by name", rc == ASN1_SUCCESS,
               rc);
        rc = asn1_write_value(ecp, "namedCurve", "1.2.840.10045.3.1.7", 1);
        checkr("write prime256v1 into it", rc == ASN1_SUCCESS, rc);

        char branch[64];
        int brlen = (int)sizeof(branch);
        rc = asn1_read_value(ecp, "", branch, &brlen);
        checkr("reading the CHOICE names the branch chosen",
               rc == ASN1_SUCCESS && strcmp(branch, "namedCurve") == 0, rc);

        unsigned char out[64];
        int outlen = (int)sizeof(out);
        errdesc[0] = 0;
        rc = asn1_der_coding(ecp, "", out, &outlen, errdesc);
        checkr("encoding a CHOICE emits the chosen alternative alone",
               rc == ASN1_SUCCESS, rc);
        check("which is the ten bytes OpenSSL wrote for prime256v1",
              rc == ASN1_SUCCESS && outlen == (int)sizeof(curve_der) &&
              memcmp(out, curve_der, sizeof(curve_der)) == 0);

        /* A branch the CHOICE does not have must be refused. */
        asn1_node bad = NULL;
        rc = asn1_create_element(defs, "WebCrypto.ECParameters", &bad);
        checkr("create a second ECParameters", rc == ASN1_SUCCESS, rc);
        rc = asn1_write_value(bad, "", "implicitCurve", 1);
        checkr("a branch that is not in the CHOICE is refused",
               rc != ASN1_SUCCESS, rc);
        asn1_delete_structure(&bad);

        asn1_delete_structure(&ecp);
    }

    /* ================================================================
     * 7. the error strings, and the version
     * ================================================================ */
    {
        const char *s = asn1_strerror(ASN1_SUCCESS);
        check("asn1_strerror answers for ASN1_SUCCESS", s != NULL);
        s = asn1_strerror(ASN1_ELEMENT_NOT_FOUND);
        check("and for ASN1_ELEMENT_NOT_FOUND", s != NULL && *s != 0);
        s = asn1_strerror(ASN1_DER_ERROR);
        check("and for ASN1_DER_ERROR", s != NULL && *s != 0);

        /*
         * The three calls that reach strverscmp — gnulib's, compiled
         * into libtasn1.a because this C library has no such function.
         * The middle one is the only check here that must answer *no*,
         * and it is the one that would catch a comparison that always
         * returns zero.
         */
        const char *v = asn1_check_version(NULL);
        check("asn1_check_version(NULL) returns the version",
              v != NULL && strcmp(v, ASN1_VERSION) == 0);
        check("a version from the future is refused",
              asn1_check_version("99.99.99") == NULL);
        check("an older version is accepted",
              asn1_check_version("4.0.0") != NULL);
        check("and its own version is accepted",
              asn1_check_version(ASN1_VERSION) != NULL);
    }

    asn1_delete_structure(&defs);
    check("the module tree frees", defs == NULL);

    printf("tasn1test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
