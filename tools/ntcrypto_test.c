/*
 * tools/ntcrypto_test.c — the old algorithms, against their own vectors.
 *
 * MD4, MD5, SHA-1, RC4 and the NTLM derivations in src/ntcrypto.h, each
 * checked against a value published by somebody else: RFC 1320, RFC
 * 1321, FIPS 180-4, RFC 2202, the RC4 vectors everyone quotes, and the
 * worked example in MS-NLMP section 4.2.4.
 *
 * Checking against published values rather than against itself is the
 * whole point. An implementation that is merely self-consistent
 * round-trips perfectly and authenticates to nothing -- and these
 * algorithms are unusually easy to get subtly wrong, because MD4 and
 * MD5 append their length little-endian and SHA-1 appends it
 * big-endian. Mixing the two produces a hash that is stable, plausible
 * and matches nobody.
 *
 * Runs on the host, where a failure is a line of output rather than a
 * silent authentication failure against a domain controller.
 */

#include <stdio.h>
#include <string.h>
#include "ntcrypto.h"
static int checks=0, fails=0;
static void hexcheck(const char *what, const uint8_t *got, const char *want, int n){
    char h[128]; for(int i=0;i<n;i++) sprintf(h+i*2,"%02x",got[i]); h[n*2]=0;
    checks++;
    if(strcmp(h,want)){ fails++; printf("  FAIL %s\n    got  %s\n    want %s\n",what,h,want); }
    else printf("  ok   %s\n",what);
}
int main(void){
    uint8_t o[20];
    printf("MD4 (RFC 1320)\n");
    md4((const uint8_t*)"",0,o);    hexcheck("md4(\"\")",o,"31d6cfe0d16ae931b73c59d7e0c089c0",16);
    md4((const uint8_t*)"abc",3,o); hexcheck("md4(\"abc\")",o,"a448017aaf21d8525fc10ae87aa6729d",16);
    md4((const uint8_t*)"message digest",14,o);
    hexcheck("md4(\"message digest\")",o,"d9130a8164549fe818874806e1c7014b",16);

    printf("\nMD5 (RFC 1321)\n");
    md5((const uint8_t*)"",0,o);    hexcheck("md5(\"\")",o,"d41d8cd98f00b204e9800998ecf8427e",16);
    md5((const uint8_t*)"abc",3,o); hexcheck("md5(\"abc\")",o,"900150983cd24fb0d6963f7d28e17f72",16);
    md5((const uint8_t*)"abcdefghijklmnopqrstuvwxyz",26,o);
    hexcheck("md5(a..z)",o,"c3fcd3d76192e4007dfb496cca67e13b",16);

    printf("\nSHA-1 (FIPS 180-4)\n");
    sha1((const uint8_t*)"abc",3,o);
    hexcheck("sha1(\"abc\")",o,"a9993e364706816aba3e25717850c26c9cd0d89d",20);
    sha1((const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56,o);
    hexcheck("sha1(56-byte)",o,"84983e441c3bd26ebaae4aa1f95129e5e54670f1",20);

    printf("\nHMAC-MD5 (RFC 2202)\n");
    { uint8_t k[16]; memset(k,0x0b,16);
      hmac_md5(k,16,(const uint8_t*)"Hi There",8,o);
      hexcheck("case 1",o,"9294727a3638bb1c13f48ef8158bfc9d",16); }
    hmac_md5((const uint8_t*)"Jefe",4,(const uint8_t*)"what do ya want for nothing?",28,o);
    hexcheck("case 2",o,"750c783e6ab0b503eaa86e310a5db738",16);

    printf("\nHMAC-SHA1 (RFC 2202)\n");
    { uint8_t k[20]; memset(k,0x0b,20);
      hmac_sha1(k,20,(const uint8_t*)"Hi There",8,o);
      hexcheck("case 1",o,"b617318655057264e28bc0b6fb378c8ef146be00",20); }
    hmac_sha1((const uint8_t*)"Jefe",4,(const uint8_t*)"what do ya want for nothing?",28,o);
    hexcheck("case 2",o,"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",20);
    { uint8_t k[20], d[50]; memset(k,0xaa,20); memset(d,0xdd,50);
      hmac_sha1(k,20,d,50,o);
      hexcheck("case 3",o,"125d7342b9ac11cd91a39af48aa17b4f63f175d3",20); }
    /* Cases 6 and 7 use an 80-byte key, which is longer than SHA-1's
     * 64-byte block and so must be hashed down first. That branch is
     * the one an HMAC written from the formula alone tends to omit. */
    /* strlen rather than a counted literal, and for a reason: writing
     * 53 where the string is 54 characters long produces a MAC that is
     * perfectly correct for the message actually passed, and reads as a
     * broken HMAC. That exact mistake happened here. */
    { uint8_t k[80]; memset(k,0xaa,80);
      const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
      const char *m7 = "Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data";
      hmac_sha1(k,80,(const uint8_t*)m6,(uint32_t)strlen(m6),o);
      hexcheck("case 6, key longer than the block",o,
               "aa4ae5e15272d00e95705637ce8a3b55ed402112",20);
      hmac_sha1(k,80,(const uint8_t*)m7,(uint32_t)strlen(m7),o);
      hexcheck("case 7",o,"e8e99d0f45237d786d6bbaa7965c7808bbff1a91",20); }

    printf("\nRC4 (published test vectors)\n");
    { rc4_ctx c; uint8_t out[9];
      rc4_init(&c,(const uint8_t*)"Key",3);
      rc4_apply(&c,(const uint8_t*)"Plaintext",out,9);
      hexcheck("Key/Plaintext",out,"bbf316e8d940af0ad3",9); }
    { rc4_ctx c; uint8_t out[14];
      rc4_init(&c,(const uint8_t*)"Secret",6);
      rc4_apply(&c,(const uint8_t*)"Attack at dawn",out,14);
      hexcheck("Secret/Attack at dawn",out,"45a01f645fc35b383552544b9bf5",14); }
    { rc4_ctx c; uint8_t out[5];
      rc4_init(&c,(const uint8_t*)"Wiki",4);
      rc4_apply(&c,(const uint8_t*)"pedia",out,5);
      hexcheck("Wiki/pedia",out,"1021bf0420",5); }

    printf("\nNTLM\n");
    ntlm_nt_hash("password",o);
    hexcheck("NT hash of \"password\"",o,"8846f7eaee8fb117ad06bdd830b7586c",16);
    ntlm_nt_hash("SecREt01",o);
    hexcheck("NT hash of \"SecREt01\"",o,"cd06ca7c7e10c99b1d33b7485a2ed808",16);
    { uint8_t nt[16], v2[16];
      ntlm_nt_hash("Password",nt);
      hexcheck("NT hash of \"Password\"",nt,"a4f49c406510bdcab6824ee7c30fd852",16);
      ntlm_v2_hash("User","Domain",nt,v2);
      hexcheck("NTLMv2 key (MS-NLMP 4.2.4)",v2,"0c868a403bfd7a93a3001ef22ef02e3f",16); }

    printf("\n%d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
