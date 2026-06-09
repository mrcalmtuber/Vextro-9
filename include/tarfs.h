#ifndef TARFS_H
#define TARFS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Ramdisk Tar File System — parses a standard POSIX (ustar) tar archive
 * loaded into RAM by the bootloader as a Limine module.
 */

#define TAR_BLOCK_SIZE 512

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];       /* octal ASCII */
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];       /* "ustar" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

/* Initialize the tar filesystem with the ramdisk base address and size */
void tarfs_init(void *base, uint64_t size);

/* Read a file from the ramdisk by name.
 * Returns pointer to file data in memory, sets *out_size to file length.
 * Returns NULL if not found. */
const void *fs_read_file(const char *filename, uint64_t *out_size);

/* List files: calls callback for each file in the archive.
 * Callback receives filename and size. */
typedef void (*tarfs_list_cb)(const char *name, uint64_t size);
void tarfs_list(tarfs_list_cb cb);

#endif /* TARFS_H */
