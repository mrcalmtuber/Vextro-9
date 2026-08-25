/*
 * tools/ntfs_test.c — the NTFS driver, against a real volume.
 *
 * A filesystem reader can be checked by reading something known. A
 * filesystem *writer* can only be checked by writing and then reading
 * back with different code than wrote it — and the only volume the
 * kernel has is the one it booted from, which is the worst possible
 * place to find out that an allocator is wrong.
 *
 * So this compiles src/fs/ntfs/ntfs_ops.c — the same source the
 * kernel runs — against a file, formats a scratch volume with
 * tools/mkntfs.py, and exercises it. The checks that matter are the
 * ones that catch a writer which is self-consistently wrong:
 *
 *   - the run list encoder is checked against the *decoder* in ntfs.h,
 *     including negative deltas, because an encoder and decoder broken
 *     the same way agree perfectly and produce a volume nothing else
 *     can read
 *   - every mutation is followed by a fresh mount, so nothing is
 *     confirmed from state the writer left in memory
 *   - the journal is replayed against a deliberately interrupted write
 */

#define NTFS_HOST_TEST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The module itself, not a pair of headers. Including the .c is
 * deliberate: the driver keeps its mount state and its journal in file
 * statics, and the checks below read them directly to confirm that a
 * write landed where it said it did. Linking the object instead would
 * hide exactly the state a filesystem test needs to see — and this way
 * the host suite and the kernel compile the same file. */
#include "fs/ntfs/ntfs_ops.c"

static int checks = 0;
static int fails = 0;

static void expect(int cond, const char *what) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

static void expect_eq(uint64_t got, uint64_t want, const char *what) {
    checks++;
    if (got == want) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s (got %llu, want %llu)\n", what,
           (unsigned long long)got, (unsigned long long)want);
}

static const char *IMG = 0;

static int remount(void) {
    ntfs_host_close();
    if (ntfs_host_open(IMG) != 0) return -1;
    ntfs.mounted = 0;
    ntfs_bmp.loaded = 0;
    /* ntfs_try returns 1 on success, not 0. */
    if (!ntfs_try(0)) return -1;
    return 0;
}

/* ===== 1. run list encoding, against our own decoder ===== */

static void test_runlist(void) {
    printf("\nrun lists\n");

    struct {
        ntfs_run_t runs[4];
        int n;
        const char *what;
    } cases[] = {
        { { {100, 8, 0} }, 1, "a single run" },
        { { {100, 8, 0}, {200, 4, 0} }, 2, "a forward second extent" },
        { { {200, 4, 0}, {100, 8, 0} }, 2,
          "a second extent *before* the first (negative delta)" },
        { { {1, 1, 0}, {1000000, 2, 0}, {5, 3, 0} }, 3,
          "a large jump forward then far back" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t buf[128];
        uint32_t n = ntfs_encode_runs(cases[c].runs, cases[c].n, buf,
                                      sizeof(buf));
        const uint8_t *p = buf, *end = buf + n;
        int64_t prev = 0;
        ntfs_run_t got;
        int i = 0, ok = (n > 0);

        while (ok && i < cases[c].n && ntfs_next_run(&p, end, &prev, &got)) {
            if (got.lcn != cases[c].runs[i].lcn ||
                got.length != cases[c].runs[i].length) ok = 0;
            i++;
        }
        if (i != cases[c].n) ok = 0;
        expect(ok, cases[c].what);
    }

    /* A run list that will not fit must report failure rather than
     * writing a truncated one, which decodes as a shorter file. */
    {
        ntfs_run_t r[4] = { {1, 1, 0}, {1000000, 1, 0}, {2, 1, 0}, {999999, 1, 0} };
        uint8_t tiny[4];
        expect_eq(ntfs_encode_runs(r, 4, tiny, sizeof(tiny)), 0,
                  "refuses  a run list that does not fit");
    }
}

/* ===== 2. mount and read what the formatter wrote ===== */

static void test_mount_and_read(void) {
    printf("\nmounting a formatted volume\n");

    expect(remount() == 0, "the volume mounts");
    expect_eq(ntfs.bytes_per_cluster, 4096, "cluster size is 4096");
    /* 4096 rather than Windows' usual 1024: a directory lives entirely
     * in its resident $INDEX_ROOT here, so the record size *is* the
     * directory capacity. See the note in tools/mkntfs.py. */
    expect_eq(ntfs.bytes_per_record, 4096, "MFT record size is 4096");

    /* Record 5 is always the root directory, and it must be a
     * directory with an index. */
    {
        uint8_t rec[4096];
        expect(ntfs_read_record(NTFS_MFT_ROOT, rec) == 0,
               "the root directory record reads");
        const ntfs_record_t *h = (const ntfs_record_t *)rec;
        expect(h->flags & 2, "and is flagged as a directory");
        expect(ntfs_find_attr(rec, NTFS_ATTR_INDEX_ROOT) != 0,
               "and carries an $INDEX_ROOT");
    }

    /* $Bitmap and $MFT must both be present and non-resident. */
    {
        uint8_t rec[4096];
        expect(ntfs_read_record(0, rec) == 0, "$MFT's own record reads");
        const ntfs_attr_t *a = ntfs_find_attr(rec, NTFS_ATTR_DATA);
        expect(a && a->non_resident, "$MFT has non-resident data");

        expect(ntfs_read_record(6, rec) == 0, "$Bitmap's record reads");
        a = ntfs_find_attr(rec, NTFS_ATTR_DATA);
        expect(a && a->non_resident, "$Bitmap has non-resident data");
    }

    expect(ntfs_bitmap_load() == 0, "the free-space bitmap loads");
    expect(ntfs_bmp.total_clusters > 0, "and reports a cluster count");
}

/* ===== 3. allocation ===== */

static void test_allocation(void) {
    uint64_t a = 0, b = 0, c = 0;

    printf("\ncluster and record allocation\n");

    expect(remount() == 0, "remounted");
    ntfs_journal_init(0, 16 * 1024 * 1024 / 512);

    expect_eq(ntfs_clusters_alloc(4, &a), NTFS_W_OK, "four clusters allocate");
    expect_eq(ntfs_clusters_alloc(4, &b), NTFS_W_OK, "four more allocate");
    expect(a != b, "and the two runs are different");
    expect(a + 4 <= b || b + 4 <= a, "and do not overlap");

    /* The bitmap must survive a remount: an allocator that only marks
     * its own memory hands the same clusters out twice after a reboot. */
    expect(remount() == 0, "remounted after allocating");
    expect_eq(ntfs_clusters_alloc(4, &c), NTFS_W_OK, "a third run allocates");
    expect(c != a && c != b, "and does not reuse either earlier run");

    /* Freeing returns the space. */
    expect_eq(ntfs_clusters_free(a, 4), NTFS_W_OK, "the first run frees");
    {
        uint64_t d = 0;
        expect(remount() == 0, "remounted after freeing");
        expect_eq(ntfs_clusters_alloc(4, &d), NTFS_W_OK, "a run allocates");
        expect_eq(d, a, "and reuses the freed clusters");
        ntfs_clusters_free(d, 4);
    }
    ntfs_clusters_free(b, 4);
    ntfs_clusters_free(c, 4);

    /* MFT records */
    {
        uint64_t n = 0;
        expect(remount() == 0, "remounted");
        expect_eq(ntfs_mft_alloc(&n), NTFS_W_OK, "an MFT record allocates");
        expect(n >= 16, "and it is past the reserved system records");
    }
}

/* ===== 4. create, read back, delete ===== */

static int read_back(const char *name, uint8_t *out, uint32_t cap,
                     uint64_t *size) {
    /* Walk the root index for the name, then read that record's data --
     * deliberately through ntfs.h's reader rather than anything the
     * writer left behind. */
    uint8_t dir[4096];
    if (ntfs_read_record(NTFS_MFT_ROOT, dir) != 0) return -1;

    ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir, NTFS_ATTR_INDEX_ROOT);
    if (!ir || ir->non_resident) return -1;

    uint8_t *node = (uint8_t *)ir + ir->u.res.value_offset + 16;
    uint8_t *p = node + (uint32_t)(node[0] | (node[1] << 8) |
                                   (node[2] << 16) | ((uint32_t)node[3] << 24));
    uint8_t *end = node + (uint32_t)(node[4] | (node[5] << 8) |
                                     (node[6] << 16) | ((uint32_t)node[7] << 24));

    while (p < end) {
        uint16_t elen = (uint16_t)(p[8] | (p[9] << 8));
        uint32_t flags = (uint32_t)(p[12] | (p[13] << 8) | (p[14] << 16) |
                                    ((uint32_t)p[15] << 24));
        if (elen < 16 || (flags & 0x02)) break;
        if (ntfs_name_cmp(p + 16 + 0x42, p[16 + 0x40], name) == 0) {
            uint64_t ref = (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
                           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
            int64_t n = ntfs_read_file(ref, out, cap);
            if (n < 0) return -1;
            if (size) *size = (uint64_t)n;
            return 0;
        }
        p += elen;
    }
    return -1;
}

static void test_create_read_delete(void) {
    static uint8_t big[9000];
    static uint8_t back[65536];
    uint64_t got = 0;

    printf("\ncreating, reading back and deleting\n");

    expect(remount() == 0, "remounted");
    ntfs_journal_init(0, 16 * 1024 * 1024 / 512);

    /* A small file, which NTFS keeps inside the record. */
    {
        const char *msg = "resident data lives in the MFT record itself\n";
        expect_eq(ntfs_create_file(NTFS_MFT_ROOT, "small.txt",
                                   (const uint8_t *)msg, (uint32_t)strlen(msg)),
                  NTFS_W_OK, "a small file is created");

        expect(remount() == 0, "remounted after the create");
        expect(read_back("small.txt", back, sizeof(back), &got) == 0,
               "and reads back after a fresh mount");
        expect_eq(got, strlen(msg), "with the right length");
        expect(memcmp(back, msg, strlen(msg)) == 0, "and the right bytes");
    }

    /* A file too large to be resident, so it exercises the cluster
     * allocator and the run list. */
    {
        for (unsigned i = 0; i < sizeof(big); i++)
            big[i] = (uint8_t)(i * 7 + (i >> 8));

        expect_eq(ntfs_create_file(NTFS_MFT_ROOT, "new.bin", big, sizeof(big)),
                  NTFS_W_OK, "a non-resident file is created");

        expect(remount() == 0, "remounted after the create");
        expect(read_back("new.bin", back, sizeof(back), &got) == 0,
               "and reads back after a fresh mount");
        expect_eq(got, sizeof(big), "with the right length");
        expect(memcmp(back, big, sizeof(big)) == 0,
               "and every byte matches across three clusters");
    }

    /* The same name twice must be refused, not silently duplicated --
     * two entries with one name is a directory a reader cannot fix. */
    {
        const char *msg = "second";
        expect_eq(ntfs_create_file(NTFS_MFT_ROOT, "small.txt",
                                   (const uint8_t *)msg, 6),
                  NTFS_W_EXISTS, "refuses  a duplicate name");
    }

    /* A directory. */
    {
        expect_eq(ntfs_mkdir_at(NTFS_MFT_ROOT, "sub"), NTFS_W_OK,
                  "a directory is created");
        expect(remount() == 0, "remounted after mkdir");

        uint8_t dir[4096];
        uint64_t ref = 0;
        expect(ntfs_read_record(NTFS_MFT_ROOT, dir) == 0, "the root reads");

        ntfs_attr_t *ir = (ntfs_attr_t *)ntfs_find_attr(dir,
                                                        NTFS_ATTR_INDEX_ROOT);
        uint8_t *node = (uint8_t *)ir + ir->u.res.value_offset + 16;
        uint8_t *p = node + 0x10;
        int found = 0;
        while (p < dir + ((ntfs_record_t *)dir)->used_size) {
            uint16_t elen = (uint16_t)(p[8] | (p[9] << 8));
            uint32_t fl = (uint32_t)(p[12] | (p[13] << 8) | (p[14] << 16) |
                                     ((uint32_t)p[15] << 24));
            if (elen < 16 || (fl & 0x02)) break;
            if (ntfs_name_cmp(p + 16 + 0x42, p[16 + 0x40], "sub") == 0) {
                found = 1;
                ref = (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                      ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24);
                break;
            }
            p += elen;
        }
        expect(found, "and appears in the root index");

        if (found) {
            uint8_t rec[4096];
            expect(ntfs_read_record(ref & 0xFFFFFFFFFFFFULL, rec) == 0,
                   "its record reads");
            expect(((ntfs_record_t *)rec)->flags & 2,
                   "and is flagged as a directory");
        }
    }

    /* Delete, and confirm the name is gone and the space came back. */
    {
        uint64_t before_free = 0, after_free = 0;
        uint64_t probe = 0;

        expect(remount() == 0, "remounted");
        ntfs_journal_init(0, 16 * 1024 * 1024 / 512);

        /* count free space crudely: how far the first-fit allocator
         * has to go for one cluster */
        ntfs_clusters_alloc(1, &before_free);
        ntfs_clusters_free(before_free, 1);

        expect_eq(ntfs_delete_file(NTFS_MFT_ROOT, "new.bin"), NTFS_W_OK,
                  "the non-resident file is deleted");

        expect(remount() == 0, "remounted after the delete");
        expect(read_back("new.bin", back, sizeof(back), &got) != 0,
               "and the name no longer resolves");
        expect(read_back("small.txt", back, sizeof(back), &got) == 0,
               "while its neighbour is untouched");

        ntfs_clusters_alloc(1, &after_free);
        expect(after_free <= before_free,
               "and the clusters it held were returned");
        ntfs_clusters_free(after_free, 1);

        expect_eq(ntfs_delete_file(NTFS_MFT_ROOT, "nothing.txt"),
                  NTFS_W_NOTFOUND, "refuses  deleting a name that is absent");
        (void)probe;
    }
}

/* ===== 5. the journal ===== */

static void test_journal(void) {
    uint8_t sector[512];
    uint8_t check[512];
    uint64_t target;

    printf("\nthe write-ahead journal\n");

    expect(remount() == 0, "remounted");
    expect_eq(ntfs_journal_init(0, 16 * 1024 * 1024 / 512), 0,
              "the journal finds room at the end of the volume");

    /* Pick a scratch sector well away from anything the filesystem
     * uses: the last data cluster before the journal. */
    target = ntfs_jrnl.base_lba - 64;

    for (int i = 0; i < 512; i++) sector[i] = (uint8_t)(i ^ 0x5A);
    expect_eq(ntfs_journal_write(target, sector), 0,
              "a journalled write completes");
    expect(blk_read(target, 1, check) == 0 &&
           memcmp(check, sector, 512) == 0,
           "and the data landed at the target");

    /*
     * Now simulate a crash between commit and the in-place write: put a
     * committed record in the log whose target still holds old data,
     * and check that replay applies it. This is the property the whole
     * mechanism exists for and the only way to see it is to stage it.
     */
    {
        uint8_t old[512], newer[512];
        for (int i = 0; i < 512; i++) old[i] = 0xAA;
        for (int i = 0; i < 512; i++) newer[i] = 0xC3;

        blk_write(target, 1, old);
        blk_flush();

        /* hand-build a committed journal record pointing at `target` */
        {
            uint8_t slot[1024];
            ntfs_jrnl_hdr_t *h = (ntfs_jrnl_hdr_t *)slot;
            memset(slot, 0, sizeof(slot));
            memcpy(slot + 512, newer, 512);
            h->magic = NTFS_JRNL_MAGIC;
            h->seq = 999;
            h->target_lba = target;
            h->length = NTFS_JRNL_PAYLOAD;
            h->checksum = ntfs_checksum(slot + 512, NTFS_JRNL_PAYLOAD);
            h->committed = 1;
            blk_write(ntfs_jrnl.base_lba, 2, slot);
            blk_flush();
        }

        expect(blk_read(target, 1, check) == 0 && check[0] == 0xAA,
               "the target still holds the old data");
        expect_eq(ntfs_journal_replay(), 0, "replay runs");
        expect(blk_read(target, 1, check) == 0 && check[0] == 0xC3,
               "and the committed record was re-applied");
    }

    /* A record whose payload is corrupt must be discarded, not applied:
     * writing a bad payload over live metadata is worse than losing the
     * write it described. */
    {
        uint8_t slot[1024];
        uint8_t known[512];
        ntfs_jrnl_hdr_t *h = (ntfs_jrnl_hdr_t *)slot;

        for (int i = 0; i < 512; i++) known[i] = 0x11;
        blk_write(target, 1, known);
        blk_flush();

        memset(slot, 0, sizeof(slot));
        memset(slot + 512, 0xEE, 512);
        h->magic = NTFS_JRNL_MAGIC;
        h->seq = 1000;
        h->target_lba = target;
        h->length = NTFS_JRNL_PAYLOAD;
        h->checksum = ntfs_checksum(slot + 512, NTFS_JRNL_PAYLOAD) ^ 0xFFFF;
        h->committed = 1;
        blk_write(ntfs_jrnl.base_lba, 2, slot);
        blk_flush();

        ntfs_journal_replay();
        expect(blk_read(target, 1, check) == 0 && check[0] == 0x11,
               "refuses  a record whose checksum does not match");
    }

    /* An uncommitted record must also be ignored. */
    {
        uint8_t slot[1024];
        uint8_t known[512];
        ntfs_jrnl_hdr_t *h = (ntfs_jrnl_hdr_t *)slot;

        for (int i = 0; i < 512; i++) known[i] = 0x22;
        blk_write(target, 1, known);
        blk_flush();

        memset(slot, 0, sizeof(slot));
        memset(slot + 512, 0xDD, 512);
        h->magic = NTFS_JRNL_MAGIC;
        h->seq = 1001;
        h->target_lba = target;
        h->length = NTFS_JRNL_PAYLOAD;
        h->checksum = ntfs_checksum(slot + 512, NTFS_JRNL_PAYLOAD);
        h->committed = 0;
        blk_write(ntfs_jrnl.base_lba, 2, slot);
        blk_flush();

        ntfs_journal_replay();
        expect(blk_read(target, 1, check) == 0 && check[0] == 0x22,
               "refuses  a record that was never committed");
    }
}

/* ===== 6. fixups ===== */

static void test_fixups(void) {
    uint8_t rec[1024];

    printf("\nupdate sequence (torn write detection)\n");

    memset(rec, 0, sizeof(rec));
    memcpy(rec, "FILE", 4);
    rec[0x04] = 0x30; rec[0x05] = 0x00;         /* fixup offset */
    rec[0x06] = 3;    rec[0x07] = 0;            /* 1024/512 + 1 */
    /* put recognisable data in the two sector tails */
    rec[510] = 0xAB; rec[511] = 0xCD;
    rec[1022] = 0x12; rec[1023] = 0x34;

    expect_eq(ntfs_install_fixups(rec, 1024, 512), 0, "fixups install");
    expect(rec[510] != 0xAB || rec[511] != 0xCD,
           "and the sector tails were replaced");
    expect_eq(ntfs_apply_fixups(rec, 1024, 512), 0, "and they undo");
    expect(rec[510] == 0xAB && rec[511] == 0xCD,
           "restoring the first tail exactly");
    expect(rec[1022] == 0x12 && rec[1023] == 0x34,
           "and the second");

    /* A record where one sector was never written carries the wrong
     * sequence number in its tail, and must be rejected. */
    ntfs_install_fixups(rec, 1024, 512);
    rec[1022] = 0x00; rec[1023] = 0x00;
    expect(ntfs_apply_fixups(rec, 1024, 512) != 0,
           "refuses  a record with a torn sector");
}


/* ===== 7. paths, listings and ranges =====
 *
 * Everything above names files by MFT record, which is how the write
 * engine was checked while NTFS was a validated driver with no callers.
 * The volume boots the system now, so the layer the desktop actually
 * calls -- paths, directory listings, byte ranges -- gets the same
 * treatment.
 *
 * These run against a second image with a directory tree in it, because
 * a path walker checked only against a flat volume never walks.
 */

static const char *TREE_IMG = 0;

static int tree_mount(void) {
    ntfs_host_close();
    if (ntfs_host_open(TREE_IMG) != 0) return -1;
    ntfs.mounted = 0;
    ntfs_bmp.loaded = 0;
    if (!ntfs_try(0)) return -1;
    return 0;
}

static void test_lookup(void) {
    uint64_t rec = 0, parent = 0, size = 0;
    int is_dir = -1;

    printf("\npath resolution\n");

    expect(tree_mount() == 0, "the tree volume mounts");

    expect(ntfs_lookup("/", &rec, 0, &is_dir, 0) == 0, "the root resolves");
    expect_eq(rec, NTFS_MFT_ROOT, "  to record 5");
    expect(is_dir == 1, "  and is a directory");

    expect(ntfs_lookup("/a.txt", &rec, &parent, &is_dir, &size) == 0,
           "a file in the root resolves");
    expect(is_dir == 0, "  and is not a directory");
    expect_eq(parent, NTFS_MFT_ROOT, "  with the root as its parent");
    expect_eq(size, 20, "  and the size the formatter wrote");

    expect(ntfs_lookup("/docs", 0, 0, &is_dir, 0) == 0,
           "a subdirectory resolves");
    expect(is_dir == 1, "  as a directory");

    expect(ntfs_lookup("/docs/readme.txt", &rec, &parent, &is_dir, &size) == 0,
           "a file one level down resolves");
    expect(is_dir == 0, "  as a file");
    expect_eq(size, 26, "  with its own size");
    {
        uint64_t droot = 0;
        ntfs_lookup("/docs", &droot, 0, 0, 0);
        expect_eq(parent, droot, "  and /docs as its parent");
    }

    expect(ntfs_lookup("/store/pkg/big.bin", &rec, 0, &is_dir, &size) == 0,
           "a file two levels down resolves");
    expect_eq(size, 300000, "  with the right size");

    /* The failures, which matter more than the successes: a lookup that
     * quietly returns the root for a bad path would make every caller
     * read the wrong file. */
    expect(ntfs_lookup("/nope.txt", 0, 0, 0, 0) != 0,
           "a missing file is refused");
    expect(ntfs_lookup("/docs/nope.txt", 0, 0, 0, 0) != 0,
           "  as is a missing file in a real directory");
    expect(ntfs_lookup("/nodir/readme.txt", 0, 0, 0, 0) != 0,
           "  as is a path through a missing directory");
    expect(ntfs_lookup("/a.txt/child", 0, 0, 0, 0) != 0,
           "  and a path *through a file* is refused, not followed");

    /* Case folding, to match the exFAT driver this replaces --
     * everything above the filesystem was written against a folding
     * volume, and /Etc and /etc being different would break it. */
    expect(ntfs_lookup("/A.TXT", &rec, 0, 0, 0) == 0,
           "lookup folds case");
    expect(ntfs_lookup("/DOCS/ReadMe.TXT", 0, 0, 0, &size) == 0,
           "  at every component");
    expect_eq(size, 26, "  and finds the same file");

    /* Leading and repeated separators are the shapes a path arrives in
     * when it has been joined from two halves. */
    expect(ntfs_lookup("//docs//readme.txt", 0, 0, 0, 0) == 0,
           "repeated separators are ignored");
    expect(ntfs_lookup("/docs/", 0, 0, &is_dir, 0) == 0,
           "a trailing separator is ignored");
}

/* ---- listing ---- */

typedef struct {
    char names[32][64];
    uint64_t sizes[32];
    int dirs[32];
    int n;
} list_ctx_t;

static void list_cb(void *ctx, const char *name, uint64_t size, int is_dir) {
    list_ctx_t *l = (list_ctx_t *)ctx;
    if (l->n >= 32) return;
    snprintf(l->names[l->n], sizeof(l->names[0]), "%s", name);
    l->sizes[l->n] = size;
    l->dirs[l->n]  = is_dir;
    l->n++;
}

static int list_has(const list_ctx_t *l, const char *name, int is_dir) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->names[i], name) == 0 && l->dirs[i] == is_dir) return 1;
    return 0;
}

static void test_list(void) {
    list_ctx_t l;

    printf("\ndirectory listing\n");

    expect(tree_mount() == 0, "remounted");

    l.n = 0;
    expect(ntfs_list("/", list_cb, &l) == 0, "the root lists");
    expect(list_has(&l, "a.txt", 0), "  and contains a.txt as a file");
    expect(list_has(&l, "docs", 1), "  and docs as a directory");
    expect(list_has(&l, "store", 1), "  and store as a directory");
    expect(list_has(&l, "pagefile.sys", 0), "  and the pagefile");

    /* The system files are on the volume and in record 0-15, but a
     * listing that showed $MFT would be true and useless. */
    {
        int sys = 0;
        for (int i = 0; i < l.n; i++) if (l.names[i][0] == '$') sys++;
        expect_eq(sys, 0, "  and no $-prefixed system files");
    }

    l.n = 0;
    expect(ntfs_list("/docs", list_cb, &l) == 0, "a subdirectory lists");
    expect_eq(l.n, 1, "  with one entry");
    expect(list_has(&l, "readme.txt", 0), "  which is readme.txt");
    expect_eq(l.sizes[0], 26, "  at its real size");

    l.n = 0;
    expect(ntfs_list("/store/pkg", list_cb, &l) == 0, "a nested one lists");
    expect(list_has(&l, "big.bin", 0), "  and holds big.bin");

    expect(ntfs_list("/a.txt", list_cb, &l) != 0,
           "listing a *file* is refused");
    expect(ntfs_list("/nodir", list_cb, &l) != 0,
           "  as is listing something absent");
}

/* ---- ranged reads ---- */

static void test_read_range(void) {
    static uint8_t whole[300000];
    static uint8_t part[70000];
    uint64_t rec = 0;
    int64_t got;

    printf("\nranged reads\n");

    expect(tree_mount() == 0, "remounted");
    expect(ntfs_lookup("/store/pkg/big.bin", &rec, 0, 0, 0) == 0,
           "the large file resolves");

    got = ntfs_read_file(rec, whole, sizeof(whole));
    expect_eq((uint64_t)got, 300000, "it reads whole");

    /* A range from the start must equal the same bytes read whole --
     * the two paths through the run list have to agree. */
    got = ntfs_read_range(rec, 0, part, 1000);
    expect_eq((uint64_t)got, 1000, "a range from zero reads");
    expect(memcmp(part, whole, 1000) == 0, "  and matches the whole read");

    /* Not on a cluster boundary, which is the case a run walk that only
     * counts whole clusters gets wrong. */
    got = ntfs_read_range(rec, 1, part, 1000);
    expect_eq((uint64_t)got, 1000, "a range at offset 1 reads");
    expect(memcmp(part, whole + 1, 1000) == 0, "  and matches");

    got = ntfs_read_range(rec, 4095, part, 2);
    expect_eq((uint64_t)got, 2, "a range straddling a cluster boundary reads");
    expect(memcmp(part, whole + 4095, 2) == 0, "  and matches");

    got = ntfs_read_range(rec, 4096, part, 4096);
    expect_eq((uint64_t)got, 4096, "a whole aligned cluster reads");
    expect(memcmp(part, whole + 4096, 4096) == 0, "  and matches");

    got = ntfs_read_range(rec, 100003, part, 70000);
    expect_eq((uint64_t)got, 70000, "a long unaligned range reads");
    expect(memcmp(part, whole + 100003, 70000) == 0, "  and matches");

    /* The ends. Reading past the end is a short read, not an error and
     * not a buffer overrun. */
    got = ntfs_read_range(rec, 299000, part, 5000);
    expect_eq((uint64_t)got, 1000, "a range past the end is short");
    expect(memcmp(part, whole + 299000, 1000) == 0, "  and matches");
    got = ntfs_read_range(rec, 300000, part, 100);
    expect_eq((uint64_t)got, 0, "a range starting at the end reads nothing");
    got = ntfs_read_range(rec, 999999, part, 100);
    expect_eq((uint64_t)got, 0, "a range well past the end reads nothing");

    /* A small file is resident -- it lives inside its own MFT record --
     * so the range path has a second implementation that also has to be
     * right. */
    expect(ntfs_lookup("/docs/readme.txt", &rec, 0, 0, 0) == 0,
           "the small file resolves");
    got = ntfs_read_file(rec, whole, sizeof(whole));
    expect_eq((uint64_t)got, 26, "it reads whole");
    got = ntfs_read_range(rec, 6, part, 10);
    expect_eq((uint64_t)got, 10, "a range inside a resident file reads");
    expect(memcmp(part, whole + 6, 10) == 0, "  and matches");
    got = ntfs_read_range(rec, 20, part, 100);
    expect_eq((uint64_t)got, 6, "  and is short at its end");
}

/* ---- free space, and the pagefile's single extent ---- */

static void test_space_and_extent(void) {
    uint64_t total = 0, freec = 0, rec = 0, lba = 0, bytes = 0;

    printf("\nfree space and the pagefile\n");

    expect(tree_mount() == 0, "remounted");

    expect(ntfs_space(&total, &freec) == 0, "the volume reports its space");
    /* Derived from the image rather than written down, so resizing the
     * scratch volume does not turn into a failing assertion about a
     * number that was never the point. */
    expect_eq(total, (uint64_t)(ntfs_host_size() / ntfs.bytes_per_cluster),
              "  total clusters match the volume");
    expect(freec > 0 && freec < total, "  and free is between none and all");
    /* 256 MB of pagefile is 65536 clusters, so at least that much must
     * read as used. A bitmap counted backwards would say the opposite. */
    expect(total - freec >= 65536,
           "  with at least the pagefile counted as used");

    expect(ntfs_lookup("/pagefile.sys", &rec, 0, 0, &bytes) == 0,
           "the pagefile resolves");
    expect_eq(bytes, 256ULL * 1024 * 1024, "  at 256 MB");

    expect(ntfs_single_extent(rec, 256ULL * 1024 * 1024, &lba, &bytes) == 0,
           "and is exactly one extent");
    expect(bytes >= 256ULL * 1024 * 1024, "  covering the whole file");
    expect(lba > 0, "  at a real LBA");
    expect_eq(lba % 8, 0, "  cluster-aligned");

    /* The refusals. A swapper handed the first run of a fragmented file
     * would write pages over whatever followed it, so anything short of
     * one whole run has to fail. */
    expect(ntfs_single_extent(rec, 512ULL * 1024 * 1024, 0, 0) != 0,
           "a request larger than the file is refused");
    /* A resident file -- one small enough to live inside its own MFT
     * record -- has no runs at all, and must be refused rather than
     * read as an extent at cluster zero. The formatter always writes
     * non-resident data, so the only way to get one is to have the
     * kernel create it, which is also the case that matters: a pagefile
     * small enough to go resident would otherwise resolve to garbage. */
    {
        uint64_t small = 0;
        ntfs_journal_init(0, 64 * 1024 * 1024 / 512);
        expect_eq(ntfs_create_file(NTFS_MFT_ROOT, "tiny.txt",
                                   (const uint8_t *)"hi", 2), NTFS_W_OK,
                  "a two-byte file is created");
        expect(ntfs_lookup("/tiny.txt", &small, 0, 0, 0) == 0,
               "  and resolves");
        {
            uint8_t rec[4096];
            const ntfs_attr_t *d;
            expect(ntfs_read_record(small, rec) == 0, "  its record reads");
            d = ntfs_find_attr(rec, NTFS_ATTR_DATA);
            expect(d && !d->non_resident, "  and its data is resident");
        }
        expect(ntfs_single_extent(small, 2, 0, 0) != 0,
               "  so it is refused as an extent");
    }
}

/* ---- how many entries a directory actually holds ---- */

static void test_directory_capacity(void) {
    uint64_t root = NTFS_MFT_ROOT;
    int made = 0;

    printf("\ndirectory capacity\n");

    expect(tree_mount() == 0, "remounted");
    ntfs_journal_init(0, 2048ULL * 1024 * 1024 / 512);

    /*
     * This test used to assert the opposite.
     *
     * A directory lived entirely in its resident $INDEX_ROOT, so it
     * held about thirty-five names and then returned ENOSPC, and the
     * check here was that it *did* fill -- that the refusal was clean
     * rather than a corrupt index. With $INDEX_ALLOCATION the ceiling
     * is gone, so the same loop now measures the opposite property:
     * that the root grows into a tree instead of refusing.
     */
    for (int i = 0; i < 500; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "fill%03d.txt", i);
        if (ntfs_create_file(root, nm, (const uint8_t *)"x", 1) != NTFS_W_OK) {
            printf("       stopped at %d: %s\n", i, ntfs_w_errstr);
            break;
        }
        made++;
    }
    expect_eq(made, 500, "five hundred names go into one directory");

    /* And the root really did become a tree rather than growing the
     * record, which is the only way that many could fit. */
    {
        uint8_t *hdr;
        expect(ntfs_read_record(root, ntfs_dirrec) == 0, "the root record reads");
        hdr = ix_root_hdr(ntfs_dirrec);
        expect(hdr && (ix_flags(hdr) & IX_HDR_LARGE),
               "  and its index has children");
    }

    {
        uint64_t rec = 0;
        expect(ntfs_lookup("/a.txt", &rec, 0, 0, 0) == 0,
               "  the original entries still resolve");
        expect(ntfs_lookup("/docs/readme.txt", &rec, 0, 0, 0) == 0,
               "  including through subdirectories");
        expect(ntfs_lookup("/fill499.txt", &rec, 0, 0, 0) == 0,
               "  and so does the five hundredth new one");
    }
}

/* ===== 8. the directory B-tree =====
 *
 * A directory used to be its own MFT record: about thirty-five names,
 * and then ENOSPC. $INDEX_ALLOCATION makes it a tree, and a B-tree is
 * the kind of structure that works perfectly on the cases you thought
 * of and loses a subtree on the ones you did not -- so these checks are
 * mostly about the shapes that only appear at scale.
 *
 * Names are deliberately of *varying* length. Fixed-width names make
 * every entry the same size, which means the median of a split always
 * lands in the same place and a whole class of length-dependent bugs --
 * an entry that grows when it gains a downlink, a successor longer than
 * the separator it replaces -- can never happen.
 */

/* A name whose length varies with i, from 8 to about 60 characters, and
 * whose sort order is deliberately not its creation order. */
static void btree_name(int i, char *out, size_t cap) {
    const int pad = 8 + (i * 7) % 52;
    int n;
    /* The leading digits are reversed so that insertion order and
     * collation order disagree: a tree that only ever appends is not a
     * tree that has been tested. */
    n = snprintf(out, cap, "%04d-", ((i * 2654435761u) >> 8) % 10000);
    for (int k = 0; k < pad && n + 1 < (int)cap; k++) out[n++] = 'a' + (k % 26);
    n += snprintf(out + n, cap - n, "-%d.txt", i);
    (void)n;
}

typedef struct {
    int      n;
    int      out_of_order;
    char     prev[300];
    int      have_prev;
    uint64_t total_size;
} bt_ctx_t;

static void bt_count(void *ctx, const char *name, uint64_t size, int is_dir) {
    bt_ctx_t *c = (bt_ctx_t *)ctx;
    (void)is_dir;
    /* Strictly ascending under the driver's own comparator: an index
     * that enumerates out of order is one a binary search cannot use,
     * and it is exactly what a mis-split produces. */
    if (c->have_prev) {
        uint8_t u[600];
        int len = 0;
        while (c->prev[len]) { u[len * 2] = (uint8_t)c->prev[len];
                               u[len * 2 + 1] = 0; len++; }
        if (ntfs_name_cmp(u, (uint32_t)len, name) >= 0) c->out_of_order++;
    }
    snprintf(c->prev, sizeof(c->prev), "%s", name);
    c->have_prev = 1;
    c->total_size += size;
    c->n++;
}

#define BT_COUNT 2000

static void test_btree(void) {
    static char nm[300];
    bt_ctx_t c;
    int made = 0;
    uint64_t rec = 0;
    int rc;

    printf("\nthe directory B-tree\n");

    expect(tree_mount() == 0, "remounted");
    ntfs_journal_init(0, 2048ULL * 1024 * 1024 / 512);

    expect_eq(ntfs_mkdir_at(NTFS_MFT_ROOT, "big"), NTFS_W_OK,
              "a directory is created");
    expect(ntfs_lookup("/big", &rec, 0, 0, 0) == 0, "  and resolves");

    /* --- fill it well past what one record can hold --- */
    for (int i = 0; i < BT_COUNT; i++) {
        btree_name(i, nm, sizeof(nm));
        rc = ntfs_create_file(rec, nm, (const uint8_t *)"x", 1);
        if (rc != NTFS_W_OK) {
            printf("       stopped at %d: %s\n", i, ntfs_w_errstr);
            break;
        }
        made++;
    }
    expect_eq(made, BT_COUNT, "two thousand names go in");
    printf("       (%u node splits along the way)\n", ntfs_ix_splits);
    expect(ntfs_ix_splits > 0, "  and the tree had to split to hold them");

    /* --- the tree is actually a tree --- */
    {
        uint8_t *hdr;
        expect(ntfs_read_record(rec, ntfs_dirrec) == 0, "the record reads");
        hdr = ix_root_hdr(ntfs_dirrec);
        expect(hdr && (ix_flags(hdr) & IX_HDR_LARGE),
               "  and its root has children");
        expect(ntfs_find_attr(ntfs_dirrec, NTFS_ATTR_INDEX_ALLOC) != 0,
               "  with an $INDEX_ALLOCATION");
        expect(ntfs_find_attr(ntfs_dirrec, NTFS_ATTR_BITMAP) != 0,
               "  and a $BITMAP");
    }

    /* --- everything is there, once, in order --- */
    expect(tree_mount() == 0, "remounted after filling");
    c.n = 0; c.out_of_order = 0; c.have_prev = 0; c.total_size = 0;
    expect(ntfs_list("/big", bt_count, &c) == 0, "the directory lists");
    expect_eq(c.n, BT_COUNT, "  and every name comes back");
    expect_eq(c.out_of_order, 0, "  in strictly ascending order");
    expect_eq(c.total_size, BT_COUNT, "  each one byte long");

    /* --- and each one resolves by path --- */
    {
        int bad = 0;
        for (int i = 0; i < BT_COUNT; i += 7) {
            char path[400];
            uint64_t r = 0;
            btree_name(i, nm, sizeof(nm));
            snprintf(path, sizeof(path), "/big/%s", nm);
            if (ntfs_lookup(path, &r, 0, 0, 0) != 0) bad++;
        }
        expect_eq(bad, 0, "every name sampled resolves through the tree");
    }
    {
        char path[400];
        snprintf(path, sizeof(path), "/big/%s", "definitely-not-here.txt");
        expect(ntfs_lookup(path, 0, 0, 0, 0) != 0,
               "  and a name that was never added does not");
    }

    /* --- a duplicate is still refused, several levels down --- */
    btree_name(BT_COUNT / 2, nm, sizeof(nm));
    expect_eq(ntfs_create_file(rec, nm, (const uint8_t *)"x", 1),
              NTFS_W_EXISTS, "a duplicate deep in the tree is refused");
}

/*
 * Deleting separators.
 *
 * A name sitting in an interior node divides the tree below it, so
 * removing it has to promote its in-order successor into the gap -- and
 * that successor arrives from a leaf, gains a downlink, and may be a
 * longer name than the one it replaces. It is the only operation in the
 * driver where removing something needs *more* room than it frees.
 *
 * Reaching it by accident is unlikely, so this finds the separators
 * deliberately: walk the tree, collect the names whose entries carry a
 * child pointer, and delete exactly those.
 */
typedef struct { char names[64][300]; int n; } sep_ctx_t;

static int sep_collect(void *ctx, const uint8_t *entry, const char *name) {
    sep_ctx_t *s = (sep_ctx_t *)ctx;
    if ((ie_flags(entry) & IE_HAS_CHILD) && s->n < 64)
        snprintf(s->names[s->n++], sizeof(s->names[0]), "%s", name);
    return 0;
}

static void test_btree_delete(void) {
    static char nm[300];
    sep_ctx_t sep;
    bt_ctx_t c;
    uint64_t rec = 0;
    int removed = 0;
    const uint32_t grew_before = ntfs_ix_promote_grew;

    printf("\nremoving names, including the ones holding the tree apart\n");

    expect(tree_mount() == 0, "remounted");
    ntfs_journal_init(0, 2048ULL * 1024 * 1024 / 512);
    expect(ntfs_lookup("/big", &rec, 0, 0, 0) == 0, "the directory resolves");

    /* Every separator currently in the tree. */
    sep.n = 0;
    expect(ntfs_read_record(rec, ntfs_dirrec) == 0, "its record reads");
    {
        uint8_t *h = ix_root_hdr(ntfs_dirrec);
        expect(h != 0, "  and has a resident index root");
        if (h) ix_walk_node(ntfs_dirrec, h, 0, sep_collect, &sep);
    }
    printf("       (%d separators found)\n", sep.n);
    expect(sep.n > 0, "the tree has interior entries to remove");

    for (int i = 0; i < sep.n; i++)
        if (ntfs_delete_file(rec, sep.names[i]) == NTFS_W_OK) removed++;
    expect_eq(removed, sep.n, "every separator is removed");
    printf("       (%u of them promoted a longer name than they held)\n",
           ntfs_ix_promote_grew - grew_before);
    expect(ntfs_ix_promote_grew > grew_before,
           "  and at least one successor was longer than the separator");

    /* The tree still holds everything else, still in order. */
    expect(tree_mount() == 0, "remounted after the deletions");
    c.n = 0; c.out_of_order = 0; c.have_prev = 0; c.total_size = 0;
    expect(ntfs_list("/big", bt_count, &c) == 0, "it still lists");
    expect_eq(c.n, BT_COUNT - removed, "  with exactly the rest present");
    expect_eq(c.out_of_order, 0, "  still in ascending order");

    /* The removed names are gone, and the survivors are not. */
    {
        int ghost = 0, lost = 0;
        char path[400];
        for (int i = 0; i < sep.n; i++) {
            snprintf(path, sizeof(path), "/big/%s", sep.names[i]);
            if (ntfs_lookup(path, 0, 0, 0, 0) == 0) ghost++;
        }
        expect_eq(ghost, 0, "no removed name still resolves");

        for (int i = 0; i < BT_COUNT; i += 13) {
            int was_removed = 0;
            btree_name(i, nm, sizeof(nm));
            for (int k = 0; k < sep.n; k++)
                if (strcmp(sep.names[k], nm) == 0) { was_removed = 1; break; }
            if (was_removed) continue;
            snprintf(path, sizeof(path), "/big/%s", nm);
            if (ntfs_lookup(path, 0, 0, 0, 0) != 0) lost++;
        }
        expect_eq(lost, 0, "  and every survivor sampled still does");
    }

    /* Deleting the rest empties it completely rather than leaving
     * fragments behind -- the case where an under-full tree has to keep
     * working all the way down to nothing. */
    {
        int gone = 0;
        for (int i = 0; i < BT_COUNT; i++) {
            btree_name(i, nm, sizeof(nm));
            if (ntfs_delete_file(rec, nm) == NTFS_W_OK) gone++;
        }
        expect_eq(gone + removed, BT_COUNT, "the remaining names all delete");

        expect(tree_mount() == 0, "remounted after emptying it");
        c.n = 0; c.out_of_order = 0; c.have_prev = 0; c.total_size = 0;
        expect(ntfs_list("/big", bt_count, &c) == 0, "the directory lists");
        expect_eq(c.n, 0, "  and is empty");
    }

    /* And it can be filled again, which catches a tree left in a state
     * that reads as empty but cannot be written. */
    {
        int again = 0;
        for (int i = 0; i < 200; i++) {
            btree_name(i, nm, sizeof(nm));
            if (ntfs_create_file(rec, nm, (const uint8_t *)"y", 1) == NTFS_W_OK)
                again++;
        }
        expect_eq(again, 200, "two hundred names go back in afterwards");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ntfs_test <image>\n");
        return 2;
    }
    IMG = argv[1];
    TREE_IMG = (argc > 2) ? argv[2] : 0;

    printf("Vextro NTFS: allocation, records, indexes, journal\n");
    printf("==================================================\n");

    if (ntfs_host_open(IMG) != 0) {
        fprintf(stderr, "cannot open %s\n", IMG);
        return 2;
    }

    test_runlist();
    test_fixups();
    test_mount_and_read();
    test_allocation();
    test_create_read_delete();
    test_journal();

    if (TREE_IMG) {
        test_lookup();
        test_list();
        test_read_range();
        test_space_and_extent();
        test_directory_capacity();
        test_btree();
        test_btree_delete();
    } else {
        printf("\n(no tree image given: skipping paths, listings and "
               "ranges)\n");
    }

    ntfs_host_close();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
