/*
 * harness.c — fuzz target for vx_validate().
 *
 * One file path from argv, one call to vx_validate(), return. Nothing
 * else: no printing, no writing, no allocation beyond the single buffer,
 * and above all none of the work vx_run.c does *after* validation, which
 * ends in mmap, mprotect and calling the image's entry point. A fuzzer
 * that reached that would be executing attacker bytes, not testing the
 * check that is supposed to stop them.
 *
 * The exit status is 0 when the header validates and 1 when it does not.
 * AFL ignores both -- it only cares about crashes -- but it lets the
 * seed corpus be checked before it is used, which matters: a corpus of
 * files that all fail at byte 0 explores nothing.
 *
 * Why the header is copied into a zeroed local rather than cast in
 * place, in two parts:
 *
 *   Alignment. vx_header_t asserts 8-byte alignment and the buffer from
 *   malloc happens to satisfy it, but a cast would still be encoding an
 *   assumption the format does not make. The kernel copies byte-wise for
 *   the same reason and says so.
 *
 *   Short files. vx_validate's first test is file_size < sizeof(header),
 *   so it is *designed* to be handed a size smaller than the struct. The
 *   only way to fuzz that branch without the harness itself over-reading
 *   is to copy what exists into a zero-filled header and pass the true
 *   size. This mirrors the kernel's loader, which checks the size before
 *   it copies. It does not mirror vx_run.c, which memcpy()s a full 80
 *   bytes out of a malloc(file_size) buffer before validating anything --
 *   see the note in the README next to this file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vx_format.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;

    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
    long tell = ftell(f);
    if (tell < 0) { fclose(f); return 2; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 2; }

    const uint64_t file_size = (uint64_t)tell;

    /* A zero-length input is still a legitimate thing to hand the
     * validator; malloc(0) is not a legitimate thing to read from. */
    unsigned char *buf = NULL;
    if (file_size) {
        buf = (unsigned char *)malloc((size_t)file_size);
        if (!buf) { fclose(f); return 2; }
        if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
            free(buf);
            fclose(f);
            return 2;
        }
    }
    fclose(f);

    vx_header_t h;
    memset(&h, 0, sizeof(h));
    const size_t take = file_size < sizeof(h) ? (size_t)file_size : sizeof(h);
    if (take) memcpy(&h, buf, take);

    const char *bad = vx_validate(&h, file_size);

    free(buf);
    return bad ? 1 : 0;
}
