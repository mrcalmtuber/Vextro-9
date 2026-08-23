/*
 * tools/av_test.c — the automaton against the search it replaced.
 *
 * src/security/anti_virus.c swapped a brute-force scan (one full pass
 * over the buffer per signature) for an Aho-Corasick automaton (one
 * pass total, regardless of signature count). That is a rewrite of a
 * security check, so "the new one finds EICAR" is not evidence. The two
 * have to agree on inputs that *don't* match as much as on ones that
 * do, and on which signature they name when several match at once.
 *
 * Both implementations are still in the file — the direct search is
 * the fallback if the automaton's node pool ever overflows — so the
 * strongest available check is to run them side by side and require
 * identical answers. That is what the randomised section below does,
 * over buffers with patterns planted at every alignment, straddling
 * boundaries, and adjacent to their own near-misses.
 *
 * The near-miss cases matter most. A matcher built from a trie without
 * failure links passes every test in which a pattern appears cleanly
 * and fails the moment a false start precedes the real thing —
 * "X5O!X5O!P%@AP..." — because it has consumed the prefix and has no
 * way back. Those cases are called out individually below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The kernel side of the seam, supplied so the module compiles here.
 * Nothing under test calls the filesystem except policy_load/save,
 * which these let us drive without a disk. */
static char  host_file[4096];
static int   host_file_len = -1;   /* -1 = no such file */
static int   host_verbose  = 0;

void serial_puts(const char *s)   { if (host_verbose) fputs(s, stdout); }
void serial_putc(char c)          { if (host_verbose) fputc(c, stdout); }
void serial_put_dec(uint32_t v)   { if (host_verbose) printf("%u", v); }
void serial_put_hex32(uint32_t v) { if (host_verbose) printf("%08x", v); }

const void *fs_read_file(const char *filename, uint64_t *out_size) {
    (void)filename;
    if (host_file_len < 0) return 0;
    if (out_size) *out_size = (uint64_t)host_file_len;
    return host_file;
}

int fs_write_file(const char *path, const void *data, uint32_t len) {
    (void)path;
    if (len > sizeof(host_file)) return -1;
    memcpy(host_file, data, len);
    host_file_len = (int)len;
    return 0;
}

int fs_mkdir(const char *path) { (void)path; return 0; }

/* The module itself. Included rather than linked so the checks can
 * reach both matchers — the automaton and the direct search are file
 * statics, and comparing them is the whole point of this file. */
#include "security/anti_virus.c"

static int checks = 0;
static int fails  = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

static void eq_int(const char *what, int got, int want) {
    checks++;
    if (got == want) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s\n        got  %d\n        want %d\n", what, got, want);
}

static void eq_str(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want) == 0) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s\n        got  \"%s\"\n        want \"%s\"\n",
           what, got, want);
}

static const char *EICAR  = "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR";
static const char *MARKER = "VEXTRO-SCANNER-TEST-PATTERN";

/* ===== the automaton is built, and is a DFA ===== */

static void test_build(void) {
    printf("\nbuilding the automaton\n");

    av_init();
    ok("it builds", ac_ready == 1);
    ok("  without overflowing the node pool", ac_overflow == 0);

    /* One node per distinct pattern byte, plus the root. The two
     * signatures share no prefix, so it is the sum of their lengths. */
    const int expect = (int)strlen(EICAR) + (int)strlen(MARKER) + 1;
    eq_int("  with one state per pattern byte, plus the root",
           ac_count, expect);
    ok("  and fits int16_t indices", ac_count < 32767);

    /*
     * Every transition must be resolved. A -1 left anywhere means the
     * scan loop would index the table with a negative state, which is
     * not a crash on this structure -- it reads backwards into the
     * previous node and silently matches nothing.
     */
    int unresolved = 0;
    for (int n = 0; n < ac_count; n++)
        for (int c = 0; c < AC_ALPHABET; c++)
            if (ac_nodes[n].next[c] < 0 || ac_nodes[n].next[c] >= ac_count)
                unresolved++;
    eq_int("  every (state, byte) pair resolves to a real state",
           unresolved, 0);

    /* The root must be a fixed point for bytes no pattern starts with,
     * or the automaton drifts on ordinary data. */
    ok("  and the root stays put on an unrelated byte",
       ac_nodes[0].next[(uint8_t)'\x01'] == 0);
}

/* ===== the cases the old scanner was specified by ===== */

static void test_semantics(void) {
    uint8_t buf[512];
    printf("\nwhat a scan reports\n");

    memset(buf, 'a', sizeof(buf));
    eq_int("a buffer of nothing in particular is clean",
           scan_buffer(buf, sizeof(buf)), SCAN_CLEAN);
    eq_str("  and leaves no detail", scan_detail, "");

    eq_int("an empty buffer is clean", scan_buffer(buf, 0), SCAN_CLEAN);

    memset(buf, 'a', sizeof(buf));
    memcpy(buf + 100, EICAR, strlen(EICAR));
    eq_int("EICAR is a signature hit",
           scan_buffer(buf, sizeof(buf)), SCAN_SIGNATURE);
    eq_str("  named as EICAR", scan_detail, "EICAR-Test-File");

    memset(buf, 'a', sizeof(buf));
    memcpy(buf + 7, MARKER, strlen(MARKER));
    eq_int("the Vextro marker is a signature hit",
           scan_buffer(buf, sizeof(buf)), SCAN_SIGNATURE);
    eq_str("  named as the marker", scan_detail, "Vextro-Test-Marker");

    /*
     * The one place the two algorithms genuinely differ, and the reason
     * scan_signatures_ac keeps scanning after its first hit.
     *
     * The old scanner looped signatures on the outside, so it reported
     * whichever came first in the *table*. An automaton finds whichever
     * comes first in the *buffer*. Here the marker is at offset 0 and
     * EICAR is at 200, so a first-hit automaton would say "marker" and
     * quietly change what the user is told about a file.
     */
    memset(buf, 'a', sizeof(buf));
    memcpy(buf + 0,   MARKER, strlen(MARKER));
    memcpy(buf + 200, EICAR,  strlen(EICAR));
    eq_int("two signatures in one buffer is still one hit",
           scan_buffer(buf, sizeof(buf)), SCAN_SIGNATURE);
    eq_str("  reported by table order, not by position",
           scan_detail, "EICAR-Test-File");

    /* Boundaries. */
    memset(buf, 'a', sizeof(buf));
    memcpy(buf, EICAR, strlen(EICAR));
    eq_int("a signature at offset zero is found",
           scan_buffer(buf, sizeof(buf)), SCAN_SIGNATURE);

    memset(buf, 'a', sizeof(buf));
    memcpy(buf + sizeof(buf) - strlen(EICAR), EICAR, strlen(EICAR));
    eq_int("a signature at the very end is found",
           scan_buffer(buf, sizeof(buf)), SCAN_SIGNATURE);

    /* Truncated by one byte: not a match, and the length check is the
     * only thing standing between this and a read past the buffer. */
    memset(buf, 'a', sizeof(buf));
    memcpy(buf, EICAR, strlen(EICAR) - 1);
    eq_int("a signature one byte short is not a match",
           scan_buffer(buf, strlen(EICAR) - 1), SCAN_CLEAN);

    /* A buffer shorter than any pattern must not read past its end;
     * ASan on the host is what actually enforces this. */
    eq_int("a one-byte buffer is clean", scan_buffer((const uint8_t *)"X", 1),
           SCAN_CLEAN);
}

/* ===== failure links ===== */

static void test_failure_links(void) {
    uint8_t buf[512];
    printf("\nfalse starts, which is what the fail links are for\n");

    /* A prefix of EICAR, then EICAR. A trie without failure links has
     * consumed "X5O!" and cannot get back to the start of the second
     * copy without re-reading the input. */
    memset(buf, 'a', sizeof(buf));
    int n = 0;
    memcpy(buf + n, "X5O!", 4); n += 4;
    memcpy(buf + n, EICAR, strlen(EICAR)); n += (int)strlen(EICAR);
    eq_int("a false start before the real pattern still matches",
           scan_buffer(buf, (uint32_t)n), SCAN_SIGNATURE);
    eq_str("  and names it correctly", scan_detail, "EICAR-Test-File");

    /* The same, one byte of overlap: the pattern begins where the near
     * miss left off. */
    memset(buf, 'a', sizeof(buf));
    n = 0;
    memcpy(buf + n, "X5O!P%@AX", 9); n += 9;
    memcpy(buf + n, EICAR, strlen(EICAR)); n += (int)strlen(EICAR);
    eq_int("a longer false start still matches",
           scan_buffer(buf, (uint32_t)n), SCAN_SIGNATURE);

    /* Repeated first byte, which drives the automaton back to depth 1
     * rather than to the root. */
    memset(buf, 'X', 64);
    memcpy(buf + 64, EICAR, strlen(EICAR));
    eq_int("a run of the pattern's first byte does not swallow it",
           scan_buffer(buf, (uint32_t)(64 + strlen(EICAR))), SCAN_SIGNATURE);

    /* Interleaved near-misses of both patterns. */
    memset(buf, 'a', sizeof(buf));
    n = 0;
    memcpy(buf + n, "VEXTRO-SCANNER-TEST-PATTER", 26); n += 26;
    memcpy(buf + n, "X5O!P%@AP[4", 11); n += 11;
    memcpy(buf + n, MARKER, strlen(MARKER)); n += (int)strlen(MARKER);
    eq_int("two interleaved near-misses then a real one",
           scan_buffer(buf, (uint32_t)n), SCAN_SIGNATURE);
    eq_str("  names the one that actually occurred",
           scan_detail, "Vextro-Test-Marker");
}

/* ===== the structural check ===== */

static void test_structural(void) {
    uint8_t buf[256];
    printf("\nthe structural .vx check\n");

    memset(buf, 0, sizeof(buf));
    buf[8] = 0x00; buf[9] = 0x01; buf[10] = 0; buf[11] = 0;   /* 256 */
    eq_int("a declared size equal to the file is clean",
           scan_buffer(buf, 256), SCAN_CLEAN);

    buf[8] = 0x01; buf[9] = 0x01;                              /* 257 */
    eq_int("a declared size past the end is malformed",
           scan_buffer(buf, 256), SCAN_MALFORMED);
    eq_str("  and says so", scan_detail, "declared size exceeds the file");

    /* The upper guard: an absurd value is treated as "not a .vx" rather
     * than as a malformed one, which is the original behaviour. */
    buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0x10;       /* 0x10000000 */
    eq_int("an absurd declared size is not reported as malformed",
           scan_buffer(buf, 256), SCAN_CLEAN);

    /* Under sixteen bytes the check does not run at all. */
    memset(buf, 0xFF, 15);
    eq_int("a file under sixteen bytes skips the check",
           scan_buffer(buf, 15), SCAN_CLEAN);

    /* A signature takes precedence over a structural problem, because
     * the signature is the more specific answer. */
    memset(buf, 0, sizeof(buf));
    buf[8] = 0xFF; buf[9] = 0x00;                              /* 255 < 256 */
    memcpy(buf + 32, EICAR, strlen(EICAR));
    eq_int("a signature is reported even in a well-formed file",
           scan_buffer(buf, 256), SCAN_SIGNATURE);
}

/* ===== the two matchers, side by side ===== */

static uint32_t rng_state = 0x2545F491u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_equivalence(void) {
    printf("\nthe automaton and the direct search, on the same inputs\n");

    enum { BUFSZ = 1024, ROUNDS = 20000 };
    static uint8_t buf[BUFSZ];
    int mismatches = 0, hits = 0, misses = 0;

    for (int r = 0; r < ROUNDS; r++) {
        const uint32_t len = 1 + (rng() % BUFSZ);

        /* A small alphabet, so partial matches happen by accident far
         * more often than they would over all 256 byte values. Random
         * bytes almost never collide with a pattern; bytes drawn from
         * the patterns' own alphabet collide constantly, which is where
         * a fail-link bug lives. */
        static const char pool[] = "X5O!P%@A[4\\PZ)7C}$EICARVETNS-";
        for (uint32_t i = 0; i < len; i++)
            buf[i] = (uint8_t)pool[rng() % (sizeof(pool) - 1)];

        /* Plant a real pattern about a third of the time, at a random
         * offset and sometimes deliberately truncated by the end of the
         * buffer. */
        const uint32_t roll = rng() % 3;
        if (roll < 2) {
            const char *pat = (rng() & 1) ? EICAR : MARKER;
            const uint32_t plen = (uint32_t)strlen(pat);
            if (len > plen) {
                const uint32_t at = rng() % (len - plen + 1);
                memcpy(buf + at, pat, plen);
            } else {
                memcpy(buf, pat, len);   /* truncated on purpose */
            }
        }

        const int a = scan_signatures_ac(buf, len);
        const int d = scan_signatures_direct(buf, len);
        if (a != d) {
            if (mismatches < 3)
                printf("       round %d: automaton=%d direct=%d len=%u\n",
                       r, a, d, len);
            mismatches++;
        }
        if (d >= 0) hits++; else misses++;
    }

    eq_int("twenty thousand buffers, no disagreement", mismatches, 0);
    ok("  and the corpus actually contained matches", hits > 1000);
    ok("  and also contained non-matches", misses > 1000);
    printf("       (%d matched, %d did not)\n", hits, misses);
}

/* ===== the allowlist and the policy file ===== */

static void test_policy(void) {
    printf("\nthe allowlist and /etc/policy.cfg\n");

    allow_count = 0;
    allowlist_on = 0;
    ok("an empty allowlist that is off permits anything",
       allow_permits("anything.vx"));

    allowlist_on = 1;
    ok("  and permits nothing once it is on",
       !allow_permits("anything.vx"));

    ok("adding a name works", allow_add("hello.vx") == 1);
    ok("  and the name is then permitted", allow_permits("hello.vx"));
    ok("  while another still is not", !allow_permits("other.vx"));
    eq_int("  and the count is one", allow_count, 1);

    ok("adding the same name twice is idempotent", allow_add("hello.vx") == 1);
    eq_int("  and does not grow the list", allow_count, 1);

    /* The list is bounded, and the bound is reported rather than
     * silently overwriting the last entry. */
    allow_count = 0;
    for (int i = 0; i < ALLOW_MAX; i++) {
        char nm[ALLOW_NAME];
        snprintf(nm, sizeof(nm), "app%d.vx", i);
        allow_add(nm);
    }
    eq_int("the list fills to its bound", allow_count, ALLOW_MAX);
    ok("  and refuses the one after", allow_add("overflow.vx") == 0);
    eq_int("  without growing", allow_count, ALLOW_MAX);

    ok("removing a name works", allow_remove("app0.vx") == 1);
    eq_int("  and shrinks the list", allow_count, ALLOW_MAX - 1);
    ok("  and the name is no longer permitted", !allow_permits("app0.vx"));
    ok("removing an absent name reports it",
       allow_remove("never-here.vx") == 0);

    /* Round-trip through the policy file. */
    allow_count = 0;
    allowlist_on = 1;
    scanner_on   = 0;
    uac_level    = UAC_PARANOID;
    allow_add("one.vx");
    allow_add("two.vx");
    policy_save();

    allowlist_on = 0;
    scanner_on   = 1;
    uac_level    = UAC_NEVER;
    allow_count  = 0;
    policy_load();

    eq_int("the UAC level survives a save and load", uac_level, UAC_PARANOID);
    eq_int("  as does the allowlist switch", allowlist_on, 1);
    eq_int("  as does the scanner switch", scanner_on, 0);
    eq_int("  and both names come back", allow_count, 2);
    eq_str("  in order, first", allow_names[0], "one.vx");
    eq_str("  and second", allow_names[1], "two.vx");

    /* A missing policy file must leave the defaults alone rather than
     * zeroing them, which is what an unchecked read would do. */
    host_file_len = -1;
    uac_level = UAC_ADMIN_TASKS;
    allow_count = 3;
    policy_load();
    eq_int("a missing policy file changes nothing", uac_level, UAC_ADMIN_TASKS);
    eq_int("  and does not clear the list", allow_count, 3);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-v") == 0) host_verbose = 1;

    printf("Vextro anti-virus: Aho-Corasick, and the search it replaced\n");
    printf("===========================================================\n");

    test_build();
    test_semantics();
    test_failure_links();
    test_structural();
    test_equivalence();
    test_policy();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
