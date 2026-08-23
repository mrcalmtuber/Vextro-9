#ifndef VEXTRO_SECURITY_H
#define VEXTRO_SECURITY_H

#include "security/anti_virus.h"

/*
 * src/security.h — archives, encrypted containers, and the three policies
 * that decide whether a program is allowed to run.
 *
 * These are together because they share one honest limitation, and it is
 * better stated once than implied five times: none of this is a kernel
 * enforcement boundary. `.vx` applications execute with full privileges
 * in a shared address space, so what lives here is *policy* — the system
 * refusing to start something, and telling you why — not *isolation*. A
 * program that gets running can still do anything. Every panel and every
 * README sentence about this says so.
 *
 * What it does buy is real: an account cannot silently run software the
 * administrator has not allowed, a modified binary is noticed before it
 * starts, privileged actions can be made to ask, and a home directory can
 * be put into a file that is useless without its passphrase.
 */

/* ===== 1. USTAR WRITER =====
 *
 * coreutils could already list and extract ustar; it could not create
 * one. Backup needs that, and so does the vault, which is a tar with a
 * cipher over it.
 *
 * Written straight into a caller-provided buffer rather than to a file
 * handle, because both callers want the bytes in memory anyway — one to
 * encrypt them, the other to write them once.
 */

#define TARW_BLOCK 512

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    int      overflow;
} tarw_t;

static void tarw_init(tarw_t *t, uint8_t *buf, uint32_t cap) {
    t->buf = buf; t->cap = cap; t->len = 0; t->overflow = 0;
}

static void tarw_raw(tarw_t *t, const void *data, uint32_t n) {
    if (t->len + n > t->cap) { t->overflow = 1; return; }
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < n; i++) t->buf[t->len++] = p[i];
}

static void tarw_pad(tarw_t *t) {
    while (t->len % TARW_BLOCK) {
        if (t->len >= t->cap) { t->overflow = 1; return; }
        t->buf[t->len++] = 0;
    }
}

/* ustar stores sizes and modes as zero-padded octal, NUL terminated. */
static void tarw_octal(char *dst, int width, uint64_t v) {
    for (int i = width - 2; i >= 0; i--) {
        dst[i] = (char)('0' + (int)(v & 7));
        v >>= 3;
    }
    dst[width - 1] = '\0';
}

static void tarw_file(tarw_t *t, const char *name, const void *data,
                      uint32_t n, int is_dir) {
    uint8_t hdr[TARW_BLOCK];
    for (int i = 0; i < TARW_BLOCK; i++) hdr[i] = 0;

    char *h = (char *)hdr;
    int k = 0;
    /* A leading slash is stripped: an archive of absolute paths extracts
     * over the top of the running system, which is never what a backup
     * is for. */
    const char *nm = (name[0] == '/') ? name + 1 : name;
    while (nm[k] && k < 99) { h[k] = nm[k]; k++; }

    tarw_octal(h + 100, 8, is_dir ? 0755u : 0644u);   /* mode  */
    tarw_octal(h + 108, 8, 0);                        /* uid   */
    tarw_octal(h + 116, 8, 0);                        /* gid   */
    tarw_octal(h + 124, 12, is_dir ? 0 : n);          /* size  */
    tarw_octal(h + 136, 12, 0);                       /* mtime */
    h[156] = is_dir ? '5' : '0';                      /* typeflag */
    h[257] = 'u'; h[258] = 's'; h[259] = 't';
    h[260] = 'a'; h[261] = 'r'; h[262] = '\0';
    h[263] = '0'; h[264] = '0';

    /* The checksum is computed with its own field read as spaces, then
     * written back over it — the format's one piece of self-reference. */
    for (int i = 0; i < 8; i++) h[148 + i] = ' ';
    uint32_t sum = 0;
    for (int i = 0; i < TARW_BLOCK; i++) sum += hdr[i];
    tarw_octal(h + 148, 7, sum);
    h[154] = ' ';

    tarw_raw(t, hdr, TARW_BLOCK);
    if (!is_dir && n) { tarw_raw(t, data, n); tarw_pad(t); }
}

static void tarw_end(tarw_t *t) {
    uint8_t zero[TARW_BLOCK];
    for (int i = 0; i < TARW_BLOCK; i++) zero[i] = 0;
    tarw_raw(t, zero, TARW_BLOCK);      /* two empty blocks end an archive */
    tarw_raw(t, zero, TARW_BLOCK);
}

/* ===== 2. THE VAULT =====
 *
 * A directory sealed into one file: ustar, then ChaCha20 over the whole
 * archive, behind a header that carries the salt and nonce in clear —
 * they are not secrets — and a verifier that is not the key.
 *
 * The verifier exists because a stream cipher has no idea whether it was
 * given the right key: decrypting with the wrong passphrase produces
 * plausible-looking garbage rather than an error, and a "restore" that
 * silently writes noise over a home directory would be worse than no
 * backup at all. It is a second, independent hash of the key, so having
 * it does not give an attacker the key any faster than guessing does.
 */

/*
 * Declared, not included: xorshift32 lives in login.h, and the two trees
 * pull that in at different points relative to this header. A forward
 * declaration costs nothing and removes the ordering dependency.
 */
static uint32_t xorshift32(void);

#define VAULT_MAGIC0 'V'
#define VAULT_MAGIC1 'X'
#define VAULT_MAGIC2 'V'
#define VAULT_MAGIC3 '1'
#define VAULT_HDR    64

typedef struct {
    uint8_t magic[4];
    uint8_t salt[16];
    uint8_t nonce[12];
    uint8_t verifier[16];
    uint8_t plain_len[8];        /* little-endian, for a sanity check */
    uint8_t reserved[8];
} __attribute__((packed)) vault_hdr_t;

/* A second hash of the key, domain-separated so it cannot be confused
 * with the key itself. */
static void vault_verifier(const uint8_t key[32], uint8_t out[16]) {
    uint8_t tmp[32 + 8];
    for (int i = 0; i < 32; i++) tmp[i] = key[i];
    const char *tag = "vxverify";
    for (int i = 0; i < 8; i++) tmp[32 + i] = (uint8_t)tag[i];
    uint8_t d[32];
    sha256(tmp, 40, d);
    for (int i = 0; i < 16; i++) out[i] = d[i];
}

/* ===== 3. POLICY, THE SCANNER, AND POLICY PERSISTENCE =====
 *
 * Moved to src/security/anti_virus.c, which is compiled as its own
 * object rather than included here. What went is everything consulted
 * between "the user launched a program" and "the program started": the
 * allowlist, the signature scanner and the UAC level. Its interface is
 * src/security/anti_virus.h, included above.
 *
 * What stayed is storage — the tar writer above and the vault below —
 * because both are about putting bytes in a container, and neither is
 * on the launch path.
 *
 * The scanner also stopped being a loop per signature on the way out;
 * it is an Aho-Corasick automaton now, and the reason is written down
 * where it lives.
 */
/* ===== 6. SEALING A DIRECTORY =====
 *
 * The walk is breadth-first with a hard budget, the same shape as the
 * start menu's search, and for the same reason: this runs inside a frame
 * and must not be able to run away.
 *
 * fs_list's callback carries no user pointer, so the state is
 * file-scope. That is also why the callback only queues directories and
 * the loop below drains the queue -- it cannot recurse into itself.
 */

#define VW_QUEUE_MAX 24
#define VW_PATH      160
#define VW_FILES_MAX 96

static char vw_queue[VW_QUEUE_MAX][VW_PATH];
static int  vw_queue_n, vw_queue_head;
static char vw_cur[VW_PATH];
static char vw_files[VW_FILES_MAX][VW_PATH];
static int  vw_files_n;

static void vw_join(char *out, int cap, const char *dir, const char *name) {
    str_copy(out, dir, cap);
    if (!(dir[0] == '/' && dir[1] == '\0')) str_append(out, "/", cap);
    str_append(out, name, cap);
}

static void vw_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (name[0] == '.') return;
    char full[VW_PATH];
    vw_join(full, VW_PATH, vw_cur, name);
    if (is_dir) {
        if (vw_queue_n < VW_QUEUE_MAX) str_copy(vw_queue[vw_queue_n++], full, VW_PATH);
    } else if (vw_files_n < VW_FILES_MAX) {
        str_copy(vw_files[vw_files_n++], full, VW_PATH);
    }
}

/* Collect every file under `root`. Returns how many were found. */
static int vw_collect(const char *root) {
    vw_queue_n = vw_queue_head = 0;
    vw_files_n = 0;
    str_copy(vw_queue[vw_queue_n++], root, VW_PATH);
    while (vw_queue_head < vw_queue_n && vw_files_n < VW_FILES_MAX) {
        str_copy(vw_cur, vw_queue[vw_queue_head++], VW_PATH);
        fs_list(vw_cur, vw_cb);
    }
    return vw_files_n;
}

/*
 * The archive staging buffer. Static because there is no allocator, and
 * sized to what a home directory on a machine like this plausibly holds;
 * anything larger is refused with a message rather than truncated, since
 * a backup that silently stops halfway is the worst possible outcome.
 */
#define VAULT_MAX_BYTES (6u << 20)
static uint8_t vault_buf[VAULT_MAX_BYTES];

/*
 * Seal a directory into one encrypted file.
 * Returns 0, or a message describing why not.
 */
static char vault_err[160];

static const char *vault_seal(const char *dir, const char *outpath,
                              const char *passphrase, uint32_t *out_files) {
    const int nf = vw_collect(dir);
    if (nf == 0) return "nothing to archive there";

    tarw_t t;
    tarw_init(&t, vault_buf + VAULT_HDR, VAULT_MAX_BYTES - VAULT_HDR);

    for (int i = 0; i < nf; i++) {
        uint64_t n = 0;
        const void *d = fs_read_file(vw_files[i], &n);
        if (!d) continue;
        tarw_file(&t, vw_files[i], d, (uint32_t)n, 0);
        if (t.overflow) {
            /* Naming the file is the difference between a message that
             * can be acted on and one that can only be resented. */
            str_copy(vault_err, "does not fit in the archive buffer: ",
                     sizeof(vault_err));
            str_append(vault_err, vw_files[i], sizeof(vault_err));
            return vault_err;
        }
    }
    tarw_end(&t);
    if (t.overflow) return "too much data for the archive buffer";

    vault_hdr_t h;
    h.magic[0] = VAULT_MAGIC0; h.magic[1] = VAULT_MAGIC1;
    h.magic[2] = VAULT_MAGIC2; h.magic[3] = VAULT_MAGIC3;

    /* Salt and nonce from the cycle counter mixed through the same
     * generator the login screen uses. Not a hardware RNG -- and the
     * README says so -- but distinct per container, which is what the
     * salt is actually for. */
    for (int i = 0; i < 16; i++) h.salt[i]  = (uint8_t)(xorshift32() >> 17);
    for (int i = 0; i < 12; i++) h.nonce[i] = (uint8_t)(xorshift32() >> 19);

    uint8_t key[32];
    cc20_derive_key(passphrase, h.salt, key);
    vault_verifier(key, h.verifier);

    for (int i = 0; i < 8; i++)
        h.plain_len[i] = (uint8_t)((uint64_t)t.len >> (i * 8));
    for (int i = 0; i < 8; i++) h.reserved[i] = 0;

    cc20_xor(key, h.nonce, 0, vault_buf + VAULT_HDR, t.len);

    const uint8_t *hp = (const uint8_t *)&h;
    for (int i = 0; i < (int)sizeof(vault_hdr_t); i++) vault_buf[i] = hp[i];
    for (int i = (int)sizeof(vault_hdr_t); i < VAULT_HDR; i++) vault_buf[i] = 0;

    if (fs_write_file(outpath, vault_buf, t.len + VAULT_HDR) != 0)
        return fs_errstr;
    if (out_files) *out_files = (uint32_t)nf;
    return 0;
}

/* Open a container and write its contents back out under `dir`. */
static const char *vault_unseal(const char *inpath, const char *dir,
                                const char *passphrase, uint32_t *out_files) {
    uint64_t n = 0;
    const void *d = fs_read_file(inpath, &n);
    if (!d) return "cannot read that container";
    if (n <= VAULT_HDR) return "that file is too small to be a container";
    if (n - VAULT_HDR > VAULT_MAX_BYTES - VAULT_HDR)
        return "container is larger than the buffer";

    const uint8_t *src = (const uint8_t *)d;
    vault_hdr_t h;
    uint8_t *hp = (uint8_t *)&h;
    for (int i = 0; i < (int)sizeof(vault_hdr_t); i++) hp[i] = src[i];

    if (h.magic[0] != VAULT_MAGIC0 || h.magic[1] != VAULT_MAGIC1 ||
        h.magic[2] != VAULT_MAGIC2 || h.magic[3] != VAULT_MAGIC3)
        return "not a Vextro container";

    uint8_t key[32], want[16];
    cc20_derive_key(passphrase, h.salt, key);
    vault_verifier(key, want);
    /*
     * Checked before anything is written. A stream cipher cannot tell a
     * wrong key from a right one -- it decrypts to garbage either way --
     * so without this a mistyped passphrase would cheerfully write noise
     * over a home directory and call it a restore.
     */
    if (!cc20_equal(want, h.verifier, 16)) return "wrong passphrase";

    const uint32_t body = (uint32_t)(n - VAULT_HDR);
    for (uint32_t i = 0; i < body; i++) vault_buf[i] = src[VAULT_HDR + i];
    cc20_xor(key, h.nonce, 0, vault_buf, body);

    /* Walk the ustar and write each member out. */
    uint32_t off = 0, written = 0;
    fs_mkdir(dir);
    while (off + TARW_BLOCK <= body) {
        const char *hdr = (const char *)(vault_buf + off);
        if (hdr[0] == '\0') break;
        uint64_t sz = octal_parse(hdr + 124, 12);
        const char *nm = hdr;

        if (hdr[156] == '0' || hdr[156] == '\0') {
            char dst[VW_PATH];
            str_copy(dst, dir, sizeof(dst));
            if (!(dir[0] == '/' && dir[1] == '\0')) str_append(dst, "/", sizeof(dst));
            /* Members are stored with their leading slash stripped, so a
             * container can never write outside where it is opened. */
            const char *base = nm;
            for (int i = 0; nm[i] && i < 99; i++) if (nm[i] == '/') base = nm + i + 1;
            str_append(dst, base, sizeof(dst));
            if (fs_write_file(dst, vault_buf + off + TARW_BLOCK, (uint32_t)sz) == 0)
                written++;
        }
        off += TARW_BLOCK + (uint32_t)((sz + TARW_BLOCK - 1) / TARW_BLOCK) * TARW_BLOCK;
    }
    if (out_files) *out_files = written;
    return 0;
}

#endif /* VEXTRO_SECURITY_H */
