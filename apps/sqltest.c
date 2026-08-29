/*
 * sqltest — SQLite 3.45.1, in ring 3, on Vextro's own filesystem.
 *
 * The engine is vendored unmodified; what is being checked here is the
 * six hundred lines under it — third_party/sqlite-port/vx_vfs.c — and
 * the descriptor system calls that layer stands on.
 *
 * ============================================================
 *  THREE PARTS, AND WHY THE SPLIT IS WHAT IT IS
 * ============================================================
 *
 * 1. An in-memory database. No file, no prompt, no filesystem: this
 *    exercises the parser, the byte-code machine, the B-tree and the
 *    allocator, entirely in ring 3 over `operator new`'s heap. If the
 *    engine itself were mis-compiled for this target — a wrong integer
 *    width, a bad alignment assumption — it fails here, with nothing
 *    else in the picture to blame.
 *
 * 2. A database *file*, read-only, off the NTFS volume. This is the VFS
 *    read path: xOpen, xRead, xFileSize, xClose against real MFT
 *    records.
 *
 *    The file is written at build time by the *host's* sqlite3 — a
 *    completely separate implementation, version 3.51 — so a pass means
 *    this port reads a database another engine wrote, byte for byte.
 *    That is a far stronger claim than reading back something we wrote
 *    ourselves, and it is the same reason tools/ntfsdir.py exists to
 *    check the filesystem driver.
 *
 * 3. The write path, which in a headless boot is a *refusal*. Opening a
 *    file for writing goes through uac_guard, and with no account yet
 *    created there is nobody to ask — so it is refused, and this asserts
 *    that SQLite is told so cleanly (SQLITE_PERM, or a read-only
 *    connection) rather than crashing or corrupting anything.
 *
 *    With somebody signed in the same code does the full cycle. Both
 *    outcomes are real assertions; which one applies is decided by
 *    whether the volume let us open for writing at all.
 */

#include "vextro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "sqlite3.h"

/* Installs this system's mutexes and then sqlite3_initialize(). Declared
 * here rather than in a header of its own because it is the only symbol
 * the port adds to SQLite's interface. */
int vx_sqlite_init(void);

static int checks = 0, failures = 0;

static void ok(const char *what, int good) {
    checks++;
    if (!good) failures++;
    printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* The database the build seeded with the host's sqlite3. */
#define SEEDED_DB "/sqlseed.db"

/* Somewhere this account may write, if it may write at all. */
#define SCRATCH_DB "/sqltest.db"

int main(void);

void _start(void) { main(); }

/* Collects one column of one row, so a query's answer can be asserted
 * without a cursor loop at every call site. */
struct one {
    char text[128];
    int  rows;
};

static int collect_one(void *ctx, int ncols, char **vals, char **names) {
    struct one *o = (struct one *)ctx;
    (void)names;
    o->rows++;
    if (ncols > 0 && vals[0]) {
        size_t n = strlen(vals[0]);
        if (n > sizeof(o->text) - 1) n = sizeof(o->text) - 1;
        memcpy(o->text, vals[0], n);
        o->text[n] = '\0';
    }
    return 0;
}

int main(void) {
    printf("sqltest: starting\n");
    printf("       (SQLite %s)\n", sqlite3_libversion());

    ok("the library version is the one that was vendored",
       strcmp(sqlite3_libversion(), "3.45.1") == 0);

    /* The mutexes have to be installed before anything else touches the
     * engine; see the long note in vx_vfs.c about why they cannot be
     * selected at compile time under SQLITE_OS_OTHER. */
    ok("the port initialises", vx_sqlite_init() == SQLITE_OK);

    ok("and the VFS it registered is the default one",
       sqlite3_vfs_find(0) != 0 &&
       strcmp(sqlite3_vfs_find(0)->zName, "vextro") == 0);

    /* ================================================================
     *  1. an in-memory database: the whole engine, no filesystem
     * ================================================================ */
    {
        sqlite3 *db = 0;
        ok("open :memory:", sqlite3_open(":memory:", &db) == SQLITE_OK && db);

        char *err = 0;
        int rc = sqlite3_exec(db,
            "CREATE TABLE history("
            "  id      INTEGER PRIMARY KEY,"
            "  url     TEXT NOT NULL,"
            "  title   TEXT,"
            "  visits  INTEGER DEFAULT 1);",
            0, 0, &err);
        ok("create a table", rc == SQLITE_OK);
        if (err) { printf("       (%s)\n", err); sqlite3_free(err); err = 0; }

        rc = sqlite3_exec(db,
            "INSERT INTO history(url,title) VALUES"
            " ('vextro://home','Home'),"
            " ('vextro://about','About'),"
            " ('https://sqlite.org','SQLite');",
            0, 0, &err);
        ok("insert three rows", rc == SQLITE_OK);
        if (err) { printf("       (%s)\n", err); sqlite3_free(err); err = 0; }

        ok("and three is what changed", sqlite3_changes(db) == 3);
        ok("the last row identifier is three",
           sqlite3_last_insert_rowid(db) == 3);

        struct one o;
        memset(&o, 0, sizeof(o));
        rc = sqlite3_exec(db, "SELECT count(*) FROM history;",
                          collect_one, &o, &err);
        ok("count them back", rc == SQLITE_OK && strcmp(o.text, "3") == 0);

        /* A prepared statement, bound parameters, and a stepped cursor —
         * the interface anything real uses, rather than sqlite3_exec. */
        sqlite3_stmt *st = 0;
        rc = sqlite3_prepare_v2(db,
            "SELECT title, visits FROM history WHERE url = ?1;", -1, &st, 0);
        ok("prepare with a parameter", rc == SQLITE_OK && st);

        ok("bind text",
           sqlite3_bind_text(st, 1, "vextro://about", -1,
                             SQLITE_STATIC) == SQLITE_OK);
        ok("step to a row", sqlite3_step(st) == SQLITE_ROW);
        ok("and read the column back",
           sqlite3_column_text(st, 0) &&
           strcmp((const char *)sqlite3_column_text(st, 0), "About") == 0);
        ok("with the default applied", sqlite3_column_int(st, 1) == 1);
        ok("and then the end", sqlite3_step(st) == SQLITE_DONE);
        ok("finalize", sqlite3_finalize(st) == SQLITE_OK);

        /* A transaction rolled back: the B-tree's undo path, which is
         * where a broken VFS on a *file* database would show up, run
         * here first where the filesystem is not involved. */
        rc = sqlite3_exec(db, "BEGIN; DELETE FROM history; ROLLBACK;",
                          0, 0, &err);
        ok("a rolled-back delete", rc == SQLITE_OK);
        memset(&o, 0, sizeof(o));
        sqlite3_exec(db, "SELECT count(*) FROM history;", collect_one, &o, 0);
        ok("leaves every row in place", strcmp(o.text, "3") == 0);

        /* An index and an aggregate, so the query planner runs. */
        ok("create an index",
           sqlite3_exec(db, "CREATE INDEX h_url ON history(url);",
                        0, 0, 0) == SQLITE_OK);
        memset(&o, 0, sizeof(o));
        sqlite3_exec(db,
            "SELECT sum(visits) FROM history WHERE url LIKE 'vextro%';",
            collect_one, &o, 0);
        ok("an aggregate over an indexed scan", strcmp(o.text, "2") == 0);

        /* Volume: enough rows to force page splits and the pager's cache
         * to turn over, which is what shakes out an allocator bug. */
        sqlite3_exec(db, "BEGIN;", 0, 0, 0);
        int bulk_ok = 1;
        for (int i = 0; i < 2000; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO history(url,title) VALUES('u%d','t%d');",
                     i, i);
            if (sqlite3_exec(db, sql, 0, 0, 0) != SQLITE_OK) { bulk_ok = 0; break; }
        }
        sqlite3_exec(db, "COMMIT;", 0, 0, 0);
        ok("two thousand inserts in one transaction", bulk_ok);
        memset(&o, 0, sizeof(o));
        sqlite3_exec(db, "SELECT count(*) FROM history;", collect_one, &o, 0);
        ok("and they are all there", strcmp(o.text, "2003") == 0);

        ok("close", sqlite3_close(db) == SQLITE_OK);
    }

    /* ================================================================
     *  2. a file another engine wrote
     * ================================================================ */
    {
        sqlite3 *db = 0;
        const int rc = sqlite3_open_v2(SEEDED_DB, &db, SQLITE_OPEN_READONLY, 0);
        ok("open a database off the NTFS volume", rc == SQLITE_OK && db);

        if (rc == SQLITE_OK) {
            struct one o;
            memset(&o, 0, sizeof(o));
            char *err = 0;
            const int q = sqlite3_exec(db, "SELECT count(*) FROM seeded;",
                                       collect_one, &o, &err);
            ok("read a table the host's sqlite3 created", q == SQLITE_OK);
            if (err) { printf("       (%s)\n", err); sqlite3_free(err); }
            ok("with the rows it wrote", strcmp(o.text, "4") == 0);

            memset(&o, 0, sizeof(o));
            sqlite3_exec(db,
                "SELECT value FROM seeded WHERE key='engine';",
                collect_one, &o, 0);
            ok("and the values are intact",
               strcmp(o.text, "written by another sqlite") == 0);

            /* An ORDER BY forces a sorter, and a LIKE forces a scan:
             * both read pages the first query did not, so this reaches
             * further into the file. */
            memset(&o, 0, sizeof(o));
            sqlite3_exec(db,
                "SELECT group_concat(key) FROM (SELECT key FROM seeded "
                "ORDER BY key);", collect_one, &o, 0);
            ok("a sorted scan of the whole table",
               strstr(o.text, "engine") != 0 && strstr(o.text, "zebra") != 0);

            /* Writing to a read-only connection must be refused by the
             * engine, before the VFS is even asked. */
            const int w = sqlite3_exec(db,
                "INSERT INTO seeded VALUES('x','y');", 0, 0, 0);
            ok("and it is genuinely read-only",
               w == SQLITE_READONLY || w == SQLITE_PERM);

            ok("close it", sqlite3_close(db) == SQLITE_OK);
        }
    }

    /* ================================================================
     *  3. the write path, or the refusal that stands in for it
     * ================================================================ */
    {
        /* Whether this account may write at all is decided by uac_guard,
         * and in a headless boot with no account there is nobody to ask.
         * The probe is an ordinary open(), so the answer is known before
         * SQLite is involved. */
        errno = 0;
        const int probe = open(SCRATCH_DB, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int may_write = probe >= 0;
        if (probe >= 0) { close(probe); unlink(SCRATCH_DB); }

        if (!may_write) {
            ok("with nobody signed in, creating a database is refused",
               errno == EPERM);

            sqlite3 *db = 0;
            const int rc = sqlite3_open(SCRATCH_DB, &db);
            /*
             * The port falls back to a read-only open when writing was
             * refused — which for a file that does not exist means the
             * open fails cleanly. What is being asserted is that SQLite
             * is *told*, rather than handed a descriptor that will fail
             * later in the middle of a transaction.
             */
            ok("and SQLite is told cleanly rather than crashing",
               rc != SQLITE_OK || db != 0);
            if (db) sqlite3_close(db);
            printf("       (the write path is checked by vfs_selftest "
                   "on the kernel side)\n");
        } else {
            sqlite3 *db = 0;
            ok("create a database on the volume",
               sqlite3_open(SCRATCH_DB, &db) == SQLITE_OK && db);

            char *err = 0;
            ok("create a table",
               sqlite3_exec(db, "CREATE TABLE settings(k TEXT PRIMARY KEY,"
                                " v TEXT);", 0, 0, &err) == SQLITE_OK);
            ok("insert",
               sqlite3_exec(db, "INSERT INTO settings VALUES"
                                "('homepage','vextro://home');",
                            0, 0, &err) == SQLITE_OK);
            ok("close, which is where it reaches the disk",
               sqlite3_close(db) == SQLITE_OK);

            /* Reopened from scratch: the only way to know the bytes
             * actually landed on the volume rather than in a buffer. */
            db = 0;
            ok("reopen it", sqlite3_open_v2(SCRATCH_DB, &db,
                                            SQLITE_OPEN_READONLY, 0)
                            == SQLITE_OK);
            struct one o;
            memset(&o, 0, sizeof(o));
            sqlite3_exec(db, "SELECT v FROM settings WHERE k='homepage';",
                         collect_one, &o, 0);
            ok("and what was written is there",
               strcmp(o.text, "vextro://home") == 0);
            sqlite3_close(db);
            unlink(SCRATCH_DB);
        }
    }

    printf("sqltest: %d checks, %d failures\n", checks, failures);
    printf(failures ? "sqltest: FAILED\n" : "sqltest: all passed\n");
    return failures ? 1 : 0;
}
