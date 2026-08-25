#ifndef VEXTRO_FS_NTFS_H
#define VEXTRO_FS_NTFS_H

/*
 * src/fs/ntfs/ntfs.h — what the rest of the kernel may call in the NTFS
 * module.
 *
 * ---- this is the boot volume ----
 *
 * It was not always. For a long time the writer here was compiled,
 * host-tested against scratch images, and called by nothing: the system
 * booted from exFAT, and pointing a half-exercised filesystem writer at
 * the volume holding someone's account is the one bug with no recovery.
 *
 * That changed with the surface at the bottom of this file. Everything
 * above names files by MFT record, which is NTFS's own way of doing it
 * and enough for a driver with no callers. A volume the desktop mounts
 * has to answer what is at /etc/policy.cfg, what is in /store/pkg, and
 * what the sixty-four kilobytes four hundred megabytes into wiki.zim
 * are — so paths, listings and ranged reads are here too, and
 * src/desktop.h dispatches to them.
 *
 * exFAT and FAT32 are still in the tree and still mount. The boot
 * volume moved; the ability to read a volume made by an older build, or
 * a stick from another machine, did not.
 */

#include <stdint.h>

/*
 * Probe every partition, and the whole disk, for an NTFS volume and
 * mount the first one found. Reports what it finds on the console.
 * Safe to call with no disk attached — it asks blk_present() first and
 * returns quietly.
 */
void ntfs_mount(void);

/* ---- reading ---- */

/*
 * Read a file's data by MFT record number into `buf`, up to `cap`
 * bytes. Returns the number of bytes read, or -1 if the volume is not
 * mounted or the record does not parse. Handles both resident data
 * (small files, living inside their own record) and non-resident runs.
 */
int64_t ntfs_read_file(uint64_t record, void *buf, uint64_t cap);

/*
 * The size of a record's $DATA attribute, with *resident set to whether
 * the data lives inside the record rather than in clusters. Takes a
 * record that has already been read and had its fixups applied.
 */
uint64_t ntfs_record_size(const uint8_t *rec, int *resident);

/* ---- writing ---- */

/*
 * Create `name` in the directory at `dir_record` with `len` bytes of
 * `data`. Allocates clusters, allocates an MFT record, writes
 * $STANDARD_INFORMATION, $FILE_NAME and $DATA, inserts the directory
 * entry, and logs the change to the USN journal — all through the
 * write-ahead journal, so a power loss part-way leaves the volume
 * consistent rather than half-updated.
 */
int ntfs_create_file(uint64_t dir_record, const char *name,
                     const uint8_t *data, uint32_t len);

/* Remove `name` from the directory at `dir_record`, freeing its
 * clusters and its MFT record. */
int ntfs_delete_file(uint64_t dir_record, const char *name);

/* Create a subdirectory `name` under `dir_record`. It starts with a
 * resident $INDEX_ROOT and grows an $INDEX_ALLOCATION B-tree when that
 * fills, so there is no practical limit on how many names it holds. */
int ntfs_mkdir_at(uint64_t dir_record, const char *name);

/* ---- the journal ---- */

/*
 * Place the write-ahead journal on the volume at `part_lba`. Called by
 * the mount path; separate because the host test drives it directly.
 */
int ntfs_journal_init(uint64_t part_lba, uint64_t part_sectors);

/*
 * Replay any committed-but-unapplied journal records, which is what
 * makes a torn write recoverable. Returns the number applied.
 */
int ntfs_journal_replay(void);

/* ===== THE FILESYSTEM SURFACE =====
 *
 * Everything above names files by MFT record, which is NTFS's own way of
 * doing it. Everything below speaks paths, because the volume this
 * driver mounts is now the one the system boots from, and the desktop
 * asks for /etc/policy.cfg rather than for record 43.
 */

/*
 * Resolve an absolute path. Any of the four outputs may be null.
 *
 * `out_parent` is the record of the directory holding the last
 * component — every write needs it, and returning it here saves the
 * caller a second walk down the same path.
 *
 * Case-insensitive, matching the exFAT driver this replaces: everything
 * above the filesystem was written against a folding volume.
 */
int ntfs_lookup(const char *path, uint64_t *out_record, uint64_t *out_parent,
                int *out_is_dir, uint64_t *out_size);

/* Called once per entry by ntfs_list, in the order the index holds them
 * (which NTFS keeps sorted). Names are ASCII; a code point that does not
 * fit a byte arrives as '?'. */
typedef void (*ntfs_list_cb_t)(void *ctx, const char *name,
                               uint64_t size, int is_dir);

/* Enumerate a directory by path. Returns -1 if it is not a directory
 * here. System files (`$MFT` and the rest of records 0–15) are omitted:
 * listing them would be true and useless. */
int ntfs_list(const char *path, ntfs_list_cb_t cb, void *ctx);

/*
 * A byte range from a file, by record.
 *
 * This is what makes a 937 MB encyclopedia readable on a machine that
 * cannot hold one — the archive reader asks for a window at a time.
 * Returns bytes read (short at end of file) or -1. Handles resident
 * files, sparse runs, and ranges that start part-way into a cluster.
 */
int64_t ntfs_read_range(uint64_t record, uint64_t offset,
                        void *buf, uint64_t len);

/* Total and free clusters, counted out of $Bitmap rather than tracked —
 * a count maintained separately from the thing it counts is one that
 * drifts. One pass, for `df` and the settings panel. */
int ntfs_space(uint64_t *out_total, uint64_t *out_free);

uint32_t ntfs_cluster_bytes(void);

/* Whether a volume is mounted, what the mount decided, and how big it
 * is. The volume state itself stays inside the module. */
int         ntfs_mounted(void);
const char *ntfs_status(void);
uint64_t    ntfs_total_clusters(void);

/* Why the last write call failed. Set by every NTFS_W_* return that is
 * not NTFS_W_OK, so the filesystem layer can report a reason rather
 * than a number. */
extern const char *ntfs_w_errstr;

/* The write-call return codes, so callers can compare against OK
 * rather than against zero and hope. */
#define NTFS_W_OK            0
#define NTFS_W_NOSPACE      -1
#define NTFS_W_IO           -2
#define NTFS_W_EXISTS       -3
#define NTFS_W_NOTFOUND     -4
#define NTFS_W_TOOBIG       -5
#define NTFS_W_READONLY     -6

/*
 * Where a file's data physically starts, if and only if it is exactly
 * one non-sparse run of at least `need_bytes`.
 *
 * For the pagefile and nothing else: src/swap.h resolves its backing
 * store to one absolute LBA at boot so that no filesystem code runs
 * inside a page fault. Fragmented, sparse, resident or short is refused
 * rather than partially accepted — a swapper that quietly used the first
 * run of a fragmented file would write pages over whatever followed it.
 */
int ntfs_single_extent(uint64_t record, uint64_t need_bytes,
                       uint64_t *out_lba, uint64_t *out_bytes);

#endif /* VEXTRO_FS_NTFS_H */
