#include <stdint.h>
#include <stddef.h>
#include "../include/tarfs.h"

static uint8_t *tarfs_base = 0;
static uint64_t tarfs_size = 0;

static int str_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static uint64_t octal_parse(const char *s, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        val = val * 8 + (uint64_t)(s[i] - '0');
    return val;
}

void tarfs_init(void *base, uint64_t size) {
    tarfs_base = (uint8_t *)base;
    tarfs_size = size;
}

const void *fs_read_file(const char *filename, uint64_t *out_size) {
    if (!tarfs_base || !filename)
        return 0;

    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;

    while (ptr + TAR_BLOCK_SIZE <= end) {
        tar_header_t *hdr = (tar_header_t *)ptr;

        /* End of archive: two zero blocks */
        if (hdr->name[0] == '\0')
            break;

        uint64_t file_size = octal_parse(hdr->size, 12);

        /* Match filename (strip leading ./ if present) */
        const char *entry_name = hdr->name;
        if (entry_name[0] == '.' && entry_name[1] == '/')
            entry_name += 2;

        /* Also try matching without leading slash */
        const char *query = filename;
        if (query[0] == '/')
            query++;

        if (str_equal(entry_name, query) || str_equal(hdr->name, filename)) {
            if (out_size)
                *out_size = file_size;
            return (const void *)(ptr + TAR_BLOCK_SIZE);
        }

        /* Advance to next header: header block + data blocks (rounded up) */
        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }

    if (out_size)
        *out_size = 0;
    return 0;
}

void tarfs_list(tarfs_list_cb cb) {
    if (!tarfs_base || !cb)
        return;

    uint8_t *ptr = tarfs_base;
    uint8_t *end = tarfs_base + tarfs_size;

    while (ptr + TAR_BLOCK_SIZE <= end) {
        tar_header_t *hdr = (tar_header_t *)ptr;

        if (hdr->name[0] == '\0')
            break;

        uint64_t file_size = octal_parse(hdr->size, 12);

        /* Only report regular files (typeflag '0' or '\0') */
        if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            const char *name = hdr->name;
            if (name[0] == '.' && name[1] == '/')
                name += 2;
            if (name[0] != '\0')
                cb(name, file_size);
        }

        uint64_t blocks = (file_size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        ptr += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
    }
}
