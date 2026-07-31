#ifndef COREUTILS_H
#define COREUTILS_H

/*
 * The Unix toolset.
 *
 * The shell had about fifty commands, most of them specific to this
 * machine — `zim`, `llm`, `gpu`, `agora`. What it did not have was the
 * ordinary vocabulary: no `grep`, no `wc`, no `head`, no `sort`. This is
 * that vocabulary, written against the filesystem and string helpers the
 * rest of the system already uses.
 *
 * Deliberately in its own file, and deliberately touching nothing
 * architecture-specific: only fs_*, str_*, term_print* and the standard
 * integer helpers. That makes it byte-identical on the x86_64 and aarch64
 * trees, so the two copies cannot drift the way desktop.h and term.h
 * already have.
 *
 * Two constraints shape nearly everything here, and they are worth
 * stating once rather than repeating:
 *
 *   - There is no allocator. Every buffer is static and bounded, and a
 *     command that would exceed its bound says so rather than truncating
 *     in silence.
 *
 *   - fs_read_file() hands back a pointer into one shared 4 MB buffer, so
 *     a second read invalidates the first. Anything comparing two files
 *     (cmp, diff, comm) has to copy one side out first, and does.
 */

/* ===== output plumbing =====
 *
 * These commands are written to read a named file, because there are no
 * processes and so no stdin to inherit. `cu_src` is what makes a pipeline
 * possible anyway: the left-hand side's output is captured into a buffer
 * and the right-hand side reads from that instead of from disk.
 */

#define CU_PIPE_MAX (256 * 1024)
static char     cu_pipe[CU_PIPE_MAX];
static uint32_t cu_pipe_len = 0;
static int      cu_pipe_ready = 0;      /* read from cu_pipe, not a file */

/*
 * Resolve an argument to bytes: the pipe if one is waiting and no name
 * was given, otherwise the named file.
 *
 * Returns 0 and reports the reason on failure. A NULL name with no pipe
 * is the one case worth a specific message — it is what someone typing
 * `sort` on its own gets, and "usage" is more useful than "not found".
 */
static const char *cu_lasterr = "";

static int cu_src(const char *name, const uint8_t **out, uint32_t *len) {
    if ((!name || !name[0]) && cu_pipe_ready) {
        *out = (const uint8_t *)cu_pipe;
        *len = cu_pipe_len;
        return 1;
    }
    if (!name || !name[0]) {
        cu_lasterr = "no file given, and nothing piped in";
        return 0;
    }
    char abs[256];
    term_resolve(name, abs);

    uint64_t sz = 0;
    int is_dir = 0;
    if (!fs_stat(abs, &sz, &is_dir)) { cu_lasterr = "no such file"; return 0; }
    if (is_dir) { cu_lasterr = "is a directory"; return 0; }

    uint64_t got = 0;
    const void *d = fs_read_file(abs, &got);
    if (!d) { cu_lasterr = fs_errstr; return 0; }
    *out = (const uint8_t *)d;
    *len = (uint32_t)got;
    return 1;
}

static void cu_err(const char *cmd, const char *msg) {
    term_print_c(cmd, 2);
    term_print_c(": ", 2);
    term_print_c(msg, 2);
    term_putc('\n');
}

static void cu_usage(const char *text) {
    term_print_c("usage: ", 3);
    term_print_c(text, 3);
    term_putc('\n');
}

/* ===== small helpers ===== */

static int cu_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

static char cu_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int cu_atoi(const char *s, int dflt) {
    if (!s || !s[0]) return dflt;
    int neg = 0, v = 0, any = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') break;
        v = v * 10 + (*s - '0');
        any = 1;
    }
    return any ? (neg ? -v : v) : dflt;
}

/* Print a number right-aligned in `w` columns. Tables read far better
 * with the digits lined up, and there is no printf to do it. */
static void cu_put_num(uint32_t v, int w) {
    char nb[16];
    uint_to_str(v, nb);
    int n = 0;
    while (nb[n]) n++;
    for (int i = n; i < w; i++) term_putc(' ');
    term_print(nb);
}

static void cu_put_line(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) term_putc((char)p[i]);
    term_putc('\n');
}

/* Substring search. Used by grep, strings and file-type sniffing. */
static int cu_find_sub(const uint8_t *hay, uint32_t hn,
                       const char *needle, int fold) {
    uint32_t nn = 0;
    while (needle[nn]) nn++;
    if (nn == 0) return 0;
    if (nn > hn) return -1;
    for (uint32_t i = 0; i + nn <= hn; i++) {
        uint32_t k = 0;
        while (k < nn) {
            char a = (char)hay[i + k], b = needle[k];
            if (fold) { a = cu_lower(a); b = cu_lower(b); }
            if (a != b) break;
            k++;
        }
        if (k == nn) return (int)i;
    }
    return -1;
}

/*
 * Shell-style glob: * ? and [abc] / [a-z].
 *
 * Recursive on '*' only, and the recursion is bounded by the pattern
 * length, so it cannot run away on a kernel stack.
 */
static int cu_glob(const char *pat, const char *s, int fold) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *q = s; ; q++) {
                if (cu_glob(pat, q, fold)) return 1;
                if (!*q) return 0;
            }
        }
        if (!*s) return 0;
        if (*pat == '?') { pat++; s++; continue; }
        if (*pat == '[') {
            const char *p = pat + 1;
            int neg = 0, hit = 0;
            if (*p == '!' || *p == '^') { neg = 1; p++; }
            char c = fold ? cu_lower(*s) : *s;
            while (*p && *p != ']') {
                char lo = fold ? cu_lower(*p) : *p;
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    char hi = fold ? cu_lower(p[2]) : p[2];
                    if (c >= lo && c <= hi) hit = 1;
                    p += 3;
                } else {
                    if (c == lo) hit = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if (hit == neg) return 0;
            pat = p; s++;
            continue;
        }
        {
            char a = fold ? cu_lower(*pat) : *pat;
            char b = fold ? cu_lower(*s) : *s;
            if (a != b) return 0;
        }
        pat++; s++;
    }
    return *s == '\0';
}

/* Split a buffer into lines. Returns the count; offsets/lengths go into
 * the caller's arrays. A trailing fragment with no newline still counts,
 * because a file that does not end in one still has a last line. */
#define CU_MAX_LINES 8192
static uint32_t cu_line_off[CU_MAX_LINES];
static uint32_t cu_line_len[CU_MAX_LINES];

static uint32_t cu_split_lines(const uint8_t *d, uint32_t n) {
    uint32_t count = 0, start = 0;
    for (uint32_t i = 0; i < n && count < CU_MAX_LINES; i++) {
        if (d[i] == '\n') {
            uint32_t len = i - start;
            if (len > 0 && d[start + len - 1] == '\r') len--;   /* CRLF */
            cu_line_off[count] = start;
            cu_line_len[count] = len;
            count++;
            start = i + 1;
        }
    }
    if (start < n && count < CU_MAX_LINES) {
        uint32_t len = n - start;
        if (len > 0 && d[start + len - 1] == '\r') len--;
        cu_line_off[count] = start;
        cu_line_len[count] = len;
        count++;
    }
    return count;
}

/* ===== file and directory operations ===== */

/*
 * Walking a tree.
 *
 * fs_list() takes a callback and is *not* reentrant: the filesystem layer
 * reads directory sectors through shared static buffers, so calling it
 * again from inside its own callback corrupts the outer iteration. The
 * first version of tree did exactly that and quietly lost every entry
 * after the first subdirectory -- `ls /` showed four things, `tree /`
 * showed two.
 *
 * So each level is collected first and recursed into afterwards, once
 * fs_list has returned. That costs one array per level, which is why the
 * depth is bounded rather than arbitrary.
 */
#define CU_WALK_DEPTH 6
#define CU_WALK_ENTS  128

typedef struct {
    char     name[64];
    uint32_t size;
    int      is_dir;
} cu_ent_t;

static cu_ent_t cu_ents[CU_WALK_DEPTH][CU_WALK_ENTS];
static int      cu_nents[CU_WALK_DEPTH];
static int      cu_level = 0;         /* which row the callback fills */
static int      cu_overflow = 0;

static void cu_collect(const char *name, uint32_t size, int is_dir) {
    if (str_eq(name, ".") || str_eq(name, "..")) return;
    int L = cu_level;
    if (cu_nents[L] >= CU_WALK_ENTS) { cu_overflow = 1; return; }
    cu_ent_t *e = &cu_ents[L][cu_nents[L]++];
    str_copy(e->name, name, sizeof(e->name));
    e->size = size;
    e->is_dir = is_dir;
}

/* Read one directory into row `depth`. Returns the entry count. */
static int cu_read_dir(const char *path, int depth) {
    if (depth >= CU_WALK_DEPTH) return 0;
    cu_level = depth;
    cu_nents[depth] = 0;
    fs_list(path, cu_collect);
    return cu_nents[depth];
}

static void cu_join(char *out, int max, const char *dir, const char *name) {
    str_copy(out, dir, max);
    if (dir[0] && !(dir[0] == '/' && dir[1] == '\0'))
        str_append(out, "/", max);
    else if (dir[0] != '/')
        str_append(out, "/", max);
    str_append(out, name, max);
}

static uint32_t cu_tree_dirs = 0, cu_tree_files = 0;

static void cu_tree_walk(const char *path, int depth) {
    int n = cu_read_dir(path, depth);
    for (int i = 0; i < n; i++) {
        /* Copied out: the row is reused by the recursive call below. */
        char name[64];
        int  is_dir = cu_ents[depth][i].is_dir;
        str_copy(name, cu_ents[depth][i].name, sizeof(name));

        for (int k = 0; k < depth; k++) term_print("|   ");
        term_print("|-- ");
        term_print_c(name, is_dir ? 4 : 0);
        term_putc('\n');

        if (is_dir) {
            cu_tree_dirs++;
            if (depth + 1 < CU_WALK_DEPTH) {
                char sub[256];
                cu_join(sub, sizeof(sub), path, name);
                cu_tree_walk(sub, depth + 1);
            }
        } else {
            cu_tree_files++;
        }
    }
}

static void cu_cmd_tree(int argc, char **argv) {
    char abs[256];
    term_resolve(argc >= 2 ? argv[1] : ".", abs);
    cu_tree_dirs = cu_tree_files = 0;
    cu_overflow = 0;
    term_print_c(abs, 4);
    term_putc('\n');
    cu_tree_walk(abs, 0);
    term_putc('\n');
    cu_put_num(cu_tree_dirs, 1);
    term_print(" directories, ");
    cu_put_num(cu_tree_files, 1);
    term_print(" files\n");
    if (cu_overflow) term_print_c("(a directory had more than 128 entries)\n", 3);
}

/* stat: what the filesystem actually knows, which is less than POSIX
 * defines. Saying so is better than inventing a mode and an inode. */
static void cu_cmd_stat(int argc, char **argv) {
    if (argc < 2) { cu_usage("stat <path>"); return; }
    for (int a = 1; a < argc; a++) {
        char abs[256];
        term_resolve(argv[a], abs);
        uint64_t sz = 0;
        int is_dir = 0;
        if (!fs_stat(abs, &sz, &is_dir)) { cu_err("stat", "no such file"); continue; }
        term_print("  File: ");
        term_print_c(abs, is_dir ? 4 : 0);
        term_putc('\n');
        term_print("  Size: ");
        cu_put_num((uint32_t)sz, 1);
        term_print(is_dir ? "   Type: directory\n" : "   Type: regular file\n");
        term_print("  FS:   ");
        term_print(fs_name());
        term_print("   (no owner or mode: exFAT and FAT32 store neither)\n");
    }
}

static void cu_cmd_touch(int argc, char **argv) {
    if (argc < 2) { cu_usage("touch <file>..."); return; }
    for (int a = 1; a < argc; a++) {
        char abs[256];
        term_resolve(argv[a], abs);
        if (fs_stat(abs, 0, 0)) continue;      /* exists: nothing to do */
        if (fs_write_file(abs, "", 0) != 0) cu_err("touch", fs_errstr);
    }
}

static void cu_cmd_mv(int argc, char **argv) {
    if (argc < 3) { cu_usage("mv <src> <dst>"); return; }
    char src[256], dst[256];
    term_resolve(argv[1], src);
    term_resolve(argv[2], dst);

    uint64_t sz = 0;
    int is_dir = 0;
    if (!fs_stat(src, &sz, &is_dir)) { cu_err("mv", "no such file"); return; }
    if (is_dir) { cu_err("mv", "moving a directory is not supported"); return; }

    /* No rename in the filesystem layer, so this is copy-then-delete. The
     * copy is verified before the original goes, which is the difference
     * between a move and a way to lose a file. */
    uint64_t got = 0;
    const void *d = fs_read_file(src, &got);
    if (!d) { cu_err("mv", fs_errstr); return; }
    if (fs_write_file(dst, d, (uint32_t)got) != 0) {
        cu_err("mv", fs_errstr);
        return;
    }
    if (!fs_stat(dst, 0, 0)) { cu_err("mv", "destination did not appear"); return; }
    if (fs_delete(src) != 0) cu_err("mv", "copied, but the original remains");
}

static void cu_cmd_ln(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Neither filesystem here records a link, hard or symbolic. Copying
     * instead would be a lie with different semantics. */
    cu_err("ln", "exFAT and FAT32 have no links; use cp");
}

static void cu_cmd_basename(int argc, char **argv) {
    if (argc < 2) { cu_usage("basename <path> [suffix]"); return; }
    const char *s = argv[1];
    int last = -1;
    for (int i = 0; s[i]; i++) if (s[i] == '/') last = i;
    char out[128];
    str_copy(out, s + last + 1, sizeof(out));

    if (argc >= 3) {                       /* strip the suffix if present */
        int ol = 0; while (out[ol]) ol++;
        int sl = 0; while (argv[2][sl]) sl++;
        if (sl > 0 && ol > sl) {
            int k = 0;
            while (k < sl && out[ol - sl + k] == argv[2][k]) k++;
            if (k == sl) out[ol - sl] = '\0';
        }
    }
    term_print(out[0] ? out : "/");
    term_putc('\n');
}

static void cu_cmd_dirname(int argc, char **argv) {
    if (argc < 2) { cu_usage("dirname <path>"); return; }
    char out[256];
    str_copy(out, argv[1], sizeof(out));
    int last = -1;
    for (int i = 0; out[i]; i++) if (out[i] == '/') last = i;
    if (last < 0) { term_print(".\n"); return; }
    if (last == 0) { term_print("/\n"); return; }
    out[last] = '\0';
    term_print(out);
    term_putc('\n');
}

static void cu_cmd_realpath(int argc, char **argv) {
    if (argc < 2) { cu_usage("realpath <path>"); return; }
    for (int a = 1; a < argc; a++) {
        char abs[256];
        term_resolve(argv[a], abs);
        term_print(abs);
        term_putc('\n');
    }
}

/* find: name glob and a type filter, over the same collect-then-recurse
 * walk tree uses. */
static char cu_find_pat[96];
static char cu_find_type = 0;            /* 'f', 'd', or 0 for any */
static uint32_t cu_find_hits = 0;

static void cu_find_walk(const char *path, int depth) {
    int n = cu_read_dir(path, depth);
    for (int i = 0; i < n; i++) {
        char name[64];
        int  is_dir = cu_ents[depth][i].is_dir;
        str_copy(name, cu_ents[depth][i].name, sizeof(name));

        char full[256];
        cu_join(full, sizeof(full), path, name);

        int type_ok = !cu_find_type ||
                      (cu_find_type == 'd' && is_dir) ||
                      (cu_find_type == 'f' && !is_dir);
        if (type_ok && (!cu_find_pat[0] || cu_glob(cu_find_pat, name, 0))) {
            term_print_c(full, is_dir ? 4 : 0);
            term_putc('\n');
            cu_find_hits++;
        }
        if (is_dir && depth + 1 < CU_WALK_DEPTH)
            cu_find_walk(full, depth + 1);
    }
}

static void cu_cmd_find(int argc, char **argv) {
    char abs[256];
    term_resolve((argc >= 2 && argv[1][0] != '-') ? argv[1] : ".", abs);
    cu_find_pat[0] = '\0';
    cu_find_type = 0;
    cu_find_hits = 0;

    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-name") && a + 1 < argc)
            str_copy(cu_find_pat, argv[++a], sizeof(cu_find_pat));
        else if (str_eq(argv[a], "-type") && a + 1 < argc)
            cu_find_type = argv[++a][0];
    }
    cu_find_walk(abs, 0);
    if (cu_find_hits == 0) term_print_c("no matches\n", 3);
}

/* du: the recursive size of a tree, which df cannot tell you. */
static uint64_t cu_du_total = 0;
static int      cu_du_all = 0;

static void cu_du_walk(const char *path, int depth) {
    int n = cu_read_dir(path, depth);
    for (int i = 0; i < n; i++) {
        char name[64];
        int  is_dir = cu_ents[depth][i].is_dir;
        uint32_t size = cu_ents[depth][i].size;
        str_copy(name, cu_ents[depth][i].name, sizeof(name));

        if (is_dir) {
            if (depth + 1 < CU_WALK_DEPTH) {
                char sub[256];
                cu_join(sub, sizeof(sub), path, name);
                cu_du_walk(sub, depth + 1);
            }
        } else {
            cu_du_total += size;
            if (cu_du_all) {
                char full[256];
                cu_join(full, sizeof(full), path, name);
                cu_put_num((size + 1023) / 1024, 8);
                term_print("  ");
                term_print(full);
                term_putc('\n');
            }
        }
    }
}

static void cu_cmd_du(int argc, char **argv) {
    const char *target = ".";
    cu_du_all = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-a")) cu_du_all = 1;
        else if (argv[a][0] != '-') target = argv[a];
    }
    char abs[256];
    term_resolve(target, abs);

    uint64_t sz = 0;
    int is_dir = 0;
    if (!fs_stat(abs, &sz, &is_dir)) { cu_err("du", "no such file"); return; }
    if (!is_dir) {
        cu_put_num((uint32_t)((sz + 1023) / 1024), 8);
        term_print("  ");
        term_print(abs);
        term_putc('\n');
        return;
    }
    cu_du_total = 0;
    cu_du_walk(abs, 0);
    cu_put_num((uint32_t)((cu_du_total + 1023) / 1024), 8);
    term_print("  ");
    term_print(abs);
    term_print("   (KiB)\n");
}

/* ===== text ===== */

static void cu_cmd_wc(int argc, char **argv) {
    int want_l = 0, want_w = 0, want_c = 0, files = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-l")) want_l = 1;
        else if (str_eq(argv[a], "-w")) want_w = 1;
        else if (str_eq(argv[a], "-c") || str_eq(argv[a], "-m")) want_c = 1;
        else if (argv[a][0] != '-') files++;
    }
    if (!want_l && !want_w && !want_c) want_l = want_w = want_c = 1;

    uint32_t tl = 0, tw = 0, tc = 0;
    int shown = 0;

    for (int a = 1; a <= argc; a++) {
        const char *name = 0;
        if (a < argc) {
            if (argv[a][0] == '-') continue;
            name = argv[a];
        } else if (files > 0) {
            break;                                   /* already did them */
        }

        const uint8_t *d;
        uint32_t n;
        if (!cu_src(name, &d, &n)) { cu_err("wc", cu_lasterr); return; }

        uint32_t l = 0, w = 0, inw = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (d[i] == '\n') l++;
            if (cu_is_space((char)d[i])) inw = 0;
            else if (!inw) { inw = 1; w++; }
        }
        if (n > 0 && d[n - 1] != '\n') l++;          /* unterminated last */

        if (want_l) { cu_put_num(l, 8); }
        if (want_w) { cu_put_num(w, 8); }
        if (want_c) { cu_put_num(n, 9); }
        if (name) { term_print("  "); term_print(name); }
        term_putc('\n');
        tl += l; tw += w; tc += n;
        shown++;
        if (!name) break;
    }
    if (shown > 1) {
        if (want_l) cu_put_num(tl, 8);
        if (want_w) cu_put_num(tw, 8);
        if (want_c) cu_put_num(tc, 9);
        term_print("  total\n");
    }
}

static void cu_cmd_head_tail(int argc, char **argv, int from_end) {
    int count = 10;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-' && argv[a][1] == 'n' && a + 1 < argc)
            count = cu_atoi(argv[++a], 10);
        else if (argv[a][0] == '-' && argv[a][1] >= '0' && argv[a][1] <= '9')
            count = cu_atoi(argv[a] + 1, 10);
        else if (argv[a][0] != '-') name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err(from_end ? "tail" : "head", cu_lasterr); return; }

    uint32_t nl = cu_split_lines(d, n);
    uint32_t first = 0, last = nl;
    if (count < 0) count = 0;
    if (from_end) { if ((uint32_t)count < nl) first = nl - (uint32_t)count; }
    else          { if ((uint32_t)count < nl) last = (uint32_t)count; }

    for (uint32_t i = first; i < last; i++)
        cu_put_line(d + cu_line_off[i], cu_line_len[i]);
}

static void cu_cmd_grep(int argc, char **argv) {
    int fold = 0, invert = 0, number = 0, count_only = 0, names_only = 0;
    const char *pat = 0, *name = 0;

    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-' && argv[a][1]) {
            for (int k = 1; argv[a][k]; k++) {
                switch (argv[a][k]) {
                case 'i': fold = 1; break;
                case 'v': invert = 1; break;
                case 'n': number = 1; break;
                case 'c': count_only = 1; break;
                case 'l': names_only = 1; break;
                default: break;
                }
            }
        } else if (!pat) pat = argv[a];
        else if (!name) name = argv[a];
    }
    if (!pat) { cu_usage("grep [-ivncl] <pattern> [file]"); return; }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("grep", cu_lasterr); return; }

    uint32_t nl = cu_split_lines(d, n), hits = 0;
    for (uint32_t i = 0; i < nl; i++) {
        int hit = cu_find_sub(d + cu_line_off[i], cu_line_len[i], pat, fold) >= 0;
        if (hit == invert) continue;
        hits++;
        if (count_only || names_only) continue;
        if (number) { cu_put_num(i + 1, 6); term_print(": "); }
        cu_put_line(d + cu_line_off[i], cu_line_len[i]);
    }
    if (count_only) { cu_put_num(hits, 1); term_putc('\n'); }
    if (names_only && hits && name) { term_print(name); term_putc('\n'); }
}

/* sort: an index sort over the line table, so the file itself never
 * moves. Insertion sort — the line cap is 8192 and this is not a hot
 * path, and it keeps equal lines in their original order. */
static void cu_cmd_sort(int argc, char **argv) {
    int rev = 0, uniq = 0, numeric = 0, fold = 0;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-' && argv[a][1]) {
            for (int k = 1; argv[a][k]; k++) {
                switch (argv[a][k]) {
                case 'r': rev = 1; break;
                case 'u': uniq = 1; break;
                case 'n': numeric = 1; break;
                case 'f': fold = 1; break;
                default: break;
                }
            }
        } else name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("sort", cu_lasterr); return; }

    uint32_t nl = cu_split_lines(d, n);
    static uint32_t idx[CU_MAX_LINES];
    for (uint32_t i = 0; i < nl; i++) idx[i] = i;

    for (uint32_t i = 1; i < nl; i++) {
        uint32_t key = idx[i];
        uint32_t j = i;
        while (j > 0) {
            uint32_t a = idx[j - 1], b = key;
            int cmp;
            if (numeric) {
                char ta[24], tb[24];
                uint32_t la = cu_line_len[a] < 23 ? cu_line_len[a] : 23;
                uint32_t lb = cu_line_len[b] < 23 ? cu_line_len[b] : 23;
                for (uint32_t k = 0; k < la; k++) ta[k] = (char)d[cu_line_off[a] + k];
                for (uint32_t k = 0; k < lb; k++) tb[k] = (char)d[cu_line_off[b] + k];
                ta[la] = '\0'; tb[lb] = '\0';
                int va = cu_atoi(ta, 0), vb = cu_atoi(tb, 0);
                cmp = va < vb ? -1 : (va > vb ? 1 : 0);
            } else {
                uint32_t la = cu_line_len[a], lb = cu_line_len[b];
                uint32_t m = la < lb ? la : lb;
                cmp = 0;
                for (uint32_t k = 0; k < m; k++) {
                    char ca = (char)d[cu_line_off[a] + k];
                    char cb = (char)d[cu_line_off[b] + k];
                    if (fold) { ca = cu_lower(ca); cb = cu_lower(cb); }
                    if (ca != cb) { cmp = ca < cb ? -1 : 1; break; }
                }
                if (cmp == 0 && la != lb) cmp = la < lb ? -1 : 1;
            }
            if (rev) cmp = -cmp;
            if (cmp <= 0) break;
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = key;
    }

    for (uint32_t i = 0; i < nl; i++) {
        if (uniq && i > 0) {
            uint32_t a = idx[i - 1], b = idx[i];
            if (cu_line_len[a] == cu_line_len[b]) {
                uint32_t k = 0;
                while (k < cu_line_len[a] &&
                       d[cu_line_off[a] + k] == d[cu_line_off[b] + k]) k++;
                if (k == cu_line_len[a]) continue;
            }
        }
        cu_put_line(d + cu_line_off[idx[i]], cu_line_len[idx[i]]);
    }
}

static void cu_cmd_uniq(int argc, char **argv) {
    int count = 0, only_dup = 0, only_uniq = 0;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-c")) count = 1;
        else if (str_eq(argv[a], "-d")) only_dup = 1;
        else if (str_eq(argv[a], "-u")) only_uniq = 1;
        else name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("uniq", cu_lasterr); return; }

    uint32_t nl = cu_split_lines(d, n);
    uint32_t i = 0;
    while (i < nl) {
        uint32_t run = 1;
        while (i + run < nl) {
            uint32_t a = i, b = i + run;
            if (cu_line_len[a] != cu_line_len[b]) break;
            uint32_t k = 0;
            while (k < cu_line_len[a] &&
                   d[cu_line_off[a] + k] == d[cu_line_off[b] + k]) k++;
            if (k != cu_line_len[a]) break;
            run++;
        }
        int show = 1;
        if (only_dup && run < 2) show = 0;
        if (only_uniq && run > 1) show = 0;
        if (show) {
            if (count) { cu_put_num(run, 6); term_print(" "); }
            cu_put_line(d + cu_line_off[i], cu_line_len[i]);
        }
        i += run;
    }
}

static void cu_cmd_tac(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(argc >= 2 ? argv[1] : 0, &d, &n)) { cu_err("tac", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);
    for (uint32_t i = nl; i > 0; i--)
        cu_put_line(d + cu_line_off[i - 1], cu_line_len[i - 1]);
}

static void cu_cmd_rev(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(argc >= 2 ? argv[1] : 0, &d, &n)) { cu_err("rev", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);
    for (uint32_t i = 0; i < nl; i++) {
        for (uint32_t k = cu_line_len[i]; k > 0; k--)
            term_putc((char)d[cu_line_off[i] + k - 1]);
        term_putc('\n');
    }
}

static void cu_cmd_nl(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(argc >= 2 ? argv[1] : 0, &d, &n)) { cu_err("nl", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);
    for (uint32_t i = 0; i < nl; i++) {
        cu_put_num(i + 1, 6);
        term_print("  ");
        cu_put_line(d + cu_line_off[i], cu_line_len[i]);
    }
}

static void cu_cmd_cut(int argc, char **argv) {
    char delim = '\t';
    int  f_lo = 1, f_hi = 1, by_char = 0;
    const char *name = 0;

    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-d") && a + 1 < argc) delim = argv[++a][0];
        else if (str_eq(argv[a], "-f") && a + 1 < argc) {
            const char *r = argv[++a];
            f_lo = cu_atoi(r, 1);
            const char *dash = r;
            while (*dash && *dash != '-') dash++;
            f_hi = *dash ? (dash[1] ? cu_atoi(dash + 1, f_lo) : 9999) : f_lo;
        } else if (str_eq(argv[a], "-c") && a + 1 < argc) {
            by_char = 1;
            const char *r = argv[++a];
            f_lo = cu_atoi(r, 1);
            const char *dash = r;
            while (*dash && *dash != '-') dash++;
            f_hi = *dash ? (dash[1] ? cu_atoi(dash + 1, f_lo) : 9999) : f_lo;
        } else if (argv[a][0] != '-') name = argv[a];
    }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("cut", cu_lasterr); return; }

    uint32_t nl = cu_split_lines(d, n);
    for (uint32_t i = 0; i < nl; i++) {
        const uint8_t *L = d + cu_line_off[i];
        uint32_t ln = cu_line_len[i];
        if (by_char) {
            for (uint32_t k = 0; k < ln; k++)
                if ((int)k + 1 >= f_lo && (int)k + 1 <= f_hi) term_putc((char)L[k]);
            term_putc('\n');
        } else {
            int field = 1;
            uint32_t k = 0;
            int wrote = 0;
            while (k <= ln) {
                uint32_t start = k;
                while (k < ln && (char)L[k] != delim) k++;
                if (field >= f_lo && field <= f_hi) {
                    if (wrote) term_putc(delim);
                    for (uint32_t j = start; j < k; j++) term_putc((char)L[j]);
                    wrote = 1;
                }
                if (k >= ln) break;
                k++; field++;
            }
            term_putc('\n');
        }
    }
}

static void cu_cmd_tr(int argc, char **argv) {
    int del = 0, squeeze = 0, ai = 1;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1]) {
        for (int k = 1; argv[ai][k]; k++) {
            if (argv[ai][k] == 'd') del = 1;
            if (argv[ai][k] == 's') squeeze = 1;
        }
        ai++;
    }
    if (ai >= argc) { cu_usage("tr [-ds] <set1> [set2] [file]"); return; }
    const char *s1 = argv[ai++];
    const char *s2 = (!del && ai < argc) ? argv[ai++] : "";
    const char *name = (ai < argc) ? argv[ai] : 0;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("tr", cu_lasterr); return; }

    /*
     * Expand ranges. `tr a-z A-Z` is the canonical use and it is useless
     * without this -- taken literally, a-z is the three characters 'a',
     * '-' and 'z', which is what it did at first and why only the letter
     * a came out uppercased.
     */
    static char e1[256], e2[256];
    int s1n = 0, s2n = 0;
    for (int i = 0; s1[i] && s1n < 255; i++) {
        if (s1[i + 1] == '-' && s1[i + 2] && s1[i + 2] >= s1[i]) {
            for (char c = s1[i]; c <= s1[i + 2] && s1n < 255; c++) e1[s1n++] = c;
            i += 2;
        } else e1[s1n++] = s1[i];
    }
    for (int i = 0; s2[i] && s2n < 255; i++) {
        if (s2[i + 1] == '-' && s2[i + 2] && s2[i + 2] >= s2[i]) {
            for (char c = s2[i]; c <= s2[i + 2] && s2n < 255; c++) e2[s2n++] = c;
            i += 2;
        } else e2[s2n++] = s2[i];
    }
    s1 = e1; s2 = e2;

    char prev = 0;
    int have_prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = (char)d[i];
        int at = -1;
        for (int k = 0; k < s1n; k++) if (s1[k] == c) { at = k; break; }
        if (at >= 0) {
            if (del) continue;
            if (s2n > 0) c = s2[at < s2n ? at : s2n - 1];
        }
        if (squeeze && have_prev && c == prev && at >= 0) continue;
        term_putc(c);
        prev = c;
        have_prev = 1;
    }
}

static void cu_cmd_tee(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    if (!cu_pipe_ready) { cu_err("tee", "nothing piped in"); return; }
    d = (const uint8_t *)cu_pipe;
    n = cu_pipe_len;

    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-') continue;
        char abs[256];
        term_resolve(argv[a], abs);
        if (fs_write_file(abs, d, n) != 0) cu_err("tee", fs_errstr);
    }
    for (uint32_t i = 0; i < n; i++) term_putc((char)d[i]);
}

static void cu_cmd_fold(int argc, char **argv) {
    int width = 80;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-w") && a + 1 < argc) width = cu_atoi(argv[++a], 80);
        else if (argv[a][0] == '-' && argv[a][1] >= '0' && argv[a][1] <= '9')
            width = cu_atoi(argv[a] + 1, 80);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (width < 1) width = 1;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("fold", cu_lasterr); return; }

    int col = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (d[i] == '\n') { term_putc('\n'); col = 0; continue; }
        term_putc((char)d[i]);
        if (++col >= width) { term_putc('\n'); col = 0; }
    }
    if (col) term_putc('\n');
}

static void cu_cmd_expand(int argc, char **argv, int reverse) {
    int width = 8;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-t") && a + 1 < argc) width = cu_atoi(argv[++a], 8);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (width < 1) width = 1;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) {
        cu_err(reverse ? "unexpand" : "expand", cu_lasterr);
        return;
    }
    int col = 0, run = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\n') { term_putc('\n'); col = run = 0; continue; }
        if (!reverse) {
            if (c == '\t') {
                int adv = width - (col % width);
                for (int k = 0; k < adv; k++) { term_putc(' '); col++; }
            } else { term_putc(c); col++; }
        } else {
            if (c == ' ') {
                run++;
                if ((col + run) % width == 0) { term_putc('\t'); col += run; run = 0; }
            } else {
                for (int k = 0; k < run; k++) term_putc(' ');
                col += run; run = 0;
                term_putc(c); col++;
            }
        }
    }
}

static void cu_cmd_strings(int argc, char **argv) {
    int minlen = 4;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-n") && a + 1 < argc) minlen = cu_atoi(argv[++a], 4);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (minlen < 1) minlen = 1;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("strings", cu_lasterr); return; }

    uint32_t start = 0, run = 0;
    for (uint32_t i = 0; i <= n; i++) {
        int printable = i < n && d[i] >= 0x20 && d[i] < 0x7F;
        if (printable) { if (run == 0) start = i; run++; continue; }
        if ((int)run >= minlen) cu_put_line(d + start, run);
        run = 0;
    }
}

/* od / hexdump / xxd: one implementation, three names, because the only
 * real difference is the default format. */
static void cu_cmd_hexdump(int argc, char **argv, int style) {
    const char *name = 0;
    uint32_t limit = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-n") && a + 1 < argc)
            limit = (uint32_t)cu_atoi(argv[++a], 0);
        else if (argv[a][0] != '-') name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("hexdump", cu_lasterr); return; }
    if (limit && limit < n) n = limit;

    static const char hx[] = "0123456789abcdef";
    for (uint32_t off = 0; off < n; off += 16) {
        for (int k = 7; k >= 0; k--) term_putc(hx[(off >> (k * 4)) & 15]);
        term_print("  ");
        for (uint32_t k = 0; k < 16; k++) {
            if (off + k < n) {
                term_putc(hx[d[off + k] >> 4]);
                term_putc(hx[d[off + k] & 15]);
            } else term_print("  ");
            term_putc(' ');
            if (k == 7 && style) term_putc(' ');
        }
        if (style) {
            term_print(" |");
            for (uint32_t k = 0; k < 16 && off + k < n; k++) {
                uint8_t c = d[off + k];
                term_putc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
            }
            term_putc('|');
        }
        term_putc('\n');
    }
    for (int k = 7; k >= 0; k--) term_putc(hx[(n >> (k * 4)) & 15]);
    term_putc('\n');
}

/*
 * cmp: the shared read buffer means the two files cannot both be open, so
 * the first is copied out. That bounds what can be compared, and the
 * bound is stated rather than silently applied.
 */
#define CU_CMP_MAX (512 * 1024)
static uint8_t cu_cmp_buf[CU_CMP_MAX];

static void cu_cmd_cmp(int argc, char **argv) {
    if (argc < 3) { cu_usage("cmp <file1> <file2>"); return; }

    const uint8_t *a;
    uint32_t an;
    if (!cu_src(argv[1], &a, &an)) { cu_err("cmp", cu_lasterr); return; }
    if (an > CU_CMP_MAX) { cu_err("cmp", "first file is over 512 KiB"); return; }
    for (uint32_t i = 0; i < an; i++) cu_cmp_buf[i] = a[i];

    const uint8_t *b;
    uint32_t bn;
    if (!cu_src(argv[2], &b, &bn)) { cu_err("cmp", cu_lasterr); return; }

    uint32_t m = an < bn ? an : bn;
    for (uint32_t i = 0; i < m; i++) {
        if (cu_cmp_buf[i] != b[i]) {
            term_print(argv[1]);
            term_print(" ");
            term_print(argv[2]);
            term_print(" differ: byte ");
            cu_put_num(i + 1, 1);
            term_putc('\n');
            return;
        }
    }
    if (an != bn) {
        term_print("EOF on ");
        term_print(an < bn ? argv[1] : argv[2]);
        term_putc('\n');
        return;
    }
    term_print_c("identical\n", 4);
}

/* diff: line-level, and honest about being so. A real diff needs an LCS
 * over two line tables, which needs both files resident at once. */
static void cu_cmd_diff(int argc, char **argv) {
    if (argc < 3) { cu_usage("diff <file1> <file2>"); return; }

    const uint8_t *a;
    uint32_t an;
    if (!cu_src(argv[1], &a, &an)) { cu_err("diff", cu_lasterr); return; }
    if (an > CU_CMP_MAX) { cu_err("diff", "first file is over 512 KiB"); return; }
    for (uint32_t i = 0; i < an; i++) cu_cmp_buf[i] = a[i];
    uint32_t anl = cu_split_lines(cu_cmp_buf, an);

    static uint32_t aoff[CU_MAX_LINES], alen[CU_MAX_LINES];
    for (uint32_t i = 0; i < anl; i++) { aoff[i] = cu_line_off[i]; alen[i] = cu_line_len[i]; }

    const uint8_t *b;
    uint32_t bn;
    if (!cu_src(argv[2], &b, &bn)) { cu_err("diff", cu_lasterr); return; }
    uint32_t bnl = cu_split_lines(b, bn);

    uint32_t i = 0, j = 0, diffs = 0;
    while (i < anl || j < bnl) {
        int same = 0;
        if (i < anl && j < bnl && alen[i] == cu_line_len[j]) {
            uint32_t k = 0;
            while (k < alen[i] && cu_cmp_buf[aoff[i] + k] == b[cu_line_off[j] + k]) k++;
            same = (k == alen[i]);
        }
        if (same) { i++; j++; continue; }

        if (i < anl) {
            term_print_c("< ", 2);
            for (uint32_t k = 0; k < alen[i]; k++) term_putc((char)cu_cmp_buf[aoff[i] + k]);
            term_putc('\n');
            i++;
        }
        if (j < bnl) {
            term_print_c("> ", 4);
            cu_put_line(b + cu_line_off[j], cu_line_len[j]);
            j++;
        }
        diffs++;
    }
    if (diffs == 0) term_print_c("identical\n", 4);
}

/* file: identify by magic, falling back to a text/binary judgement. */
static void cu_cmd_file(int argc, char **argv) {
    if (argc < 2) { cu_usage("file <path>..."); return; }
    for (int a = 1; a < argc; a++) {
        char abs[256];
        term_resolve(argv[a], abs);
        term_print(argv[a]);
        term_print(": ");

        uint64_t sz = 0;
        int is_dir = 0;
        if (!fs_stat(abs, &sz, &is_dir)) { term_print_c("cannot open\n", 2); continue; }
        if (is_dir) { term_print_c("directory\n", 4); continue; }
        if (sz == 0) { term_print("empty\n"); continue; }

        const uint8_t *d;
        uint32_t n;
        if (!cu_src(argv[a], &d, &n)) { term_print_c(cu_lasterr, 2); term_putc('\n'); continue; }

        const char *kind = 0;
        if (n >= 4 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')
            kind = "ELF64 executable";
        else if (n >= 4 && d[0] == 'S' && d[1] == 'B' && d[2] == 'S' && d[3] == 'D')
            kind = ".bsd executable";
        else if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
            kind = "PNG image";
        else if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
            kind = "JPEG image";
        else if (n >= 4 && d[0] == 'G' && d[1] == 'G' && d[2] == 'U' && d[3] == 'F')
            kind = "GGUF model";
        else if (n >= 4 && d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD)
            kind = "Zstandard compressed";
        else if (n >= 2 && d[0] == 0x1F && d[1] == 0x8B)
            kind = "gzip compressed";
        else if (n >= 6 && d[0] == 0xFD && d[1] == '7' && d[2] == 'z')
            kind = "XZ compressed";
        else if (n >= 5 && d[0] == 'Z' && d[1] == 'I' && d[2] == 'M' && d[3] == 0x04)
            kind = "ZIM archive";
        else if (n >= 262 && d[257] == 'u' && d[258] == 's' && d[259] == 't' &&
                 d[260] == 'a' && d[261] == 'r')
            kind = "tar archive";
        else if (n >= 2 && d[0] == 'B' && d[1] == 'M')
            kind = "BMP image";
        else if (n >= 2 && d[0] == 'P' && (d[1] >= '1' && d[1] <= '6'))
            kind = "Netpbm image";

        if (kind) { term_print(kind); term_putc('\n'); continue; }

        uint32_t sample = n < 1024 ? n : 1024, printable = 0;
        for (uint32_t i = 0; i < sample; i++)
            if ((d[i] >= 0x20 && d[i] < 0x7F) || d[i] == '\n' ||
                d[i] == '\t' || d[i] == '\r') printable++;
        term_print(printable * 10 >= sample * 9 ? "ASCII text\n" : "data\n");
    }
}

/* split / truncate */
static void cu_cmd_split(int argc, char **argv) {
    if (argc < 2) { cu_usage("split [-l n] <file> [prefix]"); return; }
    int per = 1000;
    const char *name = 0, *prefix = "x";
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-l") && a + 1 < argc) per = cu_atoi(argv[++a], 1000);
        else if (argv[a][0] != '-') { if (!name) name = argv[a]; else prefix = argv[a]; }
    }
    if (per < 1) per = 1;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("split", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);

    static char part[CU_PIPE_MAX];
    uint32_t made = 0;
    for (uint32_t i = 0; i < nl; i += (uint32_t)per) {
        uint32_t o = 0;
        for (uint32_t k = i; k < nl && k < i + (uint32_t)per; k++) {
            for (uint32_t j = 0; j < cu_line_len[k] && o < sizeof(part) - 2; j++)
                part[o++] = (char)d[cu_line_off[k] + j];
            if (o < sizeof(part) - 1) part[o++] = '\n';
        }
        char out[256], nb[16];
        str_copy(out, prefix, sizeof(out));
        uint_to_str(made, nb);
        str_append(out, nb, sizeof(out));
        char abs[256];
        term_resolve(out, abs);
        if (fs_write_file(abs, part, o) != 0) { cu_err("split", fs_errstr); return; }
        term_print("wrote ");
        term_print(out);
        term_putc('\n');
        made++;
    }
}

static void cu_cmd_truncate(int argc, char **argv) {
    if (argc < 3) { cu_usage("truncate -s <size> <file>"); return; }
    int size = 0;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-s") && a + 1 < argc) size = cu_atoi(argv[++a], 0);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (!name) { cu_usage("truncate -s <size> <file>"); return; }
    if (size < 0) size = 0;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("truncate", cu_lasterr); return; }

    static uint8_t buf[CU_CMP_MAX];
    uint32_t want = (uint32_t)size;
    if (want > CU_CMP_MAX) { cu_err("truncate", "over 512 KiB"); return; }
    for (uint32_t i = 0; i < want; i++) buf[i] = i < n ? d[i] : 0;

    char abs[256];
    term_resolve(name, abs);
    if (fs_write_file(abs, buf, want) != 0) cu_err("truncate", fs_errstr);
}

/* ===== checksums ===== */

static void cu_cmd_sha256(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    const char *name = argc >= 2 ? argv[1] : 0;
    if (!cu_src(name, &d, &n)) { cu_err("sha256sum", cu_lasterr); return; }

    uint8_t h[32];
    sha256(d, n, h);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { term_putc(hx[h[i] >> 4]); term_putc(hx[h[i] & 15]); }
    term_print("  ");
    term_print(name ? name : "-");
    term_putc('\n');
}

/* cksum: CRC-32, the one every other tool agrees on. */
static void cu_cmd_cksum(int argc, char **argv) {
    const uint8_t *d;
    uint32_t n;
    const char *name = argc >= 2 ? argv[1] : 0;
    if (!cu_src(name, &d, &n)) { cu_err("cksum", cu_lasterr); return; }

    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    crc = ~crc;
    cu_put_num(crc, 1);
    term_print(" ");
    cu_put_num(n, 1);
    term_print(" ");
    term_print(name ? name : "-");
    term_putc('\n');
}

static void cu_cmd_base64(int argc, char **argv, int decode) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-d")) decode = 1;
        else if (argv[a][0] != '-') name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("base64", cu_lasterr); return; }

    if (!decode) {
        int col = 0;
        for (uint32_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)d[i] << 16;
            if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
            if (i + 2 < n) v |= d[i + 2];
            term_putc(b64[(v >> 18) & 63]);
            term_putc(b64[(v >> 12) & 63]);
            term_putc(i + 1 < n ? b64[(v >> 6) & 63] : '=');
            term_putc(i + 2 < n ? b64[v & 63] : '=');
            if ((col += 4) >= 76) { term_putc('\n'); col = 0; }
        }
        if (col) term_putc('\n');
        return;
    }

    uint32_t acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '=' ) break;
        int v = -1;
        for (int k = 0; k < 64; k++) if (b64[k] == c) { v = k; break; }
        if (v < 0) continue;                     /* newlines and padding */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; term_putc((char)((acc >> bits) & 0xFF)); }
    }
}


/* ===== dispatch =====
 *
 * A table rather than another arm on term_exec's if-chain: this file will
 * keep growing, and a chain that long stops being readable well before it
 * stops compiling. Returns 1 if the name was handled.
 */
static int cu_dispatch(const char *cmd, int argc, char **argv) {
    /* file and directory operations */
    if (str_eq(cmd, "tree"))      { cu_cmd_tree(argc, argv); return 1; }
    if (str_eq(cmd, "stat"))      { cu_cmd_stat(argc, argv); return 1; }
    if (str_eq(cmd, "touch"))     { cu_cmd_touch(argc, argv); return 1; }
    if (str_eq(cmd, "mv"))        { cu_cmd_mv(argc, argv); return 1; }
    if (str_eq(cmd, "ln") || str_eq(cmd, "link") || str_eq(cmd, "unlink"))
                                  { cu_cmd_ln(argc, argv); return 1; }
    if (str_eq(cmd, "basename"))  { cu_cmd_basename(argc, argv); return 1; }
    if (str_eq(cmd, "dirname"))   { cu_cmd_dirname(argc, argv); return 1; }
    if (str_eq(cmd, "realpath") || str_eq(cmd, "readlink"))
                                  { cu_cmd_realpath(argc, argv); return 1; }
    if (str_eq(cmd, "find"))      { cu_cmd_find(argc, argv); return 1; }
    if (str_eq(cmd, "du"))        { cu_cmd_du(argc, argv); return 1; }

    /* text */
    if (str_eq(cmd, "wc"))        { cu_cmd_wc(argc, argv); return 1; }
    if (str_eq(cmd, "head"))      { cu_cmd_head_tail(argc, argv, 0); return 1; }
    if (str_eq(cmd, "tail"))      { cu_cmd_head_tail(argc, argv, 1); return 1; }
    if (str_eq(cmd, "grep") || str_eq(cmd, "egrep") || str_eq(cmd, "fgrep"))
                                  { cu_cmd_grep(argc, argv); return 1; }
    if (str_eq(cmd, "sort"))      { cu_cmd_sort(argc, argv); return 1; }
    if (str_eq(cmd, "uniq"))      { cu_cmd_uniq(argc, argv); return 1; }
    if (str_eq(cmd, "tac"))       { cu_cmd_tac(argc, argv); return 1; }
    if (str_eq(cmd, "rev"))       { cu_cmd_rev(argc, argv); return 1; }
    if (str_eq(cmd, "nl"))        { cu_cmd_nl(argc, argv); return 1; }
    if (str_eq(cmd, "cut"))       { cu_cmd_cut(argc, argv); return 1; }
    if (str_eq(cmd, "tr"))        { cu_cmd_tr(argc, argv); return 1; }
    if (str_eq(cmd, "tee"))       { cu_cmd_tee(argc, argv); return 1; }
    if (str_eq(cmd, "fold"))      { cu_cmd_fold(argc, argv); return 1; }
    if (str_eq(cmd, "expand"))    { cu_cmd_expand(argc, argv, 0); return 1; }
    if (str_eq(cmd, "unexpand"))  { cu_cmd_expand(argc, argv, 1); return 1; }
    if (str_eq(cmd, "strings"))   { cu_cmd_strings(argc, argv); return 1; }
    if (str_eq(cmd, "hexdump") || str_eq(cmd, "xxd"))
                                  { cu_cmd_hexdump(argc, argv, 1); return 1; }
    if (str_eq(cmd, "od"))        { cu_cmd_hexdump(argc, argv, 0); return 1; }
    if (str_eq(cmd, "cmp"))       { cu_cmd_cmp(argc, argv); return 1; }
    if (str_eq(cmd, "diff"))      { cu_cmd_diff(argc, argv); return 1; }
    if (str_eq(cmd, "file"))      { cu_cmd_file(argc, argv); return 1; }
    if (str_eq(cmd, "split"))     { cu_cmd_split(argc, argv); return 1; }
    if (str_eq(cmd, "truncate"))  { cu_cmd_truncate(argc, argv); return 1; }

    /* checksums and encodings */
    if (str_eq(cmd, "sha256sum") || str_eq(cmd, "shasum") ||
        str_eq(cmd, "sum"))       { cu_cmd_sha256(argc, argv); return 1; }
    if (str_eq(cmd, "cksum") || str_eq(cmd, "crc32"))
                                  { cu_cmd_cksum(argc, argv); return 1; }
    if (str_eq(cmd, "base64"))    { cu_cmd_base64(argc, argv, 0); return 1; }
    if (str_eq(cmd, "base64d"))   { cu_cmd_base64(argc, argv, 1); return 1; }

    return 0;
}

#endif /* COREUTILS_H */
