#ifndef PROFILE_H
#define PROFILE_H

/*
 * Profile trees.
 *
 * users.h gave the machine names and passwords, and gave each account a
 * directory: /home/<name>, made once at user_add() and never thought
 * about again. Nothing else knew it was theirs. The shell started there,
 * and that was the whole of it -- `cd /home/someoneelse` worked, `cat`
 * of their settings.cfg worked, and the file browser would happily list
 * the lot. One account per person, and one volume everybody shared.
 *
 * This file makes the directory mean something. Three things:
 *
 *   1. A profile is a *tree*, not a folder. Creating an account builds
 *      \Documents and Settings\<name>\ with My Documents and Desktop
 *      inside it, the way a machine of this vintage lays one out.
 *
 *   2. Logging in binds that tree to the session: the working directory,
 *      the file browser's path, an environment block that did not exist
 *      before, and -- for Windows processes -- the CurrentDirectory and
 *      Environment fields a PEB has always had room for and which this
 *      system left null.
 *
 *   3. Everything below fs_* asks prof_may() first. One account cannot
 *      read or write another's tree. An administrator can.
 *
 * What this is and is not
 * -----------------------
 * users.h says plainly that it is not a security boundary, and that
 * sentence has to be re-examined here rather than repeated, because this
 * file is the one making an access-control claim.
 *
 * There are no file syscalls. The whole of syscall.h is print, pixel,
 * mouse, exit, sbrk, write, yield, ticks, canvas, three drawing calls,
 * fork and meminfo -- a ring-3 program cannot open a file at all. Every
 * path that reaches the disk on a person's behalf comes from kernel-side
 * code driven by the interactive surface: the shell, the file browser,
 * coreutils, the store, backup and restore. All of it goes through fs_*,
 * and fs_* is a hundred-odd call sites through eight functions.
 *
 * So the guard here is real for the thing it guards: what the person at
 * the keyboard can reach. It is not, and cannot be, protection against
 * arbitrary ring-0 code -- a `.vx` application runs in the kernel's
 * address space and can call exf_lookup() directly, and no check in this
 * file is in its way. That is the same boundary policy.h already
 * describes: it decides what starts, not what a running program may do.
 *
 * The rule the guard follows
 * --------------------------
 * A guard that is stricter than the lookup it guards is a guard with a
 * way around it. exf_name_eq() upper-cases both sides before comparing,
 * so exFAT here is case-insensitive, so this file compares with
 * exf_upper() too -- otherwise /DOCUMENTS AND SETTINGS/bob would find
 * bob's files and match nobody's prefix on the way in. The same reasoning
 * governs separators and empty components: canonicalise the guard's own
 * copy exactly as the driver's path splitter does, then compare.
 */

/*
 * The tree.
 *
 * exFAT is the system volume and carries 128-character names, so the
 * spelling is the real one -- spaces and mixed case included.
 *
 * FAT32 cannot hold it. Creation there is 8.3 (fat32.h:594), and
 * "Documents and Settings", "My Documents" and even "Desktop" all fail
 * that test -- the first two on length and spaces, the last on the
 * lowercase tail. Rather than silently produce DOCUME~1 and call it the
 * same thing, a FAT32 volume keeps the flat /home/<name> layout with no
 * subdirectories, and profile_home() is the one place that knows which
 * volume it is talking to. The isolation guard applies to both.
 */
#define PROFILE_ROOT      "/Documents and Settings"
#define PROFILE_DOCS      "My Documents"
#define PROFILE_DESKTOP   "Desktop"

/* Where accounts lived before this file existed. Still honoured: a disk
 * written by an older build has real files under it, and they stay the
 * property of whoever's name is on the directory. */
#define PROFILE_LEGACY    "/home"

#define PROFILE_PATH_MAX  256

/*
 * A directory, as something a caller can hold.
 *
 * exf_dirent_t is the driver's idea of one and is reloaded from disk on
 * every lookup; this is the system's, and it says who the directory
 * belongs to -- which is the question the rest of this file exists to
 * answer and which no exFAT structure has a field for.
 */
struct directory {
    char     path[PROFILE_PATH_MAX];  /* absolute, canonical            */
    char     name[EXF_NAME_MAX];      /* the leaf, as stored on disk    */
    uint32_t first_clus;              /* 0 when the volume is not exFAT */
    int      is_dir;
    int      owner;                   /* users[] index, -1 if shared    */
    int      used;
};

/*
 * One per account rather than a heap allocation. kheap.h could serve
 * these, but nothing frees a profile handle -- an account has exactly one
 * home directory for as long as it exists -- so a table indexed the same
 * way users[] is indexed cannot leak and cannot dangle.
 */
static struct directory profile_dirs[USER_MAX];

/* ===== path canonicalisation =====
 *
 * Two different jobs, deliberately separate.
 *
 * fs_native() is what every fs_* entry runs on the caller's path before
 * handing it to a driver: it accepts the Windows spelling -- backslashes
 * and a drive letter -- and produces the one the drivers parse. This is
 * what lets USERPROFILE hold `C:\Documents and Settings\alice` and have
 * that string actually open something, rather than being a decoration
 * that only a Windows program pretends to read.
 *
 * prof_canon() is stricter and is only ever run on the guard's private
 * copy: it also collapses empty components and refuses `..` outright.
 */
static void fs_native(const char *in, char *out, int max) {
    int o = 0;

    /* `C:\...` and `c:/...` -- a drive letter, which this system has
     * exactly one of. Dropped, not rejected: every volume is the volume. */
    if (in[0] && in[1] == ':' &&
        ((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z')) &&
        (in[2] == '\\' || in[2] == '/'))
        in += 2;

    if (in[0] != '/' && in[0] != '\\' && o < max - 1) out[o++] = '/';
    for (int i = 0; in[i] && o < max - 1; i++)
        out[o++] = (in[i] == '\\') ? '/' : in[i];
    out[o] = '\0';
}

/*
 * Canonical form for comparison: absolute, single separators, no
 * trailing slash, and no `..` anywhere.
 *
 * `..` is refused rather than resolved, and the reason is that resolving
 * it here would be a guess about what the driver does with it. FAT32
 * directories carry real `.` and `..` entries on disk, so /home/alice/../bob
 * is a path fat_lookup() will genuinely walk to bob -- after a guard that
 * had already checked the string and seen only alice's name in it.
 *
 * Refusing it costs nothing, and that is worth stating precisely rather
 * than assuming, because `cd ..` is not a rare operation. The shell
 * flattens `..` in term_resolve() (term.h:78) before any path reaches
 * fs_*, and the file browser's up-arrow truncates exp_path at its last
 * slash (apps.h:131) rather than appending `/..`. Nothing in this system
 * hands a `..` to the filesystem layer, so the safe reading and the
 * correct one agree: refuse it.
 *
 * Returns 0 when the path cannot be made canonical.
 */
static int prof_canon(const char *in, char *out, int max) {
    char n[PROFILE_PATH_MAX];
    fs_native(in, n, sizeof(n));

    int o = 0;
    int i = 0;
    while (n[i]) {
        while (n[i] == '/') i++;              /* collapse // and /// */
        if (!n[i]) break;

        int start = i;
        while (n[i] && n[i] != '/') i++;
        int len = i - start;

        if (len == 2 && n[start] == '.' && n[start + 1] == '.') return 0;
        if (len == 1 && n[start] == '.') continue;   /* `.` is just here */

        if (o + len + 1 >= max) return 0;
        out[o++] = '/';
        for (int k = 0; k < len; k++) out[o++] = n[start + k];
    }
    if (o == 0) out[o++] = '/';               /* the root itself */
    out[o] = '\0';
    return 1;
}

/*
 * Is `canon` this prefix, or something inside it?
 *
 * Component-wise, which is the whole point: a plain string compare would
 * have `/Documents and Settings/al` match `/Documents and Settings/alice`
 * and hand account `al` everything alice owns. The character after the
 * prefix has to end the component.
 *
 * Case-insensitive via exf_upper(), to match exf_name_eq().
 */
static int prof_within(const char *canon, const char *prefix) {
    int i = 0;
    for (; prefix[i]; i++)
        if (exf_upper(canon[i]) != exf_upper(prefix[i])) return 0;
    return canon[i] == '\0' || canon[i] == '/';
}

/* Where this account's tree lives. The single place that knows. */
static void profile_home(const char *name, char *out, int max) {
    if (fs_kind == FS_EXFAT) str_copy(out, PROFILE_ROOT "/", max);
    else                     str_copy(out, PROFILE_LEGACY "/", max);
    str_append(out, name, max);
}

/*
 * Which account owns this path, or -1 if it belongs to no one.
 *
 * Both roots are checked for every account. A volume written before this
 * file existed has the person's real files under /home/<name>, and
 * leaving that unguarded would mean the new tree was private and
 * everything they already had was not.
 */
static int prof_owner_of(const char *canon) {
    for (int i = 0; i < user_count; i++) {
        char p[PROFILE_PATH_MAX];

        str_copy(p, PROFILE_ROOT "/", sizeof(p));
        str_append(p, users[i].name, sizeof(p));
        if (prof_within(canon, p)) return i;

        str_copy(p, PROFILE_LEGACY "/", sizeof(p));
        str_append(p, users[i].name, sizeof(p));
        if (prof_within(canon, p)) return i;
    }
    return -1;
}

/*
 * The guard. Called by every fs_* entry, for reads as well as writes --
 * listing someone's Desktop is reading it.
 *
 * Nobody signed in means yes. That is not a hole: users_load() reads
 * /etc/users.db before there is anyone to check against, the boot path
 * touches the volume long before the login screen, and the headless
 * harness builds never log in at all. term.h:975 already settled this
 * convention for administrator checks and this follows it.
 */
static int prof_may(const char *path, int write) {
    if (user_current < 0) return 1;
    if (user_is_admin(user_current)) return 1;

    char canon[PROFILE_PATH_MAX];
    if (!prof_canon(path, canon, sizeof(canon))) {
        fs_errstr = "that path cannot be resolved from here";
        return 0;
    }

    const int owner = prof_owner_of(canon);
    if (owner < 0 || owner == user_current) return 1;

    fs_errstr = write ? "that profile belongs to another account"
                      : "that profile is not yours to read";
    return 0;
}

/* ===== building a profile ===== */

/*
 * Idempotent: already-there counts as made.
 *
 * Which is what lets session_begin() call create_user_profile() on every
 * single login without it being a special case. An account made by an
 * older build has no tree; it gets one the next time its owner signs in,
 * and one that already has a tree costs four fs_stat() calls.
 */
static int prof_ensure_dir(const char *path) {
    int is_dir = 0;
    if (fs_stat(path, 0, &is_dir)) return is_dir;
    return fs_mkdir(path) == 0;
}

/*
 * Build \Documents and Settings\<username>\ and the subdirectories that
 * make it a profile rather than an empty folder.
 *
 * Returns the home directory itself. Null when the volume would not take
 * it -- a read-only boot with no disk attached, or a full one -- and
 * fs_errstr says which.
 */
static struct directory *create_user_profile(const char *username) {
    if (!username || !username[0]) {
        fs_errstr = "a profile needs a name";
        return 0;
    }

    char home[PROFILE_PATH_MAX];
    profile_home(username, home, sizeof(home));

    /* The container first. On FAT32 there is none -- /home is the
     * container -- and PROFILE_LEGACY is what profile_home() built. */
    if (!prof_ensure_dir(fs_kind == FS_EXFAT ? PROFILE_ROOT : PROFILE_LEGACY))
        return 0;
    if (!prof_ensure_dir(home)) return 0;

    /*
     * My Documents and Desktop, on the volume that can spell them. See
     * the note at PROFILE_ROOT for why FAT32 gets neither: 8.3 creation
     * would turn them into DOCUME~1 and DESKTOP, and a profile whose
     * folders are named differently depending on the filesystem is worse
     * than a profile that plainly does not have them there.
     */
    if (fs_kind == FS_EXFAT) {
        char sub[PROFILE_PATH_MAX];

        str_copy(sub, home, sizeof(sub));
        str_append(sub, "/" PROFILE_DOCS, sizeof(sub));
        if (!prof_ensure_dir(sub)) return 0;

        str_copy(sub, home, sizeof(sub));
        str_append(sub, "/" PROFILE_DESKTOP, sizeof(sub));
        if (!prof_ensure_dir(sub)) return 0;
    }

    /* Hand back a handle. Indexed by account so the same profile is the
     * same slot every time, and so a machine at USER_MAX accounts has
     * exactly USER_MAX of these and never needs another. */
    int idx = user_find(username);
    struct directory *d = &profile_dirs[idx >= 0 ? idx : 0];

    str_copy(d->path, home, sizeof(d->path));
    str_copy(d->name, username, sizeof(d->name));
    d->owner      = idx;
    d->is_dir     = 1;
    d->first_clus = 0;
    d->used       = 1;

    if (fs_kind == FS_EXFAT) {
        exf_dirent_t e;
        if (exf_lookup(home, &e)) d->first_clus = e.first_clus;
    }
    return d;
}

/* ===== the environment =====
 *
 * There was none. Not a thin one -- none at all: no getenv, no setenv,
 * no block anywhere in the tree, and PEB_PROCESS_PARAMS written as a
 * literal zero at desktop.h. "Lock the environment to the profile"
 * therefore starts by there being an environment to lock.
 *
 * Names compare case-insensitively, because these are Windows names and
 * %UserProfile% and %USERPROFILE% are the same variable.
 */
#define ENV_MAX        16
#define ENV_NAME_MAX   24
#define ENV_VALUE_MAX  PROFILE_PATH_MAX

typedef struct {
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
} env_var_t;

static env_var_t env_vars[ENV_MAX];
static int       env_count = 0;

static int env_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (exf_upper(*a) != exf_upper(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const char *env_get(const char *name) {
    for (int i = 0; i < env_count; i++)
        if (env_name_eq(env_vars[i].name, name)) return env_vars[i].value;
    return 0;
}

static int env_set(const char *name, const char *value) {
    for (int i = 0; i < env_count; i++)
        if (env_name_eq(env_vars[i].name, name)) {
            str_copy(env_vars[i].value, value, ENV_VALUE_MAX);
            return 0;
        }
    if (env_count >= ENV_MAX) return -1;
    str_copy(env_vars[env_count].name,  name,  ENV_NAME_MAX);
    str_copy(env_vars[env_count].value, value, ENV_VALUE_MAX);
    env_count++;
    return 0;
}

static void env_clear(void) {
    for (int i = 0; i < ENV_MAX; i++) {
        for (int k = 0; k < ENV_NAME_MAX; k++)  env_vars[i].name[k]  = '\0';
        for (int k = 0; k < ENV_VALUE_MAX; k++) env_vars[i].value[k] = '\0';
    }
    env_count = 0;
}

/*
 * Bind the environment to an account's profile.
 *
 * Both spellings are published, because both are read. USERPROFILE and
 * its relatives carry the Windows form -- drive letter, backslashes --
 * since that is what a program compiled for this target will concatenate
 * onto and hand to CreateFile; fs_native() at the top of this file is
 * what makes that string resolve. HOME carries the native form, because
 * that is what the shell and coreutils splice into paths.
 */
static void profile_bind_env(const char *name) {
    env_clear();

    char home[PROFILE_PATH_MAX];
    profile_home(name, home, sizeof(home));

    /* The Windows spelling of the same directory. */
    char win[PROFILE_PATH_MAX];
    int o = 0;
    for (int i = 0; home[i] && o < (int)sizeof(win) - 1; i++)
        win[o++] = (home[i] == '/') ? '\\' : home[i];
    win[o] = '\0';

    char drv[PROFILE_PATH_MAX];
    str_copy(drv, "C:", sizeof(drv));
    str_append(drv, win, sizeof(drv));

    env_set("USERNAME",    name);
    env_set("USERPROFILE", drv);
    env_set("HOMEDRIVE",   "C:");
    env_set("HOMEPATH",    win);
    env_set("HOME",        home);
    env_set("COMPUTERNAME", "VEXTRO");

    char sub[PROFILE_PATH_MAX];

    str_copy(sub, drv, sizeof(sub));
    str_append(sub, "\\" PROFILE_DOCS, sizeof(sub));
    env_set("APPDATA", sub);

    str_copy(sub, drv, sizeof(sub));
    str_append(sub, "\\" PROFILE_DESKTOP, sizeof(sub));
    env_set("DESKTOP", sub);

    str_copy(sub, drv, sizeof(sub));
    str_append(sub, "\\Temp", sizeof(sub));
    env_set("TEMP", sub);
    env_set("TMP",  sub);
}

/*
 * The environment as a Windows process expects to receive it: UTF-16LE
 * "NAME=VALUE\0" one after another, the run closed by a second NUL.
 *
 * Written into `out` as bytes; returns how many were used, or 0 if they
 * would not fit. Only the low byte of each unit is ever set -- every
 * character in a profile path here is ASCII by construction, since
 * user_name_ok() admits nothing else and the rest is our own spelling.
 */
static uint32_t env_build_utf16(uint8_t *out, uint32_t cap) {
    uint32_t o = 0;

    for (int i = 0; i < env_count; i++) {
        const char *parts[3] = { env_vars[i].name, "=", env_vars[i].value };
        for (int p = 0; p < 3; p++)
            for (int k = 0; parts[p][k]; k++) {
                if (o + 4 > cap) return 0;      /* room for this and the run's end */
                out[o++] = (uint8_t)parts[p][k];
                out[o++] = 0;
            }
        if (o + 4 > cap) return 0;
        out[o++] = 0; out[o++] = 0;             /* end of this variable */
    }

    if (o + 2 > cap) return 0;
    out[o++] = 0; out[o++] = 0;                 /* end of the block */
    return o;
}

#endif /* PROFILE_H */
