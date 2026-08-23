/*
 * src/security/anti_virus.c — deciding whether a program may run.
 *
 * Three policies, one file, checked between the click and the process:
 * the allowlist, the signature scanner, and the UAC level. They were
 * sections 3 to 5 of src/security.h, which also held a tar writer and
 * an encrypted container; those are storage and stayed behind.
 *
 * ---- why the scanner is an automaton ----
 *
 * The scanner this replaces searched the buffer once per signature:
 *
 *     for each signature s:
 *         for each offset i in the file:
 *             compare
 *
 * which is O(file x signatures x pattern) and, more to the point, reads
 * the whole file again for every signature in the table. Two signatures
 * made that invisible. It is also the shape of code that quietly
 * discourages adding a third, because the cost of the table is paid on
 * every program launch, and a scanner nobody adds signatures to is a
 * scanner that stops being worth having.
 *
 * What is here instead is Aho-Corasick: the signatures are compiled
 * once into a deterministic automaton, and matching is one table lookup
 * per input byte, *independent of how many signatures there are*. A
 * hundred signatures cost exactly what two cost. The build happens once
 * per boot and takes microseconds.
 *
 * The idea is a trie of the patterns plus a "failure" link on every
 * node, pointing at the longest proper suffix of the text matched so
 * far that is also a prefix of some pattern. That link is what makes
 * backtracking unnecessary: on a mismatch the automaton already knows
 * the longest partial match still alive, so the input pointer only ever
 * moves forward. Following fail links at match time would still be
 * correct but branchy, so the construction below goes further and
 * resolves every (state, byte) pair into a direct transition -- the
 * automaton is a DFA by the time it is used, and the scan loop has no
 * conditional at all except the one that notices a hit.
 *
 * ---- what is preserved exactly ----
 *
 * This is a replacement for a security check, so the two must agree on
 * every input, not merely on the ones that match:
 *
 *   - the return codes and their meanings
 *   - scan_detail, including *which* signature is named when a buffer
 *     matches more than one. The old loop was signature-major, so it
 *     reported the first match in *table* order; the automaton finds
 *     the first match in *buffer* order. Those differ, so the scan
 *     below collects the lowest table index across the whole pass
 *     rather than stopping at the first hit.
 *   - the structural `.vx` check, byte for byte, including the fact
 *     that it runs only when no signature matched.
 *
 * tools/av_test.c checks the automaton against the original brute-force
 * search over random buffers and planted patterns, which is the only
 * evidence worth having that a rewrite of a matcher matches.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel_shared.h"
#include "security/anti_virus.h"

/* ===== 1. POLICY =====
 *
 * Three settings, one file, read at login and enforced in the loader.
 *
 * /etc/policy.cfg, one key per line, because a binary format for six
 * numbers would be harder to repair from a terminal than it is to parse.
 */

const char *const uac_level_names[UAC_LEVELS] = {
    "Never notify",
    "Administrator tasks",
    "Installing software",
    "Every program",
};

int uac_level      = UAC_ADMIN_TASKS;
int allowlist_on   = 0;      /* only listed programs may run */
int scanner_on     = 1;      /* check binaries before running them */

char allow_names[ALLOW_MAX][ALLOW_NAME];
int  allow_count = 0;

int allow_permits(const char *name) {
    if (!allowlist_on) return 1;
    for (int i = 0; i < allow_count; i++)
        if (str_eq(allow_names[i], name)) return 1;
    return 0;
}

int allow_add(const char *name) {
    if (allow_count >= ALLOW_MAX) return 0;
    for (int i = 0; i < allow_count; i++)
        if (str_eq(allow_names[i], name)) return 1;
    str_copy(allow_names[allow_count++], name, ALLOW_NAME);
    return 1;
}

int allow_remove(const char *name) {
    for (int i = 0; i < allow_count; i++)
        if (str_eq(allow_names[i], name)) {
            for (int k = i; k + 1 < allow_count; k++)
                str_copy(allow_names[k], allow_names[k + 1], ALLOW_NAME);
            allow_count--;
            return 1;
        }
    return 0;
}

/* ===== 2. THE SIGNATURES =====
 *
 * Signatures plus a small number of structural checks over a `.vx` image
 * before it is allowed to start.
 *
 * The signature list is deliberately tiny and the heuristics deliberately
 * few, because the honest description of this is "it catches what it
 * knows about", and a long list would imply otherwise. What it is
 * genuinely good at is the case that actually happens on a machine like
 * this one: a binary that has been altered since it was installed.
 *
 * The list being short is now a property of the list rather than of the
 * matcher. Adding to it costs nothing at scan time.
 */

typedef struct {
    const char *name;
    const char *bytes;     /* NUL-terminated pattern */
} scan_sig_t;

static const scan_sig_t scan_sigs[] = {
    /* The EICAR standard anti-malware test string: not malware, and
     * designed precisely so a scanner can be shown to work without
     * anyone having to keep a real sample on the disk. */
    { "EICAR-Test-File", "X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR" },
    { "Vextro-Test-Marker", "VEXTRO-SCANNER-TEST-PATTERN" },
    { 0, 0 }
};

char scan_detail[64];

/* ===== 3. THE AUTOMATON =====
 *
 * Node zero is the root, and is also the "no match in progress" state.
 *
 * `next` is a full 256-way transition table rather than a sparse map.
 * That is the deliberate trade: it costs half a kilobyte per node and
 * buys a scan loop that is one indexed load per byte with no search and
 * no branch. The pool below is sized for signatures totalling roughly
 * two hundred bytes, which is many times the current table.
 *
 * int16_t indices, so the whole structure stays small enough to sit in
 * bss without thought; AC_MAX_NODES is checked against that range at
 * build time.
 */

#define AC_MAX_NODES 256
#define AC_ALPHABET  256

typedef struct {
    int16_t next[AC_ALPHABET];  /* resolved transition, never negative
                                 * once the automaton is built         */
    int16_t fail;               /* longest proper suffix that is also
                                 * a prefix of some pattern            */
    int16_t match;              /* lowest signature index ending here,
                                 * or -1                               */
} ac_node_t;

static ac_node_t ac_nodes[AC_MAX_NODES];
static int  ac_count = 0;
static int  ac_ready = 0;       /* 1 = automaton usable                */
static int  ac_overflow = 0;    /* 1 = patterns did not fit the pool   */

/* A queue for the breadth-first pass that computes the fail links.
 * Bounded by the node count, so it is sized to match and never checked
 * for overflow at run time -- it cannot overflow. */
static int16_t ac_queue[AC_MAX_NODES];

/*
 * Build the trie, then the fail links, then resolve the transitions.
 *
 * Returns 1 on success, 0 if the patterns did not fit the pool. The
 * failure is not fatal: scan_buffer falls back to the direct search,
 * which is slower but finds exactly the same things. A scanner that
 * silently stopped checking some of its signatures because a table
 * filled up would be worse than a slow one.
 */
static int ac_build(void) {
    /* --- the root --- */
    ac_count = 1;
    for (int c = 0; c < AC_ALPHABET; c++) ac_nodes[0].next[c] = -1;
    ac_nodes[0].fail  = 0;
    ac_nodes[0].match = -1;

    /* --- insert each pattern --- */
    for (int s = 0; scan_sigs[s].name; s++) {
        const char *pat = scan_sigs[s].bytes;
        int node = 0;
        for (int i = 0; pat[i]; i++) {
            const uint8_t c = (uint8_t)pat[i];
            if (ac_nodes[node].next[c] < 0) {
                if (ac_count >= AC_MAX_NODES) return 0;
                const int n = ac_count++;
                for (int k = 0; k < AC_ALPHABET; k++) ac_nodes[n].next[k] = -1;
                ac_nodes[n].fail  = 0;
                ac_nodes[n].match = -1;
                ac_nodes[node].next[c] = (int16_t)n;
            }
            node = ac_nodes[node].next[c];
        }
        /* An empty pattern would mark the root as accepting and match
         * every buffer, so it is refused rather than honoured. */
        if (node == 0) continue;
        /* Lowest table index wins, which is what preserves the old
         * scanner's report order when two signatures end at the same
         * node. */
        if (ac_nodes[node].match < 0 || ac_nodes[node].match > s)
            ac_nodes[node].match = (int16_t)s;
    }

    /*
     * --- fail links, breadth first ---
     *
     * The root's children fail to the root: a mismatch one byte in has
     * no shorter partial match to fall back to. Every deeper node fails
     * to whatever the node its parent fails to does with the same byte,
     * which is exactly the longest suffix still alive. Because the pass
     * is breadth-first, that value is always already resolved.
     */
    int head = 0, tail = 0;
    for (int c = 0; c < AC_ALPHABET; c++) {
        const int16_t v = ac_nodes[0].next[c];
        if (v < 0) {
            ac_nodes[0].next[c] = 0;     /* no edge: stay at the root */
        } else {
            ac_nodes[v].fail = 0;
            ac_queue[tail++] = v;
        }
    }

    while (head < tail) {
        const int u = ac_queue[head++];

        /* A node inherits the matches of the node it fails to. Without
         * this a pattern that is a proper suffix of another is missed
         * whenever the longer one is in progress -- "her" inside
         * "there". Keeping the lower index preserves table order. */
        const int16_t fmatch = ac_nodes[ac_nodes[u].fail].match;
        if (fmatch >= 0 &&
            (ac_nodes[u].match < 0 || ac_nodes[u].match > fmatch))
            ac_nodes[u].match = fmatch;

        for (int c = 0; c < AC_ALPHABET; c++) {
            const int16_t v = ac_nodes[u].next[c];
            if (v < 0) {
                /* No edge: this is where the automaton becomes a DFA.
                 * Rather than leaving the scan loop to walk fail links,
                 * the transition is resolved to whatever the failure
                 * state does with this byte -- already computed. */
                ac_nodes[u].next[c] = ac_nodes[ac_nodes[u].fail].next[c];
            } else {
                ac_nodes[v].fail = ac_nodes[ac_nodes[u].fail].next[c];
                ac_queue[tail++] = v;
            }
        }
    }
    return 1;
}

void av_init(void) {
    if (ac_ready || ac_overflow) return;
    if (ac_build()) {
        ac_ready = 1;
        serial_puts("[av] signature automaton: ");
        serial_put_dec((uint32_t)ac_count);
        serial_puts(" states\n");
    } else {
        ac_overflow = 1;
        serial_puts("[av] signature automaton did not fit; "
                    "falling back to direct search\n");
    }
}

/*
 * The fallback, and the reference implementation.
 *
 * This is the original scanner, kept rather than deleted for two
 * reasons: it is what runs if the automaton pool ever overflows, and it
 * is what tools/av_test.c compares the automaton against. A matcher
 * rewrite whose only evidence is "the new one passes" is a matcher
 * rewrite with no evidence at all.
 */
static int scan_signatures_direct(const uint8_t *data, uint32_t len) {
    for (int s = 0; scan_sigs[s].name; s++) {
        const char *pat = scan_sigs[s].bytes;
        int plen = 0;
        while (pat[plen]) plen++;
        if ((int)len < plen) continue;
        for (uint32_t i = 0; i + (uint32_t)plen <= len; i++) {
            int k = 0;
            while (k < plen && data[i + k] == (uint8_t)pat[k]) k++;
            if (k == plen) return s;
        }
    }
    return -1;
}

/*
 * One pass, and the lowest signature index found anywhere in it.
 *
 * Not the first hit: the old scanner reported by table order, and a
 * buffer carrying two signatures must still name the same one it always
 * did. The pass stops early only when index 0 matches, since nothing
 * can beat it.
 */
static int scan_signatures_ac(const uint8_t *data, uint32_t len) {
    int best = -1;
    int state = 0;
    for (uint32_t i = 0; i < len; i++) {
        state = ac_nodes[state].next[data[i]];
        const int m = ac_nodes[state].match;
        if (m >= 0 && (best < 0 || m < best)) {
            best = m;
            if (best == 0) break;
        }
    }
    return best;
}

int scan_buffer(const uint8_t *data, uint32_t len) {
    scan_detail[0] = '\0';

    if (!ac_ready && !ac_overflow) av_init();

    const int hit = ac_ready ? scan_signatures_ac(data, len)
                             : scan_signatures_direct(data, len);
    if (hit >= 0) {
        str_copy(scan_detail, scan_sigs[hit].name, sizeof(scan_detail));
        return SCAN_SIGNATURE;
    }

    /* Structural: a .vx that claims a section past its own end is either
     * truncated or tampered with, and either way must not be executed. */
    if (len >= 16) {
        const uint32_t claimed = (uint32_t)data[8]
                               | ((uint32_t)data[9] << 8)
                               | ((uint32_t)data[10] << 16)
                               | ((uint32_t)data[11] << 24);
        if (claimed > len && claimed < 0x10000000u) {
            str_copy(scan_detail, "declared size exceeds the file",
                     sizeof(scan_detail));
            return SCAN_MALFORMED;
        }
    }
    return SCAN_CLEAN;
}

/* ===== 4. POLICY PERSISTENCE ===== */

void policy_save(void) {
    char out[512];
    char nb[12];
    str_copy(out, "uac=", sizeof(out));
    uint_to_str((uint32_t)uac_level, nb); str_append(out, nb, sizeof(out));
    str_append(out, "\nallowlist=", sizeof(out));
    str_append(out, allowlist_on ? "1" : "0", sizeof(out));
    str_append(out, "\nscanner=", sizeof(out));
    str_append(out, scanner_on ? "1" : "0", sizeof(out));
    str_append(out, "\n", sizeof(out));
    for (int i = 0; i < allow_count; i++) {
        str_append(out, "allow=", sizeof(out));
        str_append(out, allow_names[i], sizeof(out));
        str_append(out, "\n", sizeof(out));
    }
    fs_mkdir("/etc");
    fs_write_file(POLICY_PATH, out, (uint32_t)str_len(out));
}

void policy_load(void) {
    uint64_t n = 0;
    const void *d = fs_read_file(POLICY_PATH, &n);
    if (!d || !n) return;
    const char *p = (const char *)d;
    allow_count = 0;

    uint64_t i = 0;
    while (i < n) {
        char line[96];
        int k = 0;
        while (i < n && p[i] != '\n' && k < (int)sizeof(line) - 1)
            line[k++] = p[i++];
        line[k] = '\0';
        while (i < n && p[i] != '\n') i++;
        if (i < n) i++;                       /* step over the newline */

        if (str_starts_with(line, "uac=")) {
            int v = line[4] - '0';
            if (v >= 0 && v < UAC_LEVELS) uac_level = v;
        } else if (str_starts_with(line, "allowlist=")) {
            allowlist_on = (line[10] == '1');
        } else if (str_starts_with(line, "scanner=")) {
            scanner_on = (line[8] == '1');
        } else if (str_starts_with(line, "allow=")) {
            allow_add(line + 6);
        }
    }
}
