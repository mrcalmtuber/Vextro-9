#ifndef VEXTRO_SECURITY_ANTI_VIRUS_H
#define VEXTRO_SECURITY_ANTI_VIRUS_H

/*
 * src/security/anti_virus.h — the three policies that decide whether a
 * program is allowed to run.
 *
 * These came out of src/security.h, which had grown to hold six
 * unrelated things: a tar writer, an encrypted container, a directory
 * walker, and this. The tar writer and the vault stayed — they are
 * *storage* — and what moved here is everything consulted on the path
 * between "the user clicked a program" and "the program started":
 *
 *     the allowlist   may this binary run at all
 *     the scanner     does its content match a known signature
 *     UAC             how often privileged actions should ask
 *
 * ---- the honest limitation, stated once ----
 *
 * None of this is a kernel enforcement boundary. A `.vx` application
 * executes with full privileges in a shared address space, so what
 * lives here is *policy* — the system refusing to start something, and
 * saying why — not *isolation*. A program that gets running can still
 * do anything. Every panel and every README sentence about this says so,
 * and moving the code into a directory named `security` does not change
 * it.
 *
 * What it does buy is real: an account cannot silently run software the
 * administrator has not allowed, and a binary altered since it was
 * installed is noticed before it starts.
 */

#include <stdint.h>

/* ===== UAC =====
 *
 * How often privileged actions stop to ask. Read by the terminal's
 * policy panel and by the loader.
 */
enum {                      /* how often privileged actions ask */
    UAC_NEVER = 0,          /* never prompt (the old behaviour) */
    UAC_ADMIN_TASKS,        /* prompt for administrator actions */
    UAC_ALWAYS_INSTALL,     /* ...and for anything that installs software */
    UAC_PARANOID,           /* ...and for every program launch */
    UAC_LEVELS
};

extern const char *const uac_level_names[UAC_LEVELS];
extern int uac_level;

/* ===== THE ALLOWLIST ===== */

#define ALLOW_MAX  32
#define ALLOW_NAME 32

extern int  allowlist_on;   /* only listed programs may run */
extern int  scanner_on;     /* check binaries before running them */

extern char allow_names[ALLOW_MAX][ALLOW_NAME];
extern int  allow_count;

/* Whether `name` may run. Always true when the allowlist is off, which
 * is the default — an allowlist that is on by default is one that gets
 * turned off permanently the first time it blocks something. */
int allow_permits(const char *name);
int allow_add(const char *name);
int allow_remove(const char *name);

/* ===== THE SCANNER ===== */

#define SCAN_CLEAN     0
#define SCAN_SIGNATURE 1
#define SCAN_MALFORMED 2

/* Why the last scan_buffer() returned what it did: a signature name, or
 * a description of the structural problem. Empty after a clean scan. */
extern char scan_detail[64];

/*
 * Scan `len` bytes for known signatures and for structural damage.
 * Returns one of SCAN_CLEAN, SCAN_SIGNATURE or SCAN_MALFORMED, and sets
 * scan_detail.
 *
 * Where several signatures match the same buffer, the one earliest in
 * the signature table is reported — not the one earliest in the buffer.
 * That is the behaviour the naive scanner had before the automaton
 * replaced it, and tools/av_test.c checks the two still agree.
 */
int scan_buffer(const uint8_t *data, uint32_t len);

/*
 * Build the matching automaton. Called once during boot; reports its
 * size on the console. Calling scan_buffer() without it is safe — it
 * builds on first use — but doing it eagerly keeps the cost off the
 * first program launch and makes the state visible in the boot log.
 */
void av_init(void);

/* ===== POLICY PERSISTENCE ===== */

#define POLICY_PATH "/etc/policy.cfg"

void policy_save(void);
void policy_load(void);

#endif /* VEXTRO_SECURITY_ANTI_VIRUS_H */
