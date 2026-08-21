#ifndef GPO_H
#define GPO_H

#include <stdint.h>
#include "smb2.h"
#include "registry.h"

/*
 * src/gpo.h — Group Policy: settings that arrive from somewhere else.
 *
 * This is the mechanism that makes a thousand Windows machines behave
 * the same way without anyone touching them. A domain administrator
 * edits a policy; every machine notices, downloads it, and applies it.
 * The surprising part, once you look, is how little machinery is
 * involved: the policies are *files on a file share*, and a client
 * fetches and parses them. There is no dedicated protocol at all.
 *
 * Which is why this file is short and why it comes last of the three.
 * It is built entirely out of what already exists -- src/smb2.h to
 * reach the share, src/registry.h to store the result -- and adds only
 * the two file formats in between.
 *
 * ---- the two formats ----
 *
 * GPT.INI is a Windows INI file with a Version number in it. The
 * version is two counters packed into one integer: the user half in the
 * top sixteen bits, the machine half in the bottom. A client that reads
 * it as a single number sees the version leap by 65,536 when somebody
 * changes a user setting, which looks like corruption and is not.
 *
 * Registry.pol is "PReg": a four-byte signature, a version, and then a
 * flat sequence of records
 *
 *     [ key ; value ; type ; size ; data ]
 *
 * where every one of those brackets and semicolons is a *UTF-16
 * character*, not a byte. Parsing it a byte at a time finds the
 * delimiters exactly where they are expected and produces key names
 * with a NUL between every letter.
 *
 * ---- applied as one change or not at all ----
 *
 * A policy is a set, and half of one is not a smaller policy -- it is a
 * machine in a state the administrator never specified. So the whole
 * file is parsed before anything is written, and the writes go inside
 * the registry's transaction: reg_begin, then every value, then one
 * reg_commit. A malformed record anywhere rolls the lot back.
 *
 * ---- what is not here ----
 *
 * No user policies (the Machine half only), no security filtering by
 * group membership, no WMI filters, no administrative templates -- the
 * .admx files are a presentation layer for the editor and never reach a
 * client. Deletion markers are honoured; the "**delvals." form that
 * clears an entire key is not, and is refused rather than ignored.
 */

#define GPO_MAX_VALUES  128
#define GPO_SYSVOL      "SYSVOL"

typedef struct {
    char     guid[64];
    uint32_t version;
    uint32_t machine_version;
    uint32_t user_version;
    char     display[96];
    int      values;
} gpo_policy_t;

typedef struct {
    int          count;
    gpo_policy_t policy[8];
    int          applied;
    int          failed;
    int          unsaved;
    char         last_error[96];
} gpo_state_t;

static gpo_state_t gpo;

/* ===========================================================
 * GPT.INI
 * =========================================================== */

static int gpo_ini_eq(const char *line, const char *key, int n) {
    for (int i = 0; i < n; i++) {
        char a = line[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return line[n] == '=';
}

static int gpo_parse_ini(const uint8_t *p, uint32_t n, gpo_policy_t *out) {
    out->version = 0;
    out->display[0] = 0;

    uint32_t at = 0;
    while (at < n) {
        uint32_t end = at;
        while (end < n && p[end] != '\n' && p[end] != '\r') end++;
        const char *line = (const char *)(p + at);
        uint32_t len = end - at;

        if (len > 8 && gpo_ini_eq(line, "version", 7)) {
            uint32_t v = 0;
            for (uint32_t i = 8; i < len; i++) {
                if (line[i] < '0' || line[i] > '9') break;
                v = v * 10 + (uint32_t)(line[i] - '0');
            }
            out->version = v;
        } else if (len > 12 && gpo_ini_eq(line, "displayname", 11)) {
            uint32_t k = 0;
            for (uint32_t i = 12; i < len && k + 1 < sizeof out->display; i++)
                out->display[k++] = line[i];
            out->display[k] = 0;
        }

        at = end;
        while (at < n && (p[at] == '\n' || p[at] == '\r')) at++;
    }

    /* Two counters in one word, and they move independently. */
    out->user_version    = out->version >> 16;
    out->machine_version = out->version & 0xFFFF;
    return out->version ? 0 : -1;
}

/* ===========================================================
 * Registry.pol
 * =========================================================== */

/*
 * Read a UTF-16LE string up to its NUL, narrowing to bytes.
 *
 * Returns the number of *bytes consumed*, including the terminator, so
 * the caller advances correctly even when the string was truncated to
 * fit. Returning the output length instead is the mistake that
 * desynchronises the whole rest of the file after one long key name.
 */
static uint32_t gpo_wide_str(const uint8_t *p, uint32_t n, uint32_t at,
                             char *out, uint32_t max) {
    uint32_t k = 0, i = at;
    while (i + 1 < n) {
        uint16_t u = (uint16_t)(p[i] | (p[i + 1] << 8));
        i += 2;
        if (u == 0) break;
        if (k + 1 < max) out[k++] = (u < 0x100) ? (char)u : '?';
    }
    if (max) out[k] = 0;
    return i - at;
}

/* Expect one UTF-16 character. */
static int gpo_wide_ch(const uint8_t *p, uint32_t n, uint32_t at, char want) {
    if (at + 2 > n) return 0;
    return p[at] == (uint8_t)want && p[at + 1] == 0;
}

static uint32_t gpo_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Parse and apply. Returns the number of values written, or -1.
 *
 * `dry` parses without writing, which is how the whole file is
 * validated before the transaction opens: a policy that turns out to be
 * malformed halfway through should leave nothing behind at all.
 */
static int gpo_apply_pol(const uint8_t *p, uint32_t n, int dry) {
    if (n < 8 || p[0] != 'P' || p[1] != 'R' || p[2] != 'e' || p[3] != 'g') {
        serial_puts("[gpo] not a policy file\n");
        return -1;
    }
    if (gpo_le32(p + 4) != 1) {
        serial_puts("[gpo] unsupported policy file version\n");
        return -1;
    }

    uint32_t at = 8;
    int count = 0;

    while (at + 2 <= n && count < GPO_MAX_VALUES) {
        if (!gpo_wide_ch(p, n, at, '[')) {
            serial_puts("[gpo] expected a record\n");
            return -1;
        }
        at += 2;

        char key[192], name[64];
        at += gpo_wide_str(p, n, at, key, sizeof key);
        if (!gpo_wide_ch(p, n, at, ';')) return -1;
        at += 2;
        at += gpo_wide_str(p, n, at, name, sizeof name);
        if (!gpo_wide_ch(p, n, at, ';')) return -1;
        at += 2;

        if (at + 4 > n) return -1;
        uint32_t type = gpo_le32(p + at);
        at += 4;
        if (!gpo_wide_ch(p, n, at, ';')) return -1;
        at += 2;

        if (at + 4 > n) return -1;
        uint32_t size = gpo_le32(p + at);
        at += 4;
        if (!gpo_wide_ch(p, n, at, ';')) return -1;
        at += 2;

        if (size > n - at) {
            serial_puts("[gpo] a record claims more data than the file "
                        "holds\n");
            return -1;
        }
        const uint8_t *data = p + at;
        at += size;

        if (!gpo_wide_ch(p, n, at, ']')) return -1;
        at += 2;

        /* "**del." and friends are instructions, not value names. The
         * deletion form is honoured; the one that clears an entire key
         * is refused rather than silently skipped, because a policy that
         * expected a key emptied and finds it full is worse than one
         * that failed loudly. */
        if (name[0] == '*' && name[1] == '*') {
            if (name[2] == 'd' && name[3] == 'e' && name[4] == 'l' &&
                name[5] == '.') {
                if (!dry) reg_delete(key, name + 6);
                continue;
            }
            serial_puts("[gpo] unsupported policy directive ");
            serial_puts(name);
            serial_puts("\n");
            return -1;
        }

        if (dry) { count++; continue; }

        int ok = -1;
        if (type == REG_DWORD && size == 4) {
            ok = reg_set_dword(key, name, gpo_le32(data));
        } else if (type == 1 || type == 2) {          /* SZ, EXPAND_SZ */
            char s[160];
            uint32_t k = 0;
            for (uint32_t i = 0; i + 1 < size && k + 1 < sizeof s; i += 2) {
                uint16_t u = (uint16_t)(data[i] | (data[i + 1] << 8));
                if (!u) break;
                s[k++] = (u < 0x100) ? (char)u : '?';
            }
            s[k] = 0;
            ok = reg_set_string(key, name, s);
        } else if (type == REG_BINARY) {
            ok = reg_set(key, name, REG_BINARY, data,
                         size > REG_MAX_DATA ? REG_MAX_DATA : size);
        } else {
            serial_puts("[gpo] value type not supported: ");
            serial_puts(name);
            serial_puts("\n");
            return -1;
        }
        if (ok != 0) {
            serial_puts("[gpo] the registry refused ");
            serial_puts(name);
            serial_puts("\n");
            return -1;
        }
        count++;
    }
    return count;
}

/* ===========================================================
 * the refresh
 * =========================================================== */

static void gpo_join(char *out, uint32_t max, const char *a, const char *b) {
    uint32_t n = 0;
    for (const char *p = a; *p && n + 1 < max; p++) out[n++] = *p;
    if (b && b[0] && n + 1 < max) out[n++] = '\\';
    for (const char *p = b; p && *p && n + 1 < max; p++) out[n++] = *p;
    out[n] = 0;
}

static int gpo_fetch(const char *path, uint8_t *out, uint32_t max, uint32_t *len) {
    if (smb2_create(path, 0, 0) != 0) return -1;
    uint32_t got = 0;
    for (;;) {
        int n = smb2_read(got, out + got, max - got);
        if (n < 0) { smb2_close_file(); return -1; }
        if (n == 0) break;
        got += (uint32_t)n;
        if (got >= smb2.file_size || got >= max) break;
    }
    smb2_close_file();
    *len = got;
    return 0;
}

/*
 * Refresh policy from a domain controller.
 *
 * Everything is fetched and validated first, and only then is a single
 * registry transaction opened and committed. That ordering is the whole
 * design: a network read can fail halfway, and a half-applied policy is
 * a configuration nobody chose.
 */
static int gpo_refresh(const char *host, uint16_t port, const char *domain,
                       const char *user, const char *password, uint64_t now) {
    gpo.count = 0;
    gpo.applied = 0;
    gpo.failed = 0;
    gpo.unsaved = 0;
    gpo.last_error[0] = 0;

    if (smb2_connect(host, port, domain, user, password, now) != 0) return -1;

    char unc[128];
    unc[0] = '\\'; unc[1] = '\\';
    uint32_t n = 2;
    for (const char *p = host; *p && n < sizeof unc - 12; p++) unc[n++] = *p;
    unc[n++] = '\\';
    for (const char *p = GPO_SYSVOL; *p && n < sizeof unc - 1; p++) unc[n++] = *p;
    unc[n] = 0;

    if (smb2_tree_connect(unc) != 0) { smb2_disconnect(); return -1; }

    /* Which policies exist. */
    char poldir[160];
    gpo_join(poldir, sizeof poldir, domain, "Policies");

    static smb2_entry_t ents[SMB2_MAX_ENTRIES];
    if (smb2_create(poldir, 1, 0) != 0) {
        serial_puts("[gpo] no Policies directory under SYSVOL\n");
        smb2_disconnect();
        return -1;
    }
    int found = smb2_list("*", ents, SMB2_MAX_ENTRIES);
    smb2_close_file();
    if (found <= 0) { smb2_disconnect(); return -1; }

    static uint8_t buf[16384];
    int total = 0;

    for (int i = 0; i < found && gpo.count < 8; i++) {
        /* A policy directory is named by a GUID in braces. Anything
         * else under Policies is not one. */
        if (ents[i].name[0] != '{') continue;

        gpo_policy_t *g = &gpo.policy[gpo.count];
        uint32_t k = 0;
        for (const char *p = ents[i].name; *p && k + 1 < sizeof g->guid; p++)
            g->guid[k++] = *p;
        g->guid[k] = 0;
        g->values = 0;

        char base[224], path[288];
        gpo_join(base, sizeof base, poldir, g->guid);

        uint32_t len = 0;
        gpo_join(path, sizeof path, base, "GPT.INI");
        if (gpo_fetch(path, buf, sizeof buf, &len) != 0) continue;
        if (gpo_parse_ini(buf, len, g) != 0) continue;

        gpo_join(path, sizeof path, base, "Machine\\Registry.pol");
        if (gpo_fetch(path, buf, sizeof buf, &len) != 0) {
            /* A policy with no machine half is normal, not an error. */
            gpo.count++;
            continue;
        }

        /* Validate the whole file before writing any of it. */
        int check = gpo_apply_pol(buf, len, 1);
        if (check < 0) {
            gpo.failed++;
            serial_puts("[gpo] refusing a malformed policy: ");
            serial_puts(g->guid);
            serial_puts("\n");
            continue;
        }

        if (reg_begin() != 0) {
            smb2_disconnect();
            return -1;
        }
        int wrote = gpo_apply_pol(buf, len, 0);
        if (wrote < 0) {
            reg_rollback();
            gpo.failed++;
            continue;
        }
        /*
         * reg_commit() makes the change live and *then* writes the hive
         * to disk. A failure of the second half is not a failure of the
         * policy -- the settings are in force -- so rolling back here
         * would undo a correctly applied policy because a disk write
         * did not land. What it means is that they will not survive a
         * reboot, which is worth saying out loud instead.
         */
        if (reg_commit() != 0) {
            gpo.unsaved++;
            serial_puts("[gpo] applied, but the hive could not be written: "
                        "these settings are in force and will not survive a "
                        "reboot\n");
        }

        g->values = wrote;
        total += wrote;
        gpo.count++;

        serial_puts("[gpo] ");
        serial_puts(g->display[0] ? g->display : g->guid);
        serial_puts(": version ");
        serial_put_dec(g->machine_version);
        serial_puts(" machine / ");
        serial_put_dec(g->user_version);
        serial_puts(" user, ");
        serial_put_dec((uint32_t)wrote);
        serial_puts(" settings applied\n");
    }

    smb2_disconnect();
    gpo.applied = total;
    return total;
}

static int gpo_count(void)   { return gpo.count; }
static int gpo_applied(void) { return gpo.applied; }

#endif /* GPO_H */
