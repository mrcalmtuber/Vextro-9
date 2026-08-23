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
    expect_eq(ntfs.bytes_per_record, 1024, "MFT record size is 1024");

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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ntfs_test <image>\n");
        return 2;
    }
    IMG = argv[1];

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

    ntfs_host_close();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
