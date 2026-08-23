#ifndef VEXTRO_FS_NTFS_H
#define VEXTRO_FS_NTFS_H

/*
 * src/fs/ntfs/ntfs.h — what the rest of the kernel may call in the NTFS
 * module.
 *
 * ---- a note on what is and is not wired up ----
 *
 * Everything below is compiled into the kernel and callable. Only
 * ntfs_mount() is currently *called* from it: this system still boots
 * from exFAT, so no file operation routes through here yet, and the
 * writer's only caller today is tools/ntfs_test.c against scratch
 * images. That is a deliberate state rather than an unfinished one — a
 * half-exercised filesystem writer pointed at the volume holding the
 * user's account is the single bug with no recovery, so the switch was
 * left as its own decision.
 *
 * Before the split these declarations did not exist and the writer was
 * not in the kernel image at all: src/ntfswrite.h was included by
 * nothing except the host test, so roughly 1,200 lines of it compiled
 * only on the build machine. Naming the interface here is what puts it
 * in the kernel, which means it is now built with the kernel's warning
 * set and its own flags on every build rather than only when the test
 * suite runs.
 *
 * ---- error codes ----
 *
 * The write calls return NTFS_W_OK or a negative code; those constants
 * live with their explanations in ntfs_ops.c, and callers should test
 * against zero rather than against a particular failure.
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

/* Create a subdirectory `name` under `dir_record`. The new directory
 * holds its entries in a resident $INDEX_ROOT, which is roughly forty
 * of them before it reports ENOSPC. */
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

#endif /* VEXTRO_FS_NTFS_H */
