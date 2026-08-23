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

    /*
     * AES-GCM, against the test cases in McGrew and Viega's "The
     * Galois/Counter Mode of Operation" -- the same vectors NIST
     * adopted for SP 800-38D.
     *
     * Case 1 is the one worth having first: no plaintext and no
     * associated data, so it tests nothing but the tag construction.
     * An implementation with GHASH's bit order reversed still produces
     * a tag here, just not this one.
     */
    printf("\nAES-128-GCM (McGrew/Viega, NIST SP 800-38D)\n");
    {
        uint8_t key[16]={0}, iv[12]={0}, tag[16], ct[64];

        aes_gcm_encrypt(key,128,iv,12,0,0,0,0,ct,tag,16);
        hexcheck("case 1 tag (empty key, empty message)",tag,
                 "58e2fccefa7e3061367f1d57a4e7455a",16);

        uint8_t pt16[16]={0};
        aes_gcm_encrypt(key,128,iv,12,0,0,pt16,16,ct,tag,16);
        hexcheck("case 2 ciphertext",ct,"0388dace60b6a392f328c2b971b2fe78",16);
        hexcheck("case 2 tag",tag,"ab6e47d42cec13bdf53a67b21257bddf",16);
    }
    {
        const uint8_t key[16]={0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,
                               0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
        const uint8_t iv[12]={0xca,0xfe,0xba,0xbe,0xfa,0xce,
                              0xdb,0xad,0xde,0xca,0xf8,0x88};
        const uint8_t pt[64]={
            0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,
            0xaf,0xf5,0x26,0x9a,0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,
            0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,0x1c,0x3c,0x0c,0x95,
            0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
            0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39,
            0x1a,0xaf,0xd2,0x55};
        const uint8_t aad[20]={0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
                               0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
                               0xab,0xad,0xda,0xd2};
        uint8_t ct[64], tag[16], back[64];

        aes_gcm_encrypt(key,128,iv,12,0,0,pt,64,ct,tag,16);
        hexcheck("case 3 ciphertext (first 16)",ct,
                 "42831ec2217774244b7221b784d0d49c",16);
        hexcheck("case 3 tag",tag,"4d5c2af327cd64a62cf35abd2ba6fab4",16);

        /* Case 4 shortens the plaintext and adds associated data, which
         * is the combination SMB actually uses: a header in clear that
         * must still be covered by the tag. */
        aes_gcm_encrypt(key,128,iv,12,aad,20,pt,60,ct,tag,16);
        hexcheck("case 4 tag (with associated data)",tag,
                 "5bc94fbc3221a5db94fae95ae7121a47",16);

        checks++;
        if(aes_gcm_decrypt(key,128,iv,12,aad,20,ct,60,tag,16,back)==0 &&
           memcmp(back,pt,60)==0) printf("  ok   case 4 decrypts\n");
        else { fails++; printf("  FAIL case 4 decrypts\n"); }

        /* One bit of the associated data, flipped. The tag covers it,
         * so this must be refused -- a mode that only authenticates the
         * ciphertext would happily return the plaintext here. */
        uint8_t bad[20]; memcpy(bad,aad,20); bad[0]^=1;
        checks++;
        if(aes_gcm_decrypt(key,128,iv,12,bad,20,ct,60,tag,16,back)!=0)
            printf("  ok   a modified header is rejected\n");
        else { fails++; printf("  FAIL a modified header is rejected\n"); }

        uint8_t badtag[16]; memcpy(badtag,tag,16); badtag[15]^=0x80;
        checks++;
        if(aes_gcm_decrypt(key,128,iv,12,aad,20,ct,60,badtag,16,back)!=0)
            printf("  ok   a modified tag is rejected\n");
        else { fails++; printf("  FAIL a modified tag is rejected\n"); }
    }

    /*
     * AES-CCM, against RFC 3610's packet vectors.
     *
     * These use a 13-byte nonce and an 8-byte tag; SMB 3.0 uses 11 and
     * 16. Passing them proves the formatting is driven by the
     * parameters rather than hard-coded to the one case this system
     * happens to need, which is the failure that would only show up
     * against a real server.
     */
    printf("\nAES-128-CCM (RFC 3610)\n");
    {
        const uint8_t key[16]={0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
                               0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf};
        const uint8_t nonce[13]={0x00,0x00,0x00,0x03,0x02,0x01,0x00,
                                 0xa0,0xa1,0xa2,0xa3,0xa4,0xa5};
        const uint8_t aad[8]={0,1,2,3,4,5,6,7};
        uint8_t pt[23]; for(int i=0;i<23;i++) pt[i]=(uint8_t)(8+i);
        uint8_t ct[23], tag[8], back[23];

        aes_ccm_encrypt(key,128,nonce,13,aad,8,pt,23,ct,tag,8);
        hexcheck("packet vector #1 ciphertext",ct,
                 "588c979a61c663d2f066d0c2c0f98980"
                 "6d5f6b61dac384",23);
        hexcheck("packet vector #1 tag",tag,"17e8d12cfdf926e0",8);

        checks++;
        if(aes_ccm_decrypt(key,128,nonce,13,aad,8,ct,23,tag,8,back)==0 &&
           memcmp(back,pt,23)==0) printf("  ok   packet vector #1 decrypts\n");
        else { fails++; printf("  FAIL packet vector #1 decrypts\n"); }

        uint8_t badtag[8]; memcpy(badtag,tag,8); badtag[0]^=1;
        checks++;
        if(aes_ccm_decrypt(key,128,nonce,13,aad,8,ct,23,badtag,8,back)!=0)
            printf("  ok   a modified tag is rejected\n");
        else { fails++; printf("  FAIL a modified tag is rejected\n"); }
    }
    {
        /* Packet vector #2: one more byte of payload, which pushes it
         * over a block boundary and exercises the padding. */
        const uint8_t key[16]={0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
                               0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf};
        const uint8_t nonce[13]={0x00,0x00,0x00,0x04,0x03,0x02,0x01,
                                 0xa0,0xa1,0xa2,0xa3,0xa4,0xa5};
        const uint8_t aad[8]={0,1,2,3,4,5,6,7};
        uint8_t pt[24]; for(int i=0;i<24;i++) pt[i]=(uint8_t)(8+i);
        uint8_t ct[24], tag[8];
        aes_ccm_encrypt(key,128,nonce,13,aad,8,pt,24,ct,tag,8);
        hexcheck("packet vector #2 ciphertext",ct,
                 "72c91a36e135f8cf291ca894085c87e3"
                 "cc15c439c9e43a3b",24);
        hexcheck("packet vector #2 tag",tag,"a091d56e10400916",8);
    }
    {
        /* And the shape SMB 3.0 actually asks for: an 11-byte nonce and
         * a 16-byte tag. No published vector for this one, so what is
         * checked is that the parameters are accepted and round-trip --
         * the vectors above are what establish the algorithm is right. */
        const uint8_t key[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        uint8_t nonce[11]; for(int i=0;i<11;i++) nonce[i]=(uint8_t)(0x20+i);
        uint8_t aad[32];   for(int i=0;i<32;i++) aad[i]=(uint8_t)(0x40+i);
        uint8_t pt[70];    for(int i=0;i<70;i++) pt[i]=(uint8_t)(i*7+3);
        uint8_t ct[70], tag[16], back[70];
        checks++;
        if(aes_ccm_encrypt(key,128,nonce,11,aad,32,pt,70,ct,tag,16)==0 &&
           aes_ccm_decrypt(key,128,nonce,11,aad,32,ct,70,tag,16,back)==0 &&
           memcmp(back,pt,70)==0)
            printf("  ok   SMB 3.0 parameters (11-byte nonce, 16-byte tag)\n");
        else { fails++; printf("  FAIL SMB 3.0 parameters\n"); }
    }

    printf("\n%d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
