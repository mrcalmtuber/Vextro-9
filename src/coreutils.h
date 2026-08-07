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



/* ===== more text: sed, paste, comm, join, column, fmt, pr, csplit ===== */

/*
 * sed, in the one form that carries its weight: s/old/new/[g], plus
 * address-free d and p. A full sed is a language with its own parser and
 * hold space; this is the substitution people actually reach for.
 */
static void cu_cmd_sed(int argc, char **argv) {
    if (argc < 2) { cu_usage("sed 's/old/new/[g]' [file]   |   sed -n /pat/p [file]"); return; }

    const char *script = 0, *name = 0;
    int quiet = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-n")) quiet = 1;
        else if (!script) script = argv[a];
        else name = argv[a];
    }
    if (!script) { cu_usage("sed 's/old/new/[g]' [file]"); return; }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("sed", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);

    if (script[0] == 's' && script[1]) {
        char sep = script[1];
        char pat[128], rep[128];
        int i = 2, k = 0;
        while (script[i] && script[i] != sep && k < 127) pat[k++] = script[i++];
        pat[k] = '\0';
        if (script[i] != sep) { cu_err("sed", "unterminated s command"); return; }
        i++; k = 0;
        while (script[i] && script[i] != sep && k < 127) rep[k++] = script[i++];
        rep[k] = '\0';
        int global = 0;
        if (script[i] == sep) { i++; if (script[i] == 'g') global = 1; }

        int patn = 0; while (pat[patn]) patn++;
        if (patn == 0) { cu_err("sed", "empty pattern"); return; }

        for (uint32_t li = 0; li < nl; li++) {
            const uint8_t *L = d + cu_line_off[li];
            uint32_t ln = cu_line_len[li], pos = 0;
            int done = 0;
            while (pos < ln) {
                int at = -1;
                if (!done || global)
                    at = cu_find_sub(L + pos, ln - pos, pat, 0);
                if (at < 0) break;
                for (int j = 0; j < at; j++) term_putc((char)L[pos + j]);
                term_print(rep);
                pos += (uint32_t)at + (uint32_t)patn;
                done = 1;
                if (!global) break;
            }
            for (uint32_t j = pos; j < ln; j++) term_putc((char)L[j]);
            term_putc('\n');
        }
        return;
    }

    /* /pattern/p and /pattern/d */
    if (script[0] == '/') {
        char pat[128];
        int i = 1, k = 0;
        while (script[i] && script[i] != '/' && k < 127) pat[k++] = script[i++];
        pat[k] = '\0';
        char act = script[i] == '/' ? script[i + 1] : 'p';
        for (uint32_t li = 0; li < nl; li++) {
            int hit = cu_find_sub(d + cu_line_off[li], cu_line_len[li], pat, 0) >= 0;
            if (act == 'd') { if (!hit) cu_put_line(d + cu_line_off[li], cu_line_len[li]); }
            else            { if (hit)  cu_put_line(d + cu_line_off[li], cu_line_len[li]); }
        }
        return;
    }
    (void)quiet;
    cu_err("sed", "only s/// and /pat/p and /pat/d are supported");
}

/* comm: three columns of set difference over two sorted files. */
static void cu_cmd_comm(int argc, char **argv) {
    if (argc < 3) { cu_usage("comm <file1> <file2>"); return; }

    const uint8_t *a;
    uint32_t an;
    if (!cu_src(argv[1], &a, &an)) { cu_err("comm", cu_lasterr); return; }
    if (an > CU_CMP_MAX) { cu_err("comm", "first file is over 512 KiB"); return; }
    for (uint32_t i = 0; i < an; i++) cu_cmp_buf[i] = a[i];
    uint32_t anl = cu_split_lines(cu_cmp_buf, an);
    static uint32_t aoff[CU_MAX_LINES], alen[CU_MAX_LINES];
    for (uint32_t i = 0; i < anl; i++) { aoff[i] = cu_line_off[i]; alen[i] = cu_line_len[i]; }

    const uint8_t *b;
    uint32_t bn;
    if (!cu_src(argv[2], &b, &bn)) { cu_err("comm", cu_lasterr); return; }
    uint32_t bnl = cu_split_lines(b, bn);

    uint32_t i = 0, j = 0;
    while (i < anl || j < bnl) {
        int cmp;
        if (i >= anl) cmp = 1;
        else if (j >= bnl) cmp = -1;
        else {
            uint32_t m = alen[i] < cu_line_len[j] ? alen[i] : cu_line_len[j];
            cmp = 0;
            for (uint32_t k = 0; k < m; k++) {
                if (cu_cmp_buf[aoff[i] + k] != b[cu_line_off[j] + k]) {
                    cmp = cu_cmp_buf[aoff[i] + k] < b[cu_line_off[j] + k] ? -1 : 1;
                    break;
                }
            }
            if (cmp == 0 && alen[i] != cu_line_len[j])
                cmp = alen[i] < cu_line_len[j] ? -1 : 1;
        }
        if (cmp < 0) {
            for (uint32_t k = 0; k < alen[i]; k++) term_putc((char)cu_cmp_buf[aoff[i] + k]);
            term_putc('\n');
            i++;
        } else if (cmp > 0) {
            term_putc('\t');
            cu_put_line(b + cu_line_off[j], cu_line_len[j]);
            j++;
        } else {
            term_print("\t\t");
            cu_put_line(b + cu_line_off[j], cu_line_len[j]);
            i++; j++;
        }
    }
}

/* paste: two files side by side, tab separated. */
static void cu_cmd_paste(int argc, char **argv) {
    if (argc < 3) { cu_usage("paste <file1> <file2>"); return; }

    const uint8_t *a;
    uint32_t an;
    if (!cu_src(argv[1], &a, &an)) { cu_err("paste", cu_lasterr); return; }
    if (an > CU_CMP_MAX) { cu_err("paste", "first file is over 512 KiB"); return; }
    for (uint32_t i = 0; i < an; i++) cu_cmp_buf[i] = a[i];
    uint32_t anl = cu_split_lines(cu_cmp_buf, an);
    static uint32_t aoff[CU_MAX_LINES], alen[CU_MAX_LINES];
    for (uint32_t i = 0; i < anl; i++) { aoff[i] = cu_line_off[i]; alen[i] = cu_line_len[i]; }

    const uint8_t *b;
    uint32_t bn;
    if (!cu_src(argv[2], &b, &bn)) { cu_err("paste", cu_lasterr); return; }
    uint32_t bnl = cu_split_lines(b, bn);

    uint32_t rows = anl > bnl ? anl : bnl;
    for (uint32_t i = 0; i < rows; i++) {
        if (i < anl)
            for (uint32_t k = 0; k < alen[i]; k++) term_putc((char)cu_cmp_buf[aoff[i] + k]);
        term_putc('\t');
        if (i < bnl)
            for (uint32_t k = 0; k < cu_line_len[i]; k++)
                term_putc((char)b[cu_line_off[i] + k]);
        term_putc('\n');
    }
}

/* column: pad field 1 so a table lines up. */
static void cu_cmd_column(int argc, char **argv) {
    char delim = ' ';
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-s") && a + 1 < argc) delim = argv[++a][0];
        else if (str_eq(argv[a], "-t")) continue;
        else if (argv[a][0] != '-') name = argv[a];
    }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("column", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);

    uint32_t widest = 0;
    for (uint32_t i = 0; i < nl; i++) {
        uint32_t k = 0;
        while (k < cu_line_len[i] && (char)d[cu_line_off[i] + k] != delim) k++;
        if (k > widest) widest = k;
    }
    for (uint32_t i = 0; i < nl; i++) {
        const uint8_t *L = d + cu_line_off[i];
        uint32_t ln = cu_line_len[i], k = 0;
        while (k < ln && (char)L[k] != delim) { term_putc((char)L[k]); k++; }
        for (uint32_t pad = k; pad <= widest; pad++) term_putc(' ');
        while (k < ln && (char)L[k] == delim) k++;
        for (; k < ln; k++) term_putc((char)L[k]);
        term_putc('\n');
    }
}

/* fmt: reflow a paragraph to a width. */
static void cu_cmd_fmt(int argc, char **argv) {
    int width = 72;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-w") && a + 1 < argc) width = cu_atoi(argv[++a], 72);
        else if (argv[a][0] == '-' && argv[a][1] >= '0' && argv[a][1] <= '9')
            width = cu_atoi(argv[a] + 1, 72);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (width < 8) width = 8;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("fmt", cu_lasterr); return; }

    int col = 0;
    uint32_t i = 0;
    while (i < n) {
        while (i < n && cu_is_space((char)d[i])) i++;
        uint32_t start = i;
        while (i < n && !cu_is_space((char)d[i])) i++;
        uint32_t wlen = i - start;
        if (!wlen) break;
        if (col && col + 1 + (int)wlen > width) { term_putc('\n'); col = 0; }
        else if (col) { term_putc(' '); col++; }
        for (uint32_t k = 0; k < wlen; k++) term_putc((char)d[start + k]);
        col += (int)wlen;
    }
    if (col) term_putc('\n');
}

/* pr: paginate with a header. */
static void cu_cmd_pr(int argc, char **argv) {
    int lines = 60;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-l") && a + 1 < argc) lines = cu_atoi(argv[++a], 60);
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (lines < 5) lines = 5;

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("pr", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);

    int page = 1;
    for (uint32_t i = 0; i < nl; i++) {
        if (i % (uint32_t)(lines - 4) == 0) {
            if (i) term_putc('\n');
            term_print_c(name ? name : "-", 3);
            term_print_c("    Page ", 3);
            cu_put_num((uint32_t)page++, 1);
            term_print("\n\n");
        }
        cu_put_line(d + cu_line_off[i], cu_line_len[i]);
    }
}

/* csplit: break a file at every line matching a pattern. */
static void cu_cmd_csplit(int argc, char **argv) {
    if (argc < 3) { cu_usage("csplit <file> <pattern> [prefix]"); return; }
    const char *name = argv[1], *pat = argv[2];
    const char *prefix = argc > 3 ? argv[3] : "xx";

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("csplit", cu_lasterr); return; }
    uint32_t nl = cu_split_lines(d, n);

    static char part[CU_PIPE_MAX];
    uint32_t o = 0, made = 0;
    for (uint32_t i = 0; i < nl; i++) {
        int hit = cu_find_sub(d + cu_line_off[i], cu_line_len[i], pat, 0) >= 0;
        if (hit && o > 0) {
            char out[256], nb[16];
            str_copy(out, prefix, sizeof(out));
            uint_to_str(made++, nb);
            str_append(out, nb, sizeof(out));
            char abs[256];
            term_resolve(out, abs);
            if (fs_write_file(abs, part, o) != 0) { cu_err("csplit", fs_errstr); return; }
            term_print("wrote "); term_print(out); term_putc('\n');
            o = 0;
        }
        for (uint32_t k = 0; k < cu_line_len[i] && o < sizeof(part) - 2; k++)
            part[o++] = (char)d[cu_line_off[i] + k];
        if (o < sizeof(part) - 1) part[o++] = '\n';
    }
    if (o > 0) {
        char out[256], nb[16];
        str_copy(out, prefix, sizeof(out));
        uint_to_str(made, nb);
        str_append(out, nb, sizeof(out));
        char abs[256];
        term_resolve(out, abs);
        if (fs_write_file(abs, part, o) != 0) cu_err("csplit", fs_errstr);
        else { term_print("wrote "); term_print(out); term_putc('\n'); }
    }
}

/* dd: copy with an offset and a count, in bytes rather than blocks --
 * there is no device to address by block here. */
static void cu_cmd_dd(int argc, char **argv) {
    const char *in = 0, *out = 0;
    uint32_t skip = 0, count = 0;
    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (s[0]=='i'&&s[1]=='f'&&s[2]=='=') in = s + 3;
        else if (s[0]=='o'&&s[1]=='f'&&s[2]=='=') out = s + 3;
        else if (s[0]=='s'&&s[1]=='k'&&s[2]=='i'&&s[3]=='p'&&s[4]=='=')
            skip = (uint32_t)cu_atoi(s + 5, 0);
        else if (s[0]=='c'&&s[1]=='o'&&s[2]=='u'&&s[3]=='n'&&s[4]=='t'&&s[5]=='=')
            count = (uint32_t)cu_atoi(s + 6, 0);
    }
    if (!in) { cu_usage("dd if=<file> [of=<file>] [skip=n] [count=n]  (bytes)"); return; }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(in, &d, &n)) { cu_err("dd", cu_lasterr); return; }
    if (skip >= n) { cu_err("dd", "skip is past the end"); return; }
    uint32_t avail = n - skip;
    if (count == 0 || count > avail) count = avail;

    if (!out) {
        for (uint32_t i = 0; i < count; i++) term_putc((char)d[skip + i]);
        return;
    }
    char abs[256];
    term_resolve(out, abs);
    if (fs_write_file(abs, d + skip, count) != 0) { cu_err("dd", fs_errstr); return; }
    cu_put_num(count, 1);
    term_print(" bytes copied\n");
}

/* shred: overwrite before unlinking. Honest about what that means on a
 * flash-backed image with no control over remapping. */
static void cu_cmd_shred(int argc, char **argv) {
    if (argc < 2) { cu_usage("shred <file>"); return; }
    char abs[256];
    term_resolve(argv[1], abs);
    uint64_t sz = 0;
    int is_dir = 0;
    if (!fs_stat(abs, &sz, &is_dir) || is_dir) { cu_err("shred", "no such file"); return; }
    if (sz > CU_CMP_MAX) { cu_err("shred", "over 512 KiB"); return; }

    for (uint32_t pass = 0; pass < 3; pass++) {
        for (uint32_t i = 0; i < (uint32_t)sz; i++)
            cu_cmp_buf[i] = (uint8_t)(pass == 2 ? 0 : (pass ? 0x55 : 0xAA));
        if (fs_write_file(abs, cu_cmp_buf, (uint32_t)sz) != 0) {
            cu_err("shred", fs_errstr);
            return;
        }
    }
    fs_delete(abs);
    term_print("shredded (3 passes) -- note the underlying media may still\n");
    term_print("hold the old blocks; this cannot control remapping.\n");
}

/* dos2unix / unix2dos */
static void cu_cmd_crlf(int argc, char **argv, int to_dos) {
    if (argc < 2) { cu_usage(to_dos ? "unix2dos <file>" : "dos2unix <file>"); return; }
    const uint8_t *d;
    uint32_t n;
    if (!cu_src(argv[1], &d, &n)) { cu_err("dos2unix", cu_lasterr); return; }

    static uint8_t buf[CU_CMP_MAX];
    uint32_t o = 0;
    for (uint32_t i = 0; i < n && o < CU_CMP_MAX - 2; i++) {
        if (d[i] == '\r') continue;
        if (d[i] == '\n' && to_dos) buf[o++] = '\r';
        buf[o++] = d[i];
    }
    char abs[256];
    term_resolve(argv[1], abs);
    if (fs_write_file(abs, buf, o) != 0) cu_err("dos2unix", fs_errstr);
}

/* ===== shell builtins ===== */

static void cu_cmd_printf(int argc, char **argv) {
    if (argc < 2) { cu_usage("printf <format> [args]"); return; }
    int nexta = 2;
    for (const char *f = argv[1]; *f; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            switch (*f) {
            case 'n': term_putc('\n'); break;
            case 't': term_putc('\t'); break;
            case 'r': term_putc('\r'); break;
            case '\\': term_putc('\\'); break;
            default: term_putc(*f); break;
            }
            continue;
        }
        if (*f == '%' && f[1]) {
            f++;
            if (*f == '%') { term_putc('%'); continue; }
            const char *arg = nexta < argc ? argv[nexta++] : "";
            if (*f == 's') term_print(arg);
            else if (*f == 'd' || *f == 'i' || *f == 'u') {
                int v = cu_atoi(arg, 0);
                if (v < 0) { term_putc('-'); v = -v; }
                cu_put_num((uint32_t)v, 1);
            } else if (*f == 'c') term_putc(arg[0]);
            else if (*f == 'x') {
                static const char hx[] = "0123456789abcdef";
                uint32_t v = (uint32_t)cu_atoi(arg, 0);
                int started = 0;
                for (int k = 7; k >= 0; k--) {
                    int nib = (v >> (k * 4)) & 15;
                    if (nib || started || k == 0) { term_putc(hx[nib]); started = 1; }
                }
            } else term_putc(*f);
            continue;
        }
        term_putc(*f);
    }
}

static void cu_cmd_seq(int argc, char **argv) {
    if (argc < 2) { cu_usage("seq [first [incr]] last"); return; }
    int first = 1, incr = 1, last;
    if (argc == 2) last = cu_atoi(argv[1], 1);
    else if (argc == 3) { first = cu_atoi(argv[1], 1); last = cu_atoi(argv[2], 1); }
    else { first = cu_atoi(argv[1], 1); incr = cu_atoi(argv[2], 1); last = cu_atoi(argv[3], 1); }
    if (incr == 0) { cu_err("seq", "increment cannot be zero"); return; }

    int guard = 0;
    for (int v = first; (incr > 0 ? v <= last : v >= last) && guard < 100000; v += incr, guard++) {
        int t = v;
        if (t < 0) { term_putc('-'); t = -t; }
        cu_put_num((uint32_t)t, 1);
        term_putc('\n');
    }
}

static void cu_cmd_yes(int argc, char **argv) {
    /* Bounded: without a process to kill there is no way to stop an
     * unbounded one, and the render loop would never run again. */
    const char *word = argc >= 2 ? argv[1] : "y";
    for (int i = 0; i < 100; i++) { term_print(word); term_putc('\n'); }
    term_print_c("(stopped after 100 -- nothing here can interrupt a loop)\n", 3);
}

/*
 * test / [ -- the string and file predicates. Numeric comparison too,
 * since shell scripts lean on it.
 */
static void cu_cmd_test(int argc, char **argv) {
    int n = argc;
    if (n > 1 && str_eq(argv[n - 1], "]")) n--;      /* the `[` spelling */

    int result = 0;
    if (n == 2) {
        result = argv[1][0] != '\0';
    } else if (n == 3) {
        const char *op = argv[1], *a = argv[2];
        char abs[256];
        term_resolve(a, abs);
        uint64_t sz = 0;
        int is_dir = 0, exists = fs_stat(abs, &sz, &is_dir);
        if (str_eq(op, "-e")) result = exists;
        else if (str_eq(op, "-f")) result = exists && !is_dir;
        else if (str_eq(op, "-d")) result = exists && is_dir;
        else if (str_eq(op, "-s")) result = exists && sz > 0;
        else if (str_eq(op, "-r") || str_eq(op, "-w")) result = exists;
        else if (str_eq(op, "-z")) result = a[0] == '\0';
        else if (str_eq(op, "-n")) result = a[0] != '\0';
        else if (str_eq(op, "!")) result = !(a[0] != '\0');
    } else if (n == 4) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (str_eq(op, "=") || str_eq(op, "==")) result = str_eq(a, b);
        else if (str_eq(op, "!=")) result = !str_eq(a, b);
        else {
            int x = cu_atoi(a, 0), y = cu_atoi(b, 0);
            if (str_eq(op, "-eq")) result = x == y;
            else if (str_eq(op, "-ne")) result = x != y;
            else if (str_eq(op, "-lt")) result = x < y;
            else if (str_eq(op, "-le")) result = x <= y;
            else if (str_eq(op, "-gt")) result = x > y;
            else if (str_eq(op, "-ge")) result = x >= y;
        }
    }
    term_print(result ? "true\n" : "false\n");
}

/* xargs: run one command with the piped words appended. */
static void term_exec(char *cmdline);      /* defined below; see term.h */

static void cu_cmd_xargs(int argc, char **argv) {
    if (!cu_pipe_ready) { cu_err("xargs", "nothing piped in"); return; }
    if (argc < 2) { cu_usage("<cmd> | xargs <command>"); return; }

    char line[512];
    uint32_t i = 0;
    while (i < cu_pipe_len) {
        int o = 0;
        for (int a = 1; a < argc && o < 400; a++) {
            for (int k = 0; argv[a][k] && o < 400; k++) line[o++] = argv[a][k];
            line[o++] = ' ';
        }
        /* one line of input per invocation, which is the useful default
         * when the words are filenames */
        while (i < cu_pipe_len && cu_pipe[i] != '\n' && o < 500)
            line[o++] = cu_pipe[i++];
        while (i < cu_pipe_len && cu_pipe[i] == '\n') i++;
        line[o] = '\0';

        int blank = 1;
        for (int k = 0; line[k]; k++) if (line[k] != ' ') { blank = 0; break; }
        if (!blank) {
            int saved = cu_pipe_ready;
            cu_pipe_ready = 0;             /* the child reads files, not us */
            term_exec(line);
            cu_pipe_ready = saved;
        }
    }
}

/* ===== system information ===== */

static void cu_cmd_hostname(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("vextro\n");
}

static void cu_cmd_arch(int argc, char **argv) {
    (void)argc; (void)argv;
#if defined(__aarch64__)
    term_print("aarch64\n");
#else
    term_print("x86_64\n");
#endif
}

static void cu_cmd_nproc(int argc, char **argv) {
    (void)argc; (void)argv;
    /* One, and it is not a guess: this kernel never starts a second CPU.
     * There is no SMP bring-up anywhere in the tree. */
    term_print("1\n");
}

static void cu_cmd_free(int argc, char **argv) {
    (void)argv;
    int kib = argc >= 2 && (str_eq(argv[1], "-k"));
    uint32_t total_mb = (uint32_t)system_total_memory_mb;
    term_print("               total\n");
    term_print("Mem:      ");
    if (kib) { cu_put_num(total_mb * 1024, 10); term_print(" KiB\n"); }
    else     { cu_put_num(total_mb, 10); term_print(" MiB\n"); }
    term_print_c("(no used/free split: this kernel has no allocator to ask)\n", 3);
}

/*
 * lspci -- and on aarch64, the honest equivalent.
 *
 * The `virt` machine this port targets has no PCI host bridge at all:
 * every device is virtio over MMIO at a fixed window, which is exactly
 * why the port never needed an ECAM walk. Listing an empty PCI bus there
 * would be true and useless; listing the devices that *are* present is
 * what someone typing lspci actually wants.
 */
#if defined(__aarch64__)

static void cu_cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("no PCI host bridge on this machine; virtio-mmio devices:\n");
    int found = 0;
    for (uint32_t i = 0; i < VIRTIO_MMIO_COUNT; i++) {
        uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_STRIDE;
        if (vio_rd(base, VIO_MAGIC) != VIO_MAGIC_VALUE) continue;
        uint32_t id = vio_rd(base, VIO_DEVICE_ID);
        if (id == 0) continue;
        const char *what = "unknown";
        switch (id) {
        case 1:  what = "network"; break;
        case 2:  what = "block"; break;
        case 3:  what = "console"; break;
        case 4:  what = "entropy"; break;
        case 16: what = "gpu"; break;
        case 18: what = "input"; break;
        case 19: what = "socket"; break;
        default: break;
        }
        term_print("  slot ");
        cu_put_num(i, 2);
        term_print("   id ");
        cu_put_num(id, 2);
        term_print("   ");
        term_print(what);
        term_putc('\n');
        found++;
    }
    if (!found) term_print_c("  (none)\n", 3);
}

#else

static int cu_lspci_cb(const pci_dev_t *dev, void *ctx) {
    (void)ctx;
    static const char hx[] = "0123456789abcdef";
    term_putc(hx[(dev->bus >> 4) & 15]); term_putc(hx[dev->bus & 15]);
    term_putc(':');
    term_putc(hx[(dev->slot >> 4) & 15]); term_putc(hx[dev->slot & 15]);
    term_putc('.');
    term_putc(hx[dev->func & 15]);
    term_print("  ");

    uint32_t cls = dev->class_code >> 16;
    const char *what = "device";
    switch (cls) {
    case 0x00: what = "unclassified"; break;
    case 0x01: what = "mass storage"; break;
    case 0x02: what = "network"; break;
    case 0x03: what = "display"; break;
    case 0x04: what = "multimedia"; break;
    case 0x05: what = "memory"; break;
    case 0x06: what = "bridge"; break;
    case 0x07: what = "communication"; break;
    case 0x08: what = "system peripheral"; break;
    case 0x09: what = "input"; break;
    case 0x0C: what = "serial bus"; break;
    default: break;
    }
    term_print(what);
    term_print("  [");
    for (int k = 3; k >= 0; k--) term_putc(hx[(dev->vendor >> (k * 4)) & 15]);
    term_putc(':');
    for (int k = 3; k >= 0; k--) term_putc(hx[(dev->device >> (k * 4)) & 15]);
    term_print("]\n");
    return 0;
}

static void cu_cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;
    pci_scan(cu_lspci_cb, 0);
}

#endif /* __aarch64__ */

static void cu_cmd_lsblk(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("NAME    BUS           SIZE\n");
    if (!blk_present()) { term_print_c("(no disk)\n", 3); return; }
    term_print("disk0   ");
    term_print(blk_bus_name());
    for (int i = 0; i < 14 - (int)0; i++) { }
    term_print("  ");
    cu_put_num((uint32_t)(blk_sectors() / 2048), 8);
    term_print(" MiB\n");
    if (fs_kind != FS_NONE) {
        term_print("  mounted ");
        term_print(fs_name());
        term_print(", ");
        cu_put_num(fs_free_kb() / 1024, 1);
        term_print(" MiB free of ");
        cu_put_num(fs_total_kb() / 1024, 1);
        term_print(" MiB\n");
    }
}

static void cu_cmd_lscpu(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Architecture:   ");
#if defined(__aarch64__)
    term_print("aarch64\n");
    term_print("Byte order:     Little Endian\n");
#else
    term_print("x86_64\n");
    term_print("Byte order:     Little Endian\n");
#endif
    term_print("CPU(s):         1   (no SMP bring-up in this kernel)\n");
    term_print("Privilege:      supervisor throughout; no user mode\n");
}

static void cu_cmd_lsmem(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("Total online memory: ");
    cu_put_num((uint32_t)system_total_memory_mb, 1);
    term_print(" MiB\n");
}

/* cal: a month, computed rather than looked up. */
static void cu_cmd_cal(int argc, char **argv) {
    int hh, mm, ss, day, mon, yr;
    rtc_read(&hh, &mm, &ss, &day, &mon, &yr);
    if (argc >= 3) { mon = cu_atoi(argv[1], mon); yr = cu_atoi(argv[2], yr); }
    else if (argc == 2) { yr = cu_atoi(argv[1], yr); mon = 1; }
    if (mon < 1) mon = 1;
    if (mon > 12) mon = 12;

    static const char *names[13] = { "", "January", "February", "March",
        "April", "May", "June", "July", "August", "September", "October",
        "November", "December" };
    static const int mdays[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };

    int leap = (yr % 4 == 0 && yr % 100 != 0) || (yr % 400 == 0);
    int dim = mdays[mon] + ((mon == 2 && leap) ? 1 : 0);

    /* Zeller's congruence for the weekday of the 1st. */
    int m = mon, y = yr;
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (1 + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    int first = (h + 6) % 7;                 /* 0 = Sunday */

    term_print("    ");
    term_print(names[mon]);
    term_print(" ");
    cu_put_num((uint32_t)yr, 1);
    term_print("\nSu Mo Tu We Th Fr Sa\n");
    for (int i = 0; i < first; i++) term_print("   ");
    for (int d = 1; d <= dim; d++) {
        if (d < 10) term_putc(' ');
        cu_put_num((uint32_t)d, 1);
        if ((first + d) % 7 == 0) term_putc('\n');
        else term_putc(' ');
    }
    term_putc('\n');
}

static void cu_cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;
    if (blk_flush() == 0) term_print("flushed\n");
    else term_print_c("sync: the device reported an error\n", 2);
}

static void cu_cmd_true_false(int is_true) {
    term_print(is_true ? "true\n" : "false\n");
}

/* ===== networking ===== */

static void cu_cmd_ifcfg_extra(int argc, char **argv) {
    (void)argc; (void)argv;
    char ip[20];
    term_print("Interface  net0\n");
    if (!e1000_found) { term_print_c("  no adapter found\n", 2); return; }
    ip_to_str(net_our_ip, ip);
    term_print("  address  "); term_print(ip); term_putc('\n');
    ip_to_str(net_gw_ip, ip);
    term_print("  gateway  "); term_print(ip); term_putc('\n');
    ip_to_str(net_dns_ip, ip);
    term_print("  dns      "); term_print(ip); term_putc('\n');
}

static void cu_cmd_route(int argc, char **argv) {
    (void)argc; (void)argv;
    char ip[20];
    term_print("Destination     Gateway         Iface\n");
    if (!e1000_found) { term_print_c("(no adapter)\n", 3); return; }
    ip_to_str(net_gw_ip, ip);
    term_print("default         ");
    term_print(ip);
    for (int i = 0; i < 16 - 15; i++) term_putc(' ');
    term_print("   net0\n");
    ip_to_str(net_our_ip, ip);
    term_print("10.0.2.0/24     -               net0\n");
}


/* ===== archives =====
 *
 * The decoders were already here: zstd.h reads the app store's packages
 * and lzma.h reads the picture format. What was missing was any way to
 * reach them from a shell. Compression is not offered -- these are
 * decoders, and a `gzip` that could not gzip would be a worse lie than
 * an absent one.
 */

#define CU_UNZ_MAX (4 * 1024 * 1024)
static uint8_t cu_unz[CU_UNZ_MAX];

/* tar: list or extract a ustar archive, which is the format the ramdisk
 * already uses, so the header walk is the same shape as tarfs's. */
static void cu_cmd_tar(int argc, char **argv) {
    int list = 0, extract = 0;
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-' || a == 1) {
            for (int k = 0; argv[a][k]; k++) {
                if (argv[a][k] == 't') list = 1;
                if (argv[a][k] == 'x') extract = 1;
            }
            if (argv[a][0] != '-' && !list && !extract) name = argv[a];
        } else name = argv[a];
    }
    if (!list && !extract) list = 1;
    if (!name) { cu_usage("tar -t|-x <archive.tar>"); return; }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("tar", cu_lasterr); return; }

    uint32_t off = 0, count = 0;
    while (off + 512 <= n) {
        const uint8_t *h = d + off;
        if (h[0] == '\0') break;                      /* end-of-archive */

        char fname[128];
        int k = 0;
        while (k < 99 && h[k]) { fname[k] = (char)h[k]; k++; }
        fname[k] = '\0';

        /* size is octal, in the field at offset 124 */
        uint64_t size = 0;
        for (int i = 124; i < 136 && h[i]; i++) {
            if (h[i] < '0' || h[i] > '7') break;
            size = size * 8 + (uint64_t)(h[i] - '0');
        }
        char type = (char)h[156];

        if (list) {
            cu_put_num((uint32_t)size, 10);
            term_print("  ");
            term_print_c(fname, type == '5' ? 4 : 0);
            term_putc('\n');
        } else if (extract && (type == '0' || type == '\0')) {
            char abs[256];
            term_resolve(fname, abs);
            if (off + 512 + size <= n) {
                if (fs_write_file(abs, d + off + 512, (uint32_t)size) != 0)
                    cu_err("tar", fs_errstr);
                else { term_print("x "); term_print(fname); term_putc('\n'); }
            }
        }
        count++;
        off += 512 + ((uint32_t)size + 511) / 512 * 512;
    }
    if (list) { cu_put_num(count, 1); term_print(" entries\n"); }
}

/* One decompressor front end, three formats. `to_file` writes beside the
 * source with the suffix removed; otherwise it goes to the terminal. */
static void cu_unpack(int argc, char **argv, int fmt, int to_file) {
    const char *name = 0;
    for (int a = 1; a < argc; a++) {
        if (str_eq(argv[a], "-c")) to_file = 0;
        else if (str_eq(argv[a], "-d")) continue;
        else if (argv[a][0] != '-') name = argv[a];
    }
    if (!name) { cu_usage("unzstd|unxz|zcat [-c] <file>"); return; }

    const uint8_t *d;
    uint32_t n;
    if (!cu_src(name, &d, &n)) { cu_err("unzstd", cu_lasterr); return; }

    uint64_t out_len = 0;
    const char *err = 0;
    int rc;
    if (fmt == 0) rc = zstd_decode(d, n, cu_unz, CU_UNZ_MAX, &out_len, &err);
    else          rc = lzma_alone_decode(d, n, cu_unz, CU_UNZ_MAX, &out_len, &err);

    if (rc != 0 || out_len == 0) {
        cu_err(fmt == 0 ? "unzstd" : "unxz", err ? err : "not a valid stream");
        return;
    }

    if (!to_file) {
        for (uint64_t i = 0; i < out_len; i++) term_putc((char)cu_unz[i]);
        return;
    }
    /* strip a known suffix so the result does not keep it */
    char out[256];
    str_copy(out, name, sizeof(out));
    int L = 0; while (out[L]) L++;
    static const char *sfx[] = { ".zst", ".zstd", ".xz", ".lzma", 0 };
    for (int i = 0; sfx[i]; i++) {
        int sl = 0; while (sfx[i][sl]) sl++;
        if (L > sl) {
            int m = 0;
            while (m < sl && out[L - sl + m] == sfx[i][m]) m++;
            if (m == sl) { out[L - sl] = '\0'; break; }
        }
    }
    char abs[256];
    term_resolve(out, abs);
    if (fs_write_file(abs, cu_unz, (uint32_t)out_len) != 0) {
        cu_err("unzstd", fs_errstr);
        return;
    }
    cu_put_num((uint32_t)out_len, 1);
    term_print(" bytes -> ");
    term_print(out);
    term_putc('\n');
}

static void cu_cmd_gzip_note(int argc, char **argv) {
    (void)argc; (void)argv;
    /* DEFLATE is the one codec this tree does not carry: the app store
     * uses zstd and the picture format uses LZMA, so nothing ever needed
     * it. Saying that is more use than a command that fails obscurely. */
    cu_err("gzip", "no DEFLATE decoder in this build; zstd and xz work");
}

/* ===== permissions and identity ===== */

static void cu_cmd_id(int argc, char **argv) {
    (void)argc; (void)argv;
    if (user_current < 0) { term_print("nobody\n"); return; }
    term_print("uid=");
    cu_put_num((uint32_t)user_current, 1);
    term_print("(");
    term_print(user_name_of(user_current));
    term_print(")  groups=");
    term_print(user_is_admin(user_current) ? "admin" : "users");
    term_putc('\n');
}

static void cu_cmd_groups(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Two groups, and they are the two the account flag can express.
     * Inventing a group database would be inventing a feature. */
    term_print(user_is_admin(user_current) ? "admin users\n" : "users\n");
}

static void cu_cmd_chmod(int argc, char **argv) {
    (void)argc; (void)argv;
    cu_err("chmod", "exFAT and FAT32 store no mode bits");
}

static void cu_cmd_chown(int argc, char **argv) {
    (void)argc; (void)argv;
    cu_err("chown", "exFAT and FAT32 store no owner");
}

static void cu_cmd_umask(int argc, char **argv) {
    (void)argc; (void)argv;
    term_print("0000   (no mode bits on this filesystem)\n");
}

/* ===== help ===== */

typedef struct { const char *name; const char *what; } cu_doc_t;

static const cu_doc_t cu_docs[] = {
    { "basename", "strip the directory from a path" },
    { "base64",   "encode a file; base64d decodes" },
    { "cal",      "print a month" },
    { "cat",      "print a file" },
    { "cksum",    "CRC-32 and byte count" },
    { "column",   "line a table up on its first field" },
    { "comm",     "compare two sorted files, three columns" },
    { "cmp",      "report the first byte at which two files differ" },
    { "csplit",   "split a file at every line matching a pattern" },
    { "cut",      "select fields or characters from each line" },
    { "dd",       "copy bytes with an offset and a count" },
    { "diff",     "line differences between two files" },
    { "dirname",  "strip the last component from a path" },
    { "dos2unix", "strip CR from line endings; unix2dos adds them" },
    { "du",       "recursive size of a directory" },
    { "expand",   "tabs to spaces; unexpand goes back" },
    { "file",     "identify a file by its magic" },
    { "find",     "walk a tree, -name glob and -type f|d" },
    { "fmt",      "reflow text to a width" },
    { "fold",     "hard-wrap at a column" },
    { "free",     "total memory" },
    { "grep",     "print matching lines; -i -v -n -c -l" },
    { "head",     "first lines of a file" },
    { "hexdump",  "hex and ASCII; xxd and od are the same tool" },
    { "hostname", "the machine name" },
    { "id",       "the current account and its group" },
    { "lsblk",    "disks and the volume mounted" },
    { "lscpu",    "processor summary" },
    { "lspci",    "walk the PCI bus" },
    { "nl",       "number the lines" },
    { "paste",    "two files side by side" },
    { "pr",       "paginate with a header" },
    { "printf",   "format and print; %s %d %x %c and escapes" },
    { "realpath", "resolve a path to an absolute one" },
    { "rev",      "reverse each line" },
    { "sed",      "s/old/new/[g], /pat/p, /pat/d" },
    { "seq",      "print a range of numbers" },
    { "sha256sum","SHA-256 of a file" },
    { "shred",    "overwrite then delete" },
    { "sort",     "sort lines; -r -u -n -f" },
    { "split",    "break a file into numbered parts" },
    { "stat",     "size and type of a file" },
    { "strings",  "printable runs inside a binary" },
    { "sync",     "flush the disk cache" },
    { "tac",      "print lines in reverse order" },
    { "tail",     "last lines of a file" },
    { "tar",      "-t lists a ustar archive, -x extracts it" },
    { "tee",      "write a pipeline to a file and to the screen" },
    { "test",     "file and string predicates; also spelled [" },
    { "tr",       "translate or delete characters; ranges work" },
    { "tree",     "the directory tree" },
    { "truncate", "set a file's length" },
    { "uniq",     "collapse adjacent duplicate lines; -c -d -u" },
    { "unzstd",   "decompress zstd; unxz decompresses LZMA" },
    { "wc",       "count lines, words and bytes" },
    { "xargs",    "run a command once per piped line" },
    { 0, 0 }
};

static void cu_cmd_man(int argc, char **argv) {
    if (argc < 2) { cu_usage("man <command>   (try `apropos <word>`)"); return; }
    for (int i = 0; cu_docs[i].name; i++) {
        if (str_eq(cu_docs[i].name, argv[1])) {
            term_print_c(cu_docs[i].name, 3);
            term_print(" -- ");
            term_print(cu_docs[i].what);
            term_putc('\n');
            return;
        }
    }
    cu_err("man", "no entry; `help` lists the built-ins");
}

static void cu_cmd_whatis(int argc, char **argv) { cu_cmd_man(argc, argv); }

static void cu_cmd_apropos(int argc, char **argv) {
    if (argc < 2) { cu_usage("apropos <word>"); return; }
    int hits = 0;
    for (int i = 0; cu_docs[i].name; i++) {
        uint32_t nl = 0; while (cu_docs[i].what[nl]) nl++;
        uint32_t namelen = 0; while (cu_docs[i].name[namelen]) namelen++;
        if (cu_find_sub((const uint8_t *)cu_docs[i].what, nl, argv[1], 1) >= 0 ||
            cu_find_sub((const uint8_t *)cu_docs[i].name, namelen,
                        argv[1], 1) >= 0) {
            term_print_c(cu_docs[i].name, 3);
            term_print(" -- ");
            term_print(cu_docs[i].what);
            term_putc('\n');
            hits++;
        }
    }
    if (!hits) term_print_c("nothing matches\n", 3);
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

    /* more text */
    if (str_eq(cmd, "sed"))       { cu_cmd_sed(argc, argv); return 1; }
    if (str_eq(cmd, "comm"))      { cu_cmd_comm(argc, argv); return 1; }
    if (str_eq(cmd, "paste"))     { cu_cmd_paste(argc, argv); return 1; }
    if (str_eq(cmd, "column"))    { cu_cmd_column(argc, argv); return 1; }
    if (str_eq(cmd, "fmt"))       { cu_cmd_fmt(argc, argv); return 1; }
    if (str_eq(cmd, "pr"))        { cu_cmd_pr(argc, argv); return 1; }
    if (str_eq(cmd, "csplit"))    { cu_cmd_csplit(argc, argv); return 1; }
    if (str_eq(cmd, "dd"))        { cu_cmd_dd(argc, argv); return 1; }
    if (str_eq(cmd, "shred"))     { cu_cmd_shred(argc, argv); return 1; }
    if (str_eq(cmd, "dos2unix"))  { cu_cmd_crlf(argc, argv, 0); return 1; }
    if (str_eq(cmd, "unix2dos"))  { cu_cmd_crlf(argc, argv, 1); return 1; }

    /* shell builtins */
    if (str_eq(cmd, "printf"))    { cu_cmd_printf(argc, argv); return 1; }
    if (str_eq(cmd, "seq"))       { cu_cmd_seq(argc, argv); return 1; }
    if (str_eq(cmd, "yes"))       { cu_cmd_yes(argc, argv); return 1; }
    if (str_eq(cmd, "test") || str_eq(cmd, "["))
                                  { cu_cmd_test(argc, argv); return 1; }
    if (str_eq(cmd, "xargs"))     { cu_cmd_xargs(argc, argv); return 1; }
    if (str_eq(cmd, "true"))      { cu_cmd_true_false(1); return 1; }
    if (str_eq(cmd, "false"))     { cu_cmd_true_false(0); return 1; }

    /* system information */
    if (str_eq(cmd, "hostname"))  { cu_cmd_hostname(argc, argv); return 1; }
    if (str_eq(cmd, "arch"))      { cu_cmd_arch(argc, argv); return 1; }
    if (str_eq(cmd, "nproc"))     { cu_cmd_nproc(argc, argv); return 1; }
    if (str_eq(cmd, "free"))      { cu_cmd_free(argc, argv); return 1; }
    if (str_eq(cmd, "lspci"))     { cu_cmd_lspci(argc, argv); return 1; }
    if (str_eq(cmd, "lsblk"))     { cu_cmd_lsblk(argc, argv); return 1; }
    if (str_eq(cmd, "lscpu"))     { cu_cmd_lscpu(argc, argv); return 1; }
    if (str_eq(cmd, "lsmem"))     { cu_cmd_lsmem(argc, argv); return 1; }
    if (str_eq(cmd, "cal"))       { cu_cmd_cal(argc, argv); return 1; }
    if (str_eq(cmd, "sync"))      { cu_cmd_sync(argc, argv); return 1; }

    /* networking */
    if (str_eq(cmd, "ip") || str_eq(cmd, "addr"))
                                  { cu_cmd_ifcfg_extra(argc, argv); return 1; }
    if (str_eq(cmd, "route") || str_eq(cmd, "netstat"))
                                  { cu_cmd_route(argc, argv); return 1; }

    if (str_eq(cmd, "tar"))       { cu_cmd_tar(argc, argv); return 1; }
    if (str_eq(cmd, "unzstd") || str_eq(cmd, "zstdcat"))
                                  { cu_unpack(argc, argv, 0, 1); return 1; }
    if (str_eq(cmd, "unxz") || str_eq(cmd, "xzcat") || str_eq(cmd, "unlzma"))
                                  { cu_unpack(argc, argv, 1, 1); return 1; }
    if (str_eq(cmd, "gzip") || str_eq(cmd, "gunzip") || str_eq(cmd, "zcat"))
                                  { cu_cmd_gzip_note(argc, argv); return 1; }

    /* identity and permissions */
    if (str_eq(cmd, "id"))        { cu_cmd_id(argc, argv); return 1; }
    if (str_eq(cmd, "groups"))    { cu_cmd_groups(argc, argv); return 1; }
    if (str_eq(cmd, "chmod"))     { cu_cmd_chmod(argc, argv); return 1; }
    if (str_eq(cmd, "chown") || str_eq(cmd, "chgrp"))
                                  { cu_cmd_chown(argc, argv); return 1; }
    if (str_eq(cmd, "umask"))     { cu_cmd_umask(argc, argv); return 1; }

    /* help */
    if (str_eq(cmd, "man"))       { cu_cmd_man(argc, argv); return 1; }
    if (str_eq(cmd, "whatis"))    { cu_cmd_whatis(argc, argv); return 1; }
    if (str_eq(cmd, "apropos"))   { cu_cmd_apropos(argc, argv); return 1; }

    return 0;
}


#endif /* COREUTILS_H */
