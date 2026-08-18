#ifndef FSMETA_H
#define FSMETA_H

/*
 * src/fsmeta.h — the things a filesystem needs that are not storage.
 *
 * Reading and writing bytes is the part of a filesystem that is
 * obviously a filesystem. What sits on top of it is most of what makes
 * one usable and none of it was here:
 *
 *   who may open a file            security descriptors and identifiers
 *   who has it open already        lock handles
 *   what it is called in DOS       generated 8.3 names
 *   whether "README" and "readme"  case-insensitive matching that
 *   are the same file              nonetheless preserves what was typed
 *   who to tell when it changes    directory change notifications
 *   what to undo after a crash     an intent log
 *
 * All six are policy over a byte store, which is why they are one file
 * rather than six patches into exfat.h — and why they work the same
 * whichever filesystem is mounted underneath.
 */

#include <stdint.h>
#include "kheap.h"

/* ===== CASE =====
 *
 * Windows matches names without regard to case and displays them
 * exactly as they were created. Those two requirements pull in opposite
 * directions and the resolution is the one below: fold only for
 * comparison, never for storage.
 *
 * Folding is ASCII only. Correct case folding over Unicode is a table of
 * a few thousand entries and a set of rules about Turkish dotted I; the
 * names this system creates are ASCII, and a non-ASCII byte compares
 * equal only to itself, which is wrong in a way nothing here can
 * observe and honest about being so.
 */
static inline char fs_fold(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int fs_name_eq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = fs_fold(*a), cb = fs_fold(*b);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static int fs_name_eq_n_ci(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = fs_fold(a[i]), cb = fs_fold(b[i]);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

/* ===== 8.3 SHORT NAMES =====
 *
 * "Documents and Settings" becomes DOCUME~1. The rule is older than most
 * of the software that still depends on it: six characters of the name
 * with everything illegal removed, a tilde, and a number that makes it
 * unique in its directory.
 *
 * The number is the interesting part. It is not a hash — it is a
 * sequence, assigned by trying 1 and counting up until nothing else in
 * the directory has that short name, which means the same long name can
 * get a different short one in a different directory. Anything that
 * stores a short name and expects it to be stable is already wrong;
 * generating them deterministically per directory is what Windows does
 * and what this does.
 */
static int fs_is_short_legal(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_' || c == '-' || c == '$' || c == '%' || c == '\'' ||
           c == '@' || c == '~' || c == '!' || c == '(' || c == ')' ||
           c == '{' || c == '}' || c == '^' || c == '#' || c == '&';
}

/* Does this name already fit 8.3 exactly, needing no generation? */
static int fs_fits_short(const char *name) {
    int base = 0, ext = 0, dot = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') return 0;      /* lower case: no */
        if (c == '.') {
            if (dot) return 0;                   /* only one allowed */
            dot = 1;
            continue;
        }
        if (!fs_is_short_legal(c)) return 0;
        if (dot) ext++; else base++;
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

/*
 * Build the short name for `name`, using `taken` to decide the number.
 * `taken` answers "does this short name already exist here?" and is the
 * caller's, because only the caller knows the directory.
 */
static void fs_short_name(const char *name, char *out,
                          int (*taken)(const char *, void *), void *ctx) {
    if (fs_fits_short(name)) {
        int i = 0;
        while (name[i] && i < 12) { out[i] = name[i]; i++; }
        out[i] = '\0';
        return;
    }

    /* The extension is what follows the *last* dot, up to three
     * characters, upper-cased and stripped of anything illegal. */
    int last_dot = -1;
    int len = 0;
    while (name[len]) { if (name[len] == '.') last_dot = len; len++; }

    char ext[4];
    int e = 0;
    if (last_dot >= 0)
        for (int i = last_dot + 1; name[i] && e < 3; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (fs_is_short_legal(c)) ext[e++] = c;
        }
    ext[e] = '\0';

    char base[7];
    int b = 0;
    for (int i = 0; name[i] && (last_dot < 0 || i < last_dot) && b < 6; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == ' ' || c == '.') continue;      /* dropped, not replaced */
        if (!fs_is_short_legal(c)) c = '_';
        base[b++] = c;
    }
    if (b == 0) { base[b++] = 'F'; base[b++] = 'I'; base[b++] = 'L'; }
    base[b] = '\0';

    for (int n = 1; n < 1000000; n++) {
        char num[8];
        int d = 0, v = n;
        char rev[8];
        do { rev[d++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (int i = 0; i < d; i++) num[i] = rev[d - 1 - i];
        num[d] = '\0';

        /* The base is trimmed so that base + '~' + digits is eight. */
        int keep = 8 - 1 - d;
        if (keep > b) keep = b;
        if (keep < 1) keep = 1;

        int o = 0;
        for (int i = 0; i < keep; i++) out[o++] = base[i];
        out[o++] = '~';
        for (int i = 0; i < d; i++) out[o++] = num[i];
        if (e) { out[o++] = '.'; for (int i = 0; i < e; i++) out[o++] = ext[i]; }
        out[o] = '\0';

        if (!taken || !taken(out, ctx)) return;
    }
}

/* ===== SECURITY IDENTIFIERS =====
 *
 * A SID is an authority and a list of sub-authorities, and it is
 * deliberately not a small integer: the point is that it is unique
 * across machines that have never met, so a file carried from one to
 * another still says who owned it there.
 *
 * The well-known ones below are the same on every Windows machine ever
 * made, which is why they can be constants here.
 */
#define SID_MAX_SUB 8

typedef struct {
    uint8_t  revision;
    uint8_t  sub_count;
    uint8_t  authority[6];
    uint32_t sub[SID_MAX_SUB];
} sid_t;

/* S-1-5-18 local system, S-1-5-32-544 administrators, S-1-1-0 everyone */
static const sid_t SID_SYSTEM = { 1, 1, {0,0,0,0,0,5}, {18} };
static const sid_t SID_ADMINS = { 1, 2, {0,0,0,0,0,5}, {32, 544} };
static const sid_t SID_EVERYONE = { 1, 1, {0,0,0,0,0,1}, {0} };

static int sid_eq(const sid_t *a, const sid_t *b) {
    if (a->revision != b->revision || a->sub_count != b->sub_count) return 0;
    for (int i = 0; i < 6; i++) if (a->authority[i] != b->authority[i]) return 0;
    for (int i = 0; i < a->sub_count; i++) if (a->sub[i] != b->sub[i]) return 0;
    return 1;
}

/* A user's SID is the machine authority with the account index below
 * it, which is how a local account gets one without a domain. */
static void sid_for_user(int index, sid_t *out) {
    out->revision = 1;
    out->sub_count = 2;
    for (int i = 0; i < 6; i++) out->authority[i] = 0;
    out->authority[5] = 5;
    out->sub[0] = 21;                     /* "not built in" */
    out->sub[1] = 1000u + (uint32_t)index;
}

static void sid_to_text(const sid_t *s, char *out, int cap) {
    int o = 0;
    const char *p = "S-1-";
    while (*p && o < cap - 1) out[o++] = *p++;

    uint64_t auth = 0;
    for (int i = 0; i < 6; i++) auth = (auth << 8) | s->authority[i];

    char tmp[24];
    int d = 0;
    uint64_t v = auth;
    do { tmp[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (d && o < cap - 1) out[o++] = tmp[--d];

    for (int i = 0; i < s->sub_count && o < cap - 3; i++) {
        out[o++] = '-';
        uint32_t x = s->sub[i];
        d = 0;
        do { tmp[d++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (d && o < cap - 1) out[o++] = tmp[--d];
    }
    out[o] = '\0';
}

/* ===== ACCESS CONTROL =====
 *
 * An entry says which SID gets which rights and whether it is a grant or
 * a denial. Order matters and is not a detail: denials are evaluated
 * first, so a user who is denied by one entry cannot be granted by
 * another later in the list. Getting that backwards produces a system
 * where adding a permission silently removes a restriction.
 */
#define ACE_READ    0x01
#define ACE_WRITE   0x02
#define ACE_EXECUTE 0x04
#define ACE_DELETE  0x08
#define ACE_ALL     0x0F

#define ACE_ALLOW 0
#define ACE_DENY  1

typedef struct {
    sid_t   who;
    uint8_t type;
    uint8_t rights;
} ace_t;

#define ACL_MAX_ENTRIES 8

typedef struct {
    sid_t   owner;
    sid_t   group;
    ace_t   ace[ACL_MAX_ENTRIES];
    uint8_t count;
} security_descriptor_t;

static void sd_default(security_descriptor_t *sd, const sid_t *owner) {
    sd->owner = *owner;
    sd->group = SID_ADMINS;
    sd->count = 0;
    /* Owner: everything. Administrators: everything. Everyone: read. */
    sd->ace[sd->count].who = *owner;
    sd->ace[sd->count].type = ACE_ALLOW;
    sd->ace[sd->count++].rights = ACE_ALL;
    sd->ace[sd->count].who = SID_ADMINS;
    sd->ace[sd->count].type = ACE_ALLOW;
    sd->ace[sd->count++].rights = ACE_ALL;
    sd->ace[sd->count].who = SID_EVERYONE;
    sd->ace[sd->count].type = ACE_ALLOW;
    sd->ace[sd->count++].rights = ACE_READ;
}

static int sd_permits(const security_descriptor_t *sd, const sid_t *who,
                      uint8_t wanted) {
    /* Denials first, and a single matching denial settles it. */
    for (int i = 0; i < sd->count; i++) {
        const ace_t *a = &sd->ace[i];
        if (a->type != ACE_DENY) continue;
        if (!sid_eq(&a->who, who) && !sid_eq(&a->who, &SID_EVERYONE)) continue;
        if (a->rights & wanted) return 0;
    }
    uint8_t granted = 0;
    for (int i = 0; i < sd->count; i++) {
        const ace_t *a = &sd->ace[i];
        if (a->type != ACE_ALLOW) continue;
        if (!sid_eq(&a->who, who) && !sid_eq(&a->who, &SID_EVERYONE)) continue;
        granted |= a->rights;
    }
    return (granted & wanted) == wanted;
}

/* ===== OPEN FILE HANDLES =====
 *
 * "The file is in use by another program" is a sentence a system can
 * only say if it knows. A handle records who has a path open and what
 * they are prepared to let others do with it at the same time — which is
 * the share mode, and is why two programs can both read a log while
 * neither may write it.
 */
#define FS_MAX_HANDLES 32
#define FS_PATH_LEN    96

#define SHARE_READ   0x01
#define SHARE_WRITE  0x02
#define SHARE_DELETE 0x04

typedef struct {
    int      used;
    char     path[FS_PATH_LEN];
    uint32_t owner_pid;
    uint8_t  access;             /* what the holder is doing         */
    uint8_t  share;              /* what the holder permits others   */
} fs_handle_t;

static fs_handle_t fs_handles[FS_MAX_HANDLES];
static int fs_handle_conflicts = 0;

static int fs_path_eq(const char *a, const char *b) {
    return fs_name_eq_ci(a, b);
}

/*
 * Open, or say why not.
 *
 * Returns a handle index, or -1 with the reason recorded. The check is
 * symmetric and has to be: the newcomer must be permitted by every
 * existing holder's share mode, *and* must itself permit what every
 * existing holder is already doing.
 */
static const char *fs_open_error = "";

static int fs_open_handle(const char *path, uint32_t pid,
                          uint8_t access, uint8_t share) {
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        fs_handle_t *h = &fs_handles[i];
        if (!h->used || !fs_path_eq(h->path, path)) continue;

        if ((access & ACE_WRITE) && !(h->share & SHARE_WRITE)) {
            fs_open_error = "already open by a program that forbids writing";
            fs_handle_conflicts++;
            return -1;
        }
        if ((access & ACE_READ) && !(h->share & SHARE_READ)) {
            fs_open_error = "already open by a program that forbids reading";
            fs_handle_conflicts++;
            return -1;
        }
        if ((h->access & ACE_WRITE) && !(share & SHARE_WRITE)) {
            fs_open_error = "another program is writing it";
            fs_handle_conflicts++;
            return -1;
        }
    }

    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        fs_handle_t *h = &fs_handles[i];
        if (h->used) continue;
        int n = 0;
        while (path[n] && n < FS_PATH_LEN - 1) { h->path[n] = path[n]; n++; }
        h->path[n] = '\0';
        h->used = 1;
        h->owner_pid = pid;
        h->access = access;
        h->share = share;
        return i;
    }
    fs_open_error = "too many open files";
    return -1;
}

static void fs_close_handle(int idx) {
    if (idx >= 0 && idx < FS_MAX_HANDLES) fs_handles[idx].used = 0;
}

/* A program that dies with files open must not hold them forever. */
static void fs_close_pid(uint32_t pid) {
    for (int i = 0; i < FS_MAX_HANDLES; i++)
        if (fs_handles[i].used && fs_handles[i].owner_pid == pid)
            fs_handles[i].used = 0;
}

static int fs_is_open(const char *path) {
    for (int i = 0; i < FS_MAX_HANDLES; i++)
        if (fs_handles[i].used && fs_path_eq(fs_handles[i].path, path))
            return 1;
    return 0;
}

/* ===== DIRECTORY CHANGE NOTIFICATION =====
 *
 * A file manager showing a folder has two options: ask again every so
 * often, or be told. Polling a directory four times a second is most of
 * what a naive file manager costs, and it is still a quarter of a second
 * behind.
 *
 * A watch is a path and a mask; a change raises a sequence number the
 * watcher compares against what it last saw. That is deliberately not a
 * callback: a callback would run inside whatever was writing the file,
 * at whatever privilege it had, and the watcher here is the compositor,
 * which must not be re-entered.
 */
#define FS_MAX_WATCH 8

#define FS_CHANGE_ADDED    0x01
#define FS_CHANGE_REMOVED  0x02
#define FS_CHANGE_MODIFIED 0x04
#define FS_CHANGE_RENAMED  0x08
#define FS_CHANGE_ANY      0x0F

typedef struct {
    int      used;
    char     path[FS_PATH_LEN];
    uint8_t  mask;
    uint32_t sequence;           /* raised on every matching change */
    char     last[FS_PATH_LEN];  /* what changed, most recently     */
    uint8_t  last_action;
} fs_watch_t;

static fs_watch_t fs_watches[FS_MAX_WATCH];

static int fs_watch_add(const char *dir, uint8_t mask) {
    for (int i = 0; i < FS_MAX_WATCH; i++) {
        if (fs_watches[i].used) continue;
        int n = 0;
        while (dir[n] && n < FS_PATH_LEN - 1) { fs_watches[i].path[n] = dir[n]; n++; }
        fs_watches[i].path[n] = '\0';
        fs_watches[i].used = 1;
        fs_watches[i].mask = mask;
        fs_watches[i].sequence = 0;
        fs_watches[i].last[0] = '\0';
        return i;
    }
    return -1;
}

static void fs_watch_remove(int idx) {
    if (idx >= 0 && idx < FS_MAX_WATCH) fs_watches[idx].used = 0;
}

/* Is `path` inside the directory `dir`? Directory-only, not recursive:
 * a watcher of /home should not be woken by every write under it. */
static int fs_path_in_dir(const char *path, const char *dir) {
    int i = 0;
    while (dir[i] && fs_fold(path[i]) == fs_fold(dir[i])) i++;
    if (dir[i]) return 0;
    if (i > 1 && path[i] != '/') return 0;
    const char *rest = path + (path[i] == '/' ? i + 1 : i);
    for (const char *p = rest; *p; p++) if (*p == '/') return 0;
    return 1;
}

/* Called by every path that alters the volume. */
static void fs_notify(const char *path, uint8_t action) {
    for (int i = 0; i < FS_MAX_WATCH; i++) {
        fs_watch_t *w = &fs_watches[i];
        if (!w->used || !(w->mask & action)) continue;
        if (!fs_path_in_dir(path, w->path)) continue;
        w->sequence++;
        w->last_action = action;
        int n = 0;
        while (path[n] && n < FS_PATH_LEN - 1) { w->last[n] = path[n]; n++; }
        w->last[n] = '\0';
    }
}

/* ===== THE INTENT LOG =====
 *
 * A write that is really several writes -- extend the file, update its
 * length, mark the clusters used -- must not be observable half done.
 * The oldest correct answer is to write down what is about to happen
 * before doing it, so that a machine which stops in the middle can be
 * told on the next boot what it was in the middle of.
 *
 * This records intent and completion in memory and flushes the record to
 * the volume. It is not a redo log: it cannot replay a lost write,
 * because replaying needs the data and the data is what was lost. What
 * it can do is name the file that was being written when the power went,
 * which is the difference between a filesystem check that scans
 * everything and one that looks in exactly one place.
 */
#define FS_JOURNAL_ENTRIES 32

#define JRN_NONE   0
#define JRN_CREATE 1
#define JRN_WRITE  2
#define JRN_DELETE 3
#define JRN_RENAME 4

typedef struct {
    uint32_t sequence;
    uint8_t  op;
    uint8_t  committed;
    uint16_t pad;
    char     path[FS_PATH_LEN];
} __attribute__((packed)) fs_journal_entry_t;

static fs_journal_entry_t fs_journal[FS_JOURNAL_ENTRIES];
static uint32_t fs_journal_seq = 0;
static int      fs_journal_head = 0;
static int      fs_journal_open = 0;

static int fs_journal_begin(uint8_t op, const char *path) {
    int idx = fs_journal_head;
    fs_journal_entry_t *e = &fs_journal[idx];
    e->sequence  = ++fs_journal_seq;
    e->op        = op;
    e->committed = 0;
    int n = 0;
    while (path[n] && n < FS_PATH_LEN - 1) { e->path[n] = path[n]; n++; }
    e->path[n] = '\0';
    fs_journal_head = (fs_journal_head + 1) % FS_JOURNAL_ENTRIES;
    fs_journal_open++;
    return idx;
}

static void fs_journal_commit(int idx) {
    if (idx < 0 || idx >= FS_JOURNAL_ENTRIES) return;
    fs_journal[idx].committed = 1;
    if (fs_journal_open) fs_journal_open--;
}

/* What was in flight when the machine stopped. */
static int fs_journal_incomplete(char *out, int cap) {
    int n = 0;
    for (int i = 0; i < FS_JOURNAL_ENTRIES; i++) {
        if (!fs_journal[i].sequence || fs_journal[i].committed) continue;
        n++;
        if (out && cap > 1 && n == 1) {
            int k = 0;
            while (fs_journal[i].path[k] && k < cap - 1) {
                out[k] = fs_journal[i].path[k];
                k++;
            }
            out[k] = '\0';
        }
    }
    return n;
}

#endif /* FSMETA_H */
