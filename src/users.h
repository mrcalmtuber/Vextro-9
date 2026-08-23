#ifndef USERS_H
#define USERS_H

/*
 * User accounts.
 *
 * The machine had one anonymous secret in /keycode.sys, stored and
 * compared in plaintext, with no notion of who was using it and no way
 * back to the login screen short of a reboot. This adds names, per-user
 * home directories, an administrator role, and logging out.
 *
 * The record format is one line of text per account:
 *
 *     name|salt|hash|flags
 *
 * with salt and hash in lowercase hex. Text rather than a packed struct
 * because the file is tiny, `cat /etc/users.db` should be legible when
 * something goes wrong, and a text line has no alignment or endianness to
 * get wrong across the two architectures this has to build on.
 *
 * What this is not: a security boundary. Nothing on the volume is
 * encrypted, and a `.vx` application runs with kernel privileges in a
 * shared address space, so anything already executing can do as it likes.
 * The administrator flag governs the user interface, not the hardware.
 *
 * That last sentence is now doing more work than it used to, and
 * profile.h is where it is argued properly. The short version: the flag
 * governs the user interface, and since there are no file syscalls, the
 * user interface is the only way a person reaches the disk. So one
 * account genuinely cannot read another's files. A program that is
 * already running still can.
 */

/*
 * From profile.h, which is included immediately after this file and
 * cannot be included before it -- its isolation guard asks which account
 * is signed in, and that is user_current, declared below.
 *
 * profile_home() is what makes the layout a single decision rather than
 * a literal repeated in four places: it answers with
 * \Documents and Settings\<name> on exFAT and /home/<name> on a FAT32
 * volume that cannot spell it.
 */
struct directory;
static void profile_home(const char *name, char *out, int max);
static struct directory *create_user_profile(const char *username);

#define USER_MAX        8       /* accounts on one machine   */
#define USER_NAME_MAX   9       /* 8 characters plus the NUL */
#define USER_SALT_LEN   8
#define USER_FLAG_ADMIN 1

typedef struct {
    char    name[USER_NAME_MAX];
    uint8_t salt[USER_SALT_LEN];
    uint8_t hash[32];
    uint8_t flags;
} user_t;

static user_t users[USER_MAX];
static int    user_count = 0;
static int    user_current = -1;         /* index of whoever is logged in */
static char   user_err[64] = "";

#define USERS_DB   "/etc/users.db"
#define USERS_TMP  "/etc/users.new"

/* ===== helpers ===== */

static int u_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void u_tohex(const uint8_t *in, uint32_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i * 2]     = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 15];
    }
    out[n * 2] = '\0';
}

static int u_fromhex(const char *in, uint8_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int hi = u_hexval(in[i * 2]), lo = u_hexval(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/*
 * Names are lowercase alphanumeric and at most eight characters.
 *
 * Not an arbitrary limit: the name becomes a directory, and FAT32 file
 * creation in this tree is 8.3-limited (fat32.h rejects anything else).
 * exFAT would allow far more, but one rule for both filesystems is worth
 * more than four extra characters on one of them.
 *
 * The limit outlived the reason it was chosen and is still right. The
 * profile tree is now \Documents and Settings\<name>, a name exFAT can
 * hold and FAT32 cannot -- but a FAT32 volume still falls back to
 * /home/<name>, so the eight-character rule is still what makes an
 * account portable between the two. See profile.h.
 */
static int user_name_ok(const char *n) {
    if (!n || !n[0]) { str_copy(user_err, "name cannot be empty", sizeof(user_err)); return 0; }
    int i = 0;
    for (; n[i]; i++) {
        if (i >= USER_NAME_MAX - 1) {
            str_copy(user_err, "name is limited to 8 characters", sizeof(user_err));
            return 0;
        }
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            str_copy(user_err, "use lowercase letters and digits only",
                     sizeof(user_err));
            return 0;
        }
    }
    if (n[0] >= '0' && n[0] <= '9') {
        str_copy(user_err, "name must start with a letter", sizeof(user_err));
        return 0;
    }
    return 1;
}

static int user_find(const char *name) {
    for (int i = 0; i < user_count; i++)
        if (str_eq(users[i].name, name)) return i;
    return -1;
}

static int user_is_admin(int idx) {
    return idx >= 0 && idx < user_count && (users[idx].flags & USER_FLAG_ADMIN);
}

static int user_admin_count(void) {
    int n = 0;
    for (int i = 0; i < user_count; i++)
        if (users[i].flags & USER_FLAG_ADMIN) n++;
    return n;
}

static const char *user_name_of(int idx) {
    if (idx < 0 || idx >= user_count) return "nobody";
    return users[idx].name;
}

static void user_home(int idx, char *out, int max) {
    profile_home(user_name_of(idx), out, max);
}

/* ===== persistence ===== */

static int users_load(void) {
    user_count = 0;

    uint64_t len = 0;
    const void *d = fs_read_file(USERS_DB, &len);
    if (!d || len == 0) return 0;

    const char *p = (const char *)d;
    uint64_t i = 0;
    while (i < len && user_count < USER_MAX) {
        /* one line */
        char line[160];
        int n = 0;
        while (i < len && p[i] != '\n' && n < (int)sizeof(line) - 1)
            line[n++] = p[i++];
        while (i < len && p[i] != '\n') i++;      /* overlong: discard tail */
        if (i < len) i++;                          /* step over the newline */
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        /* split on '|' */
        char *f[4];
        int nf = 0;
        f[nf++] = line;
        for (int k = 0; line[k] && nf < 4; k++)
            if (line[k] == '|') { line[k] = '\0'; f[nf++] = line + k + 1; }
        if (nf != 4) continue;

        user_t u;
        str_copy(u.name, f[0], USER_NAME_MAX);
        if (!u_fromhex(f[1], u.salt, USER_SALT_LEN)) continue;
        if (!u_fromhex(f[2], u.hash, 32)) continue;
        u.flags = (uint8_t)(f[3][0] == '1' ? USER_FLAG_ADMIN : 0);

        users[user_count++] = u;
    }
    return user_count;
}

/*
 * Write the table.
 *
 * To a temporary name first, then swap. exf_write_file frees the existing
 * cluster chain before writing the new one, so a failure part-way through
 * a direct write would take every account with it and leave a machine no
 * one can log into. Writing beside it and renaming means the worst case
 * is a stale file rather than no file.
 */
static int users_save(void) {
    static char out[USER_MAX * 160];
    int o = 0;

    for (int i = 0; i < user_count; i++) {
        char hexs[USER_SALT_LEN * 2 + 1], hexh[65];
        u_tohex(users[i].salt, USER_SALT_LEN, hexs);
        u_tohex(users[i].hash, 32, hexh);

        const char *parts[4] = { users[i].name, hexs, hexh,
                                 (users[i].flags & USER_FLAG_ADMIN) ? "1" : "0" };
        for (int k = 0; k < 4; k++) {
            for (int j = 0; parts[k][j] && o < (int)sizeof(out) - 2; j++)
                out[o++] = parts[k][j];
            if (o < (int)sizeof(out) - 2) out[o++] = (k == 3) ? '\n' : '|';
        }
    }

    fs_mkdir("/etc");                    /* harmless if it already exists */

    if (fs_write_file(USERS_TMP, out, (uint32_t)o) != 0) {
        str_copy(user_err, fs_errstr, sizeof(user_err));
        return -1;
    }
    /* No rename in the filesystem layer, so this is delete-then-write.
     * The temporary file is what makes that survivable: if the second
     * write fails, /etc/users.new still holds the table. */
    fs_delete(USERS_DB);
    if (fs_write_file(USERS_DB, out, (uint32_t)o) != 0) {
        str_copy(user_err, fs_errstr, sizeof(user_err));
        return -1;
    }
    fs_delete(USERS_TMP);
    return 0;
}

/* ===== accounts ===== */

/*
 * Salt.
 *
 * The cycle counter is the only thing here that differs run to run, so it
 * is the seed; the mixing step spreads it, and the counter is re-read
 * every pass so two accounts created in the same session cannot collide.
 *
 * Its own generator rather than login.h's: that header sits at a
 * different point in the include order on the two architecture trees, and
 * a salt is not worth making this file depend on it.
 */
static uint32_t user_rng_state = 0x9E3779B9u;

static uint32_t user_rand(void) {
    user_rng_state ^= (uint32_t)cycle_now();
    user_rng_state ^= user_rng_state << 13;
    user_rng_state ^= user_rng_state >> 17;
    user_rng_state ^= user_rng_state << 5;
    return user_rng_state;
}

static void user_make_salt(uint8_t *out) {
    for (int i = 0; i < USER_SALT_LEN; i++)
        out[i] = (uint8_t)(user_rand() >> 13);
}

static int user_add(const char *name, const char *pw, int admin) {
    if (!user_name_ok(name)) return -1;
    if (user_count >= USER_MAX) {
        str_copy(user_err, "no room for another account", sizeof(user_err));
        return -1;
    }
    if (user_find(name) >= 0) {
        str_copy(user_err, "that name is already taken", sizeof(user_err));
        return -1;
    }
    uint32_t pwlen = 0;
    while (pw[pwlen]) pwlen++;
    if (pwlen == 0) {
        str_copy(user_err, "password cannot be empty", sizeof(user_err));
        return -1;
    }

    user_t *u = &users[user_count];
    str_copy(u->name, name, USER_NAME_MAX);
    user_make_salt(u->salt);
    pw_hash(u->salt, USER_SALT_LEN, pw, pwlen, u->hash);
    u->flags = (uint8_t)(admin ? USER_FLAG_ADMIN : 0);
    user_count++;

    if (users_save() != 0) { user_count--; return -1; }

    /*
     * The account exists whether or not the tree could be built. A full
     * or read-only volume is a reason to have no My Documents yet, not a
     * reason to refuse someone an account they have just chosen a
     * password for -- and session_begin() retries this on every login,
     * so the profile appears as soon as the volume will take it.
     */
    create_user_profile(u->name);
    return user_count - 1;
}

static int user_check(int idx, const char *pw) {
    if (idx < 0 || idx >= user_count) return 0;
    uint32_t pwlen = 0;
    while (pw[pwlen]) pwlen++;

    uint8_t got[32];
    pw_hash(users[idx].salt, USER_SALT_LEN, pw, pwlen, got);
    return ct_equal(got, users[idx].hash, 32);
}

static int user_set_password(int idx, const char *pw) {
    if (idx < 0 || idx >= user_count) return -1;
    uint32_t pwlen = 0;
    while (pw[pwlen]) pwlen++;
    if (pwlen == 0) {
        str_copy(user_err, "password cannot be empty", sizeof(user_err));
        return -1;
    }
    user_make_salt(users[idx].salt);
    pw_hash(users[idx].salt, USER_SALT_LEN, pw, pwlen, users[idx].hash);
    return users_save();
}

static int user_del(const char *name) {
    int idx = user_find(name);
    if (idx < 0) {
        str_copy(user_err, "no such user", sizeof(user_err));
        return -1;
    }
    if (idx == user_current) {
        str_copy(user_err, "cannot delete the account you are using",
                 sizeof(user_err));
        return -1;
    }
    if ((users[idx].flags & USER_FLAG_ADMIN) && user_admin_count() <= 1) {
        str_copy(user_err, "cannot delete the last administrator",
                 sizeof(user_err));
        return -1;
    }

    for (int i = idx; i + 1 < user_count; i++) users[i] = users[i + 1];
    user_count--;
    if (user_current > idx) user_current--;   /* indices shifted down */
    return users_save();
}

static int user_set_admin(const char *name, int on) {
    int idx = user_find(name);
    if (idx < 0) {
        str_copy(user_err, "no such user", sizeof(user_err));
        return -1;
    }
    if (!on && (users[idx].flags & USER_FLAG_ADMIN) && user_admin_count() <= 1) {
        str_copy(user_err, "cannot remove the last administrator",
                 sizeof(user_err));
        return -1;
    }
    if (on) users[idx].flags |= USER_FLAG_ADMIN;
    else    users[idx].flags &= (uint8_t)~USER_FLAG_ADMIN;
    return users_save();
}

/*
 * Carry an old single-passcode machine forward.
 *
 * /keycode.sys held a plaintext secret and no name. Rather than discard
 * it and lock the owner out of their own disk, it becomes an
 * administrator account called "admin" with that same password, and the
 * plaintext file is removed once the hashed record is safely written.
 */
static int users_migrate_keycode(void) {
    uint64_t len = 0;
    const void *d = fs_read_file("/keycode.sys", &len);
    if (!d || len == 0 || len > 120) return 0;

    char pw[128];
    const char *p = (const char *)d;
    uint64_t i = 0;
    for (; i < len && i < sizeof(pw) - 1; i++) pw[i] = p[i];
    pw[i] = '\0';

    if (user_add("admin", pw, 1) < 0) return 0;
    fs_delete("/keycode.sys");
    return 1;
}

#endif /* USERS_H */
