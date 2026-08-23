/*
 * profile_test.c — the isolation guard, against the ways around it.
 *
 * A guard that returns "denied" for `/Documents and Settings/bob` proves
 * almost nothing. Every interesting failure of an access check is a path
 * that means bob's directory while not looking like it: a different
 * separator, a doubled slash, a different case, a `..`, or a name that
 * merely starts the same way. So the cases below are mostly the ones
 * that should be denied and could plausibly slip through, and each one
 * names the mistake it is there to catch.
 *
 * Built and run on the host against the same src/profile.h the kernel
 * compiles. What the kernel supplies and this does not is stubbed here:
 *
 *   exf_upper              mirrors src/exfat.h:329 -- ASCII upper only.
 *                          The guard has to fold case the same way
 *                          exf_name_eq does or it is stricter than the
 *                          lookup it guards, which is the bug this file
 *                          exists to prevent.
 *   str_copy / str_append  mirror src/gfx.h:534 and :540 -- bounded,
 *                          always NUL-terminated.
 *   fs_stat / fs_mkdir     a tiny in-memory directory set, so that
 *                          create_user_profile can be run for real and
 *                          the tree it builds inspected.
 *
 * If one of those ever diverges from the header it mirrors, this test
 * keeps passing while the kernel is wrong -- so they are listed above,
 * deliberately, rather than left to be discovered.
 */
#include <stdio.h>
#include <stdint.h>

static int checks = 0, failures = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) printf("  ok    %s\n", what);
    else { failures++; printf("  FAIL  %s\n", what); }
}

/* ---- what the kernel supplies ---- */

#define EXF_NAME_MAX 128
#define EXF_ATTR_DIR 0x10

typedef struct { char name[EXF_NAME_MAX]; uint32_t first_clus; uint8_t attr;
                 uint64_t size; } exf_dirent_t;

static char exf_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void str_append(char *dst, const char *src, int max) {
    int len = str_len(dst);
    int i = 0;
    while (src[i] && len < max - 1) { dst[len++] = src[i++]; }
    dst[len] = '\0';
}

/* accounts, as users.h lays them out */
#define USER_MAX        8
#define USER_NAME_MAX   9
#define USER_FLAG_ADMIN 1

typedef struct { char name[USER_NAME_MAX]; uint8_t flags; } user_t;

static user_t users[USER_MAX];
static int    user_count   = 0;
static int    user_current = -1;

static int user_find(const char *name) {
    for (int i = 0; i < user_count; i++) {
        int k = 0;
        while (users[i].name[k] && name[k] && users[i].name[k] == name[k]) k++;
        if (!users[i].name[k] && !name[k]) return i;
    }
    return -1;
}

static int user_is_admin(int idx) {
    return idx >= 0 && idx < user_count && (users[idx].flags & USER_FLAG_ADMIN);
}

/* the filesystem, as desktop.h presents it */
#define FS_NONE  0
#define FS_EXFAT 1
#define FS_FAT32 2

static int fs_kind = FS_EXFAT;
static const char *fs_errstr = "";

#define FAKE_MAX 32
static char fake_dirs[FAKE_MAX][256];
static int  fake_count = 0;

static int fake_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && exf_upper(a[i]) == exf_upper(b[i])) i++;
    return !a[i] && !b[i];
}

static int prof_may(const char *path, int write);   /* from profile.h */

static int fs_stat(const char *path, uint64_t *size, int *is_dir) {
    if (!prof_may(path, 0)) return 0;
    for (int i = 0; i < fake_count; i++)
        if (fake_eq(fake_dirs[i], path)) {
            if (size) *size = 0;
            if (is_dir) *is_dir = 1;
            return 1;
        }
    return 0;
}

static int fs_mkdir(const char *path) {
    if (!prof_may(path, 1)) return -1;
    if (fake_count >= FAKE_MAX) { fs_errstr = "full"; return -1; }
    str_copy(fake_dirs[fake_count++], path, 256);
    return 0;
}

static int exf_lookup(const char *path, exf_dirent_t *out) {
    (void)path; (void)out;
    return 0;      /* no cluster numbers on the host */
}

#include "../src/profile.h"

/* ---- helpers ---- */

static void add_user(const char *name, int admin) {
    str_copy(users[user_count].name, name, USER_NAME_MAX);
    users[user_count].flags = (uint8_t)(admin ? USER_FLAG_ADMIN : 0);
    user_count++;
}

static int has_dir(const char *p) {
    for (int i = 0; i < fake_count; i++) if (fake_eq(fake_dirs[i], p)) return 1;
    return 0;
}

static int streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return !a[i] && !b[i];
}

/* denied == the guard said no */
static int denied(const char *path) { return !prof_may(path, 0); }
static int allowed(const char *path) { return prof_may(path, 1); }

int main(void) {
    printf("profile: the isolation guard\n");

    add_user("alice", 0);
    add_user("al",    0);      /* the prefix of alice, on purpose */
    add_user("bob",   0);
    add_user("root",  1);      /* an administrator */

    /* ---- canonicalisation ---- */
    {
        char out[PROFILE_PATH_MAX];

        fs_native("\\Documents and Settings\\bob", out, sizeof(out));
        ok("backslashes become the separator the drivers parse",
           streq(out, "/Documents and Settings/bob"));

        fs_native("C:\\Documents and Settings\\bob", out, sizeof(out));
        ok("a drive letter is dropped, so USERPROFILE resolves",
           streq(out, "/Documents and Settings/bob"));

        fs_native("etc/users.db", out, sizeof(out));
        ok("a relative path is made absolute", streq(out, "/etc/users.db"));

        ok("// collapses",
           prof_canon("//Documents and Settings//bob//", out, sizeof(out)) &&
           streq(out, "/Documents and Settings/bob"));

        ok("a trailing slash is not a different path",
           prof_canon("/Documents and Settings/bob/", out, sizeof(out)) &&
           streq(out, "/Documents and Settings/bob"));

        ok(".. is refused rather than resolved",
           !prof_canon("/Documents and Settings/alice/../bob", out, sizeof(out)));

        ok("a bare .. is refused too", !prof_canon("/..", out, sizeof(out)));

        ok("a single . is just where it already is",
           prof_canon("/Documents and Settings/./bob", out, sizeof(out)) &&
           streq(out, "/Documents and Settings/bob"));
    }

    /* ---- an account reaching its own tree ---- */
    user_current = 0;                                  /* alice */
    ok("alice reaches her own profile",
       allowed("/Documents and Settings/alice"));
    ok("alice reaches her own My Documents",
       allowed("/Documents and Settings/alice/My Documents/notes.txt"));
    ok("alice reaches her own legacy home",
       allowed("/home/alice/settings.cfg"));

    /* ---- and not another's ---- */
    ok("alice cannot read bob's profile",
       denied("/Documents and Settings/bob"));
    ok("alice cannot read inside bob's profile",
       denied("/Documents and Settings/bob/Desktop/secret.txt"));
    ok("alice cannot write into bob's profile",
       !prof_may("/Documents and Settings/bob/Desktop/x", 1));
    ok("alice cannot read bob's legacy home -- files an older build "
       "wrote are private too",
       denied("/home/bob/settings.cfg"));

    /* ---- the ways around it ---- */
    ok("BYPASS backslashes: \\Documents and Settings\\bob is bob's",
       denied("\\Documents and Settings\\bob\\Desktop"));
    ok("BYPASS drive letter: C:\\Documents and Settings\\bob is bob's",
       denied("C:\\Documents and Settings\\bob"));
    ok("BYPASS case: exf_name_eq folds it, so the guard must too",
       denied("/DOCUMENTS AND SETTINGS/BOB/Desktop"));
    ok("BYPASS mixed case on the leaf",
       denied("/Documents and Settings/BoB"));
    ok("BYPASS doubled separators",
       denied("//Documents and Settings//bob"));
    ok("BYPASS trailing slash",
       denied("/Documents and Settings/bob/"));
    ok("BYPASS dot-dot out of her own tree",
       denied("/Documents and Settings/alice/../bob/Desktop"));
    ok("BYPASS dot-dot through the legacy root",
       denied("/home/alice/../bob"));
    ok("BYPASS a . component in the middle",
       denied("/Documents and Settings/./bob"));

    /* The one a plain strncmp gets wrong in the other direction. */
    user_current = 1;                                  /* al */
    ok("BYPASS prefix: account 'al' does not own 'alice'",
       denied("/Documents and Settings/alice/My Documents"));
    ok("account 'al' still reaches its own tree",
       allowed("/Documents and Settings/al/Desktop"));

    /* ---- what is shared ---- */
    user_current = 0;
    ok("the container itself is listable -- that is how anyone sees "
       "their own folder is there",
       allowed("/Documents and Settings"));
    ok("the legacy container is listable", allowed("/home"));
    ok("the account database is not inside a profile, so a password "
       "change still writes",
       allowed("/etc/users.db"));
    ok("the volume root is listable", allowed("/"));
    ok("a file beside the legacy root is not inside it",
       allowed("/home.vault"));

    /* ---- the administrator ---- */
    user_current = 3;                                  /* root */
    ok("an administrator reaches another profile",
       allowed("/Documents and Settings/bob/Desktop/secret.txt"));
    ok("an administrator is not stopped by a .. either",
       allowed("/Documents and Settings/alice/../bob"));

    /* ---- before anyone signs in ---- */
    user_current = -1;
    ok("with nobody signed in the volume is open -- users_load reads "
       "the database before there is anyone to check",
       allowed("/etc/users.db"));
    ok("and so is a profile, for the same reason",
       allowed("/Documents and Settings/bob"));

    /* ---- building a profile ---- */
    {
        user_current = -1;
        fake_count = 0;
        fs_kind = FS_EXFAT;

        struct directory *d = create_user_profile("bob");
        ok("create_user_profile returns a directory", d != 0);
        ok("  it is the home directory itself",
           d && streq(d->path, "/Documents and Settings/bob"));
        ok("  it knows which account owns it", d && d->owner == 2);
        ok("  it is a directory", d && d->is_dir == 1);
        ok("the container was made", has_dir("/Documents and Settings"));
        ok("the home directory was made",
           has_dir("/Documents and Settings/bob"));
        ok("My Documents was made",
           has_dir("/Documents and Settings/bob/My Documents"));
        ok("Desktop was made",
           has_dir("/Documents and Settings/bob/Desktop"));

        int before = fake_count;
        struct directory *again = create_user_profile("bob");
        ok("calling it twice is not an error", again != 0);
        ok("  and makes nothing a second time -- which is what lets "
           "every login call it", fake_count == before);

        /* An account made by an older build: only /home/<name> exists.
         * The next login is what gives it a tree. */
        fake_count = 0;
        str_copy(fake_dirs[fake_count++], "/home", 256);
        str_copy(fake_dirs[fake_count++], "/home/alice", 256);
        create_user_profile("alice");
        ok("a legacy account gains a profile without its old home "
           "being touched",
           has_dir("/Documents and Settings/alice/My Documents") &&
           has_dir("/home/alice"));
    }

    /* ---- the FAT32 fallback, stated rather than silent ---- */
    {
        fs_kind = FS_FAT32;
        fake_count = 0;
        user_current = -1;

        char home[PROFILE_PATH_MAX];
        profile_home("bob", home, sizeof(home));
        ok("on FAT32 the home is /home/<name>, not a name 8.3 cannot hold",
           streq(home, "/home/bob"));

        create_user_profile("bob");
        ok("  the home directory is still made", has_dir("/home/bob"));
        ok("  and no My Documents is invented for it to mangle",
           !has_dir("/home/bob/My Documents"));

        user_current = 0;                              /* alice */
        ok("  isolation still applies on FAT32", denied("/home/bob"));
        fs_kind = FS_EXFAT;
    }

    /* ---- the environment ---- */
    {
        profile_bind_env("alice");

        ok("USERNAME is the account", streq(env_get("USERNAME"), "alice"));
        ok("USERPROFILE is the Windows spelling of the profile",
           streq(env_get("USERPROFILE"),
                 "C:\\Documents and Settings\\alice"));
        ok("HOME is the spelling the shell splices into paths",
           streq(env_get("HOME"), "/Documents and Settings/alice"));
        ok("APPDATA points inside the profile",
           streq(env_get("APPDATA"),
                 "C:\\Documents and Settings\\alice\\My Documents"));
        ok("names are case-insensitive, as Windows names are",
           streq(env_get("userprofile"), env_get("USERPROFILE")));
        ok("an unset name is not a value", env_get("NOTHING") == 0);

        /* The round trip that makes USERPROFILE more than decoration:
         * the string a Windows program is handed has to come back
         * through fs_native as a path this system can open. */
        char native[PROFILE_PATH_MAX];
        fs_native(env_get("USERPROFILE"), native, sizeof(native));
        ok("USERPROFILE resolves back to the real path",
           streq(native, "/Documents and Settings/alice"));

        /* And it has to still be alice's when it does. */
        user_current = 0;
        ok("  and is still hers when it does",
           allowed(env_get("USERPROFILE")));
        user_current = 2;                              /* bob */
        ok("  and still not bob's", denied(env_get("USERPROFILE")));

        profile_bind_env("bob");
        ok("binding again replaces rather than accumulates",
           streq(env_get("USERNAME"), "bob") && env_count <= ENV_MAX);

        env_clear();
        ok("logging out empties it", env_count == 0 && env_get("HOME") == 0);
    }

    /* ---- the block a Windows process is handed ---- */
    {
        profile_bind_env("alice");
        uint8_t blk[2048];
        uint32_t n = env_build_utf16(blk, sizeof(blk));
        ok("the environment block is built", n > 0);
        ok("  it is UTF-16, so the second byte of a name is zero",
           n > 2 && blk[0] == 'U' && blk[1] == 0);
        ok("  and it ends in the double NUL that terminates the run",
           n >= 4 && blk[n-1] == 0 && blk[n-2] == 0 &&
                     blk[n-3] == 0 && blk[n-4] == 0);

        /* A capacity it cannot fit must fail rather than truncate: a
         * half-written environment block has no terminator, and a
         * runtime walking it does not stop. */
        ok("  too small a buffer is refused, not truncated",
           env_build_utf16(blk, 8) == 0);
        env_clear();
    }

    printf("\n  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
