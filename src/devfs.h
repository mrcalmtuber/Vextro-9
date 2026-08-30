#ifndef DEVFS_H
#define DEVFS_H

/*
 * src/devfs.h — the eight paths under /dev that a ported program opens
 * without ever asking whether the system has them.
 *
 * Included from the middle of src/vfs.h rather than beside it, and the
 * placement is the interface: it needs vfs_desc_t, which is declared
 * immediately above, and it must be complete before vfs_open runs,
 * because the whole of its job is to answer *before* the filesystem is
 * consulted. A device node is not a file that happens to behave oddly;
 * it is a name the filesystem has never heard of, and the lookup has to
 * be short-circuited rather than have its failure patched afterwards.
 *
 * ---- why these eight ----
 *
 * Not because Linux has them, but because leaving each one out breaks
 * something specific and the break is silent:
 *
 *   /dev/null      every port redirects to it. A program that cannot
 *                  open it usually treats that as fatal at startup.
 *   /dev/zero      the traditional way to get anonymous memory, and
 *                  still what a few allocators do first.
 *   /dev/full      exists so that a write can be made to fail on
 *                  purpose. It is two lines and it is the only way a
 *                  test suite can check its own error path.
 *   /dev/random    and /dev/urandom, which are where a TLS library
 *   /dev/urandom   looks for a key when getrandom is not there. Both
 *                  answer from the same source and neither blocks —
 *                  see the note at dev_read.
 *   /dev/tty       what a program writes a diagnostic to when it has
 *                  decided stderr is not a terminal.
 *   /dev/dri/card0 and renderD128, which is what a graphics stack opens
 *                  to find out whether there is a display at all.
 *
 * ---- and what the render node actually is here ----
 *
 * The specification this was built to asks for the graphics endpoints to
 * be mapped "directly down to our user-space framebuffers", and that is
 * exactly what they are: a write to /dev/dri/renderD128 lands in the
 * calling process's own window surface at the descriptor's offset, and a
 * read takes it back. Not a simulation of one — the same pixels the
 * compositor is about to put on the glass, the same ones the process has
 * mapped at canvas_va, reached through a descriptor instead of through a
 * pointer.
 *
 * What that is *not* is a DRM device. Real DRI is almost entirely
 * ioctl: DRM_IOCTL_VERSION, GEM buffer allocation, mode setting. This
 * kernel has no ioctl at all — the VLS table answers ENOTTY, which is
 * the truthful answer for something that is not a terminal and is also
 * the answer a program takes as "this is not the device I hoped for".
 * So a program that opens the render node here gets a framebuffer, and a
 * program that expects to negotiate with a GPU driver gets a refusal it
 * can read. Both are better than a node that opens and then does
 * nothing.
 */

/*
 * The identifiers. Ordered so that the two graphics nodes are adjacent
 * and can be tested with one comparison, which is done in three places.
 */
enum {
    DEV_NULL = 1,
    DEV_ZERO,
    DEV_FULL,
    DEV_RANDOM,
    DEV_URANDOM,
    DEV_TTY,
    DEV_DRI_CARD,
    DEV_DRI_RENDER
};

#define DEV_IS_GFX(id)  ((id) == DEV_DRI_CARD || (id) == DEV_DRI_RENDER)

typedef struct {
    const char *path;
    uint8_t     id;
    uint8_t     readable;
    uint8_t     writable;
    /* Linux's major and minor, reported by stat. A program that
     * recognises a device by its numbers rather than by its name — and
     * the graphics stack is exactly that program — gets the numbers it
     * is looking for. */
    uint16_t    major;
    uint16_t    minor;
} dev_node_t;

static const dev_node_t dev_table[] = {
    { "/dev/null",             DEV_NULL,       1, 1,   1,   3 },
    { "/dev/zero",             DEV_ZERO,       1, 1,   1,   5 },
    { "/dev/full",             DEV_FULL,       1, 1,   1,   7 },
    { "/dev/random",           DEV_RANDOM,     1, 1,   1,   8 },
    { "/dev/urandom",          DEV_URANDOM,    1, 1,   1,   9 },
    { "/dev/tty",              DEV_TTY,        1, 1,   5,   0 },
    { "/dev/dri/card0",        DEV_DRI_CARD,   1, 1, 226,   0 },
    { "/dev/dri/renderD128",   DEV_DRI_RENDER, 1, 1, 226, 128 },
};

#define DEV_COUNT ((int)(sizeof(dev_table) / sizeof(dev_table[0])))

/* The two directories these live in. Both are listable, so that a
 * program enumerating /dev finds the nodes rather than an empty
 * directory that makes it conclude the system has no devices. */
static const char *dev_dirs[] = { "/dev", "/dev/dri" };
#define DEV_DIR_COUNT 2

/*
 * Case-insensitively, because this is a volume whose own paths are
 * Windows-spelled and whose callers are not. `/DEV/NULL` from a shell
 * and `/dev/null` from a ported library are the same node, for the same
 * reason `C:\Users` and `/Users` are the same directory in fs_native.
 */
static int dev_pathcmp(const char *a, const char *b) {
    for (int i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static const dev_node_t *dev_lookup(const char *abs) {
    for (int i = 0; i < DEV_COUNT; i++)
        if (dev_pathcmp(abs, dev_table[i].path)) return &dev_table[i];
    return 0;
}

static int dev_is_dir(const char *abs) {
    for (int i = 0; i < DEV_DIR_COUNT; i++)
        if (dev_pathcmp(abs, dev_dirs[i])) return 1;
    return 0;
}

/* Anything at all under /dev, which is the question stat has to answer
 * before it goes near the volume. */
static int dev_claims(const char *abs) {
    return dev_lookup(abs) != 0 || dev_is_dir(abs);
}

/*
 * Where a graphics write actually lands.
 *
 * The process's own surface if it was given one, and the shared fallback
 * canvas if the pool was empty — which is exactly the pair of
 * possibilities app_spawn already maps at canvas_va, so a program
 * writing through the descriptor and a program writing through the
 * pointer are writing to the same pixels either way. Deliberately never
 * another process's surface: the descriptor is resolved against
 * vmm_current, not against whichever window is in front.
 */
static uint8_t *dev_surface_bytes(uint32_t *len_out) {
    *len_out = APP_SURF_BYTES;
    if (vmm_current && vmm_current->surface && vmm_current->surface->px)
        return (uint8_t *)vmm_current->surface->px;
    *len_out = (uint32_t)sizeof(app_canvas);
    return (uint8_t *)app_canvas;
}

/* The compositor redraws a surface when its generation changes. Without
 * this a write through the descriptor would reach the pixels and never
 * reach the screen, which is the kind of thing that gets diagnosed as
 * "the write failed". */
static void dev_surface_touched(void) {
    if (vmm_current && vmm_current->surface) vmm_current->surface->gen++;
}

/* How long the node is, which is a real length for the two graphics
 * nodes and zero for the rest — the same answer Linux gives, and the
 * reason a program cannot size /dev/zero by seeking to its end. */
static uint64_t dev_length(uint8_t id) {
    if (!DEV_IS_GFX(id)) return 0;
    uint32_t n = 0;
    (void)dev_surface_bytes(&n);
    return n;
}

static int dev_open(vfs_desc_t *d, const char *abs, uint32_t oflags) {
    const dev_node_t *n = dev_lookup(abs);
    if (!n) return -VXE_NOENT;

    const uint32_t acc = oflags & VX_O_ACCMODE;
    const int wants_write = (acc == VX_O_WRONLY || acc == VX_O_RDWR);
    const int wants_read  = (acc == VX_O_RDONLY || acc == VX_O_RDWR);

    if (wants_write && !n->writable) return -VXE_ACCES;
    if (wants_read && !n->readable)  return -VXE_ACCES;
    if (oflags & VX_O_DIRECTORY)     return -VXE_NOTDIR;

    vfs_desc_reset(d);
    d->kind     = FD_DEV;
    d->devid    = n->id;
    d->readable = (uint8_t)(wants_read ? 1 : 0);
    d->writable = (uint8_t)(wants_write ? 1 : 0);
    d->cloexec  = (oflags & VX_O_CLOEXEC) ? 1 : 0;
    d->oflags   = oflags;
    d->pos      = 0;
    d->sock     = -1;
    str_copy(d->path, n->path, sizeof(d->path));

    /*
     * O_TRUNC on a graphics node clears the window, and that is a real
     * operation rather than a courtesy: `> /dev/dri/renderD128` is how
     * anything on a Unix system says "start from blank", and a truncate
     * that silently did nothing would leave the previous program's
     * picture underneath the new one's.
     */
    if ((oflags & VX_O_TRUNC) && DEV_IS_GFX(n->id) && wants_write) {
        uint32_t len = 0;
        uint8_t *px = dev_surface_bytes(&len);
        for (uint32_t i = 0; i < len; i++) px[i] = 0;
        dev_surface_touched();
    }
    return 0;
}

/*
 * Listing /dev, so that an enumeration finds the nodes.
 *
 * Built into the same fixed-length records vfs_open_dir builds, because
 * a directory descriptor is read by one path whatever produced it — and
 * a second record format reachable only from here is a second thing for
 * getdents to get wrong.
 */
static int dev_open_dir(vfs_desc_t *d, const char *abs, uint32_t oflags) {
    if ((oflags & VX_O_ACCMODE) != VX_O_RDONLY) return -VXE_ISDIR;

    const int root = dev_pathcmp(abs, "/dev");

    /* Count first: the root also carries the `dri` subdirectory. */
    uint32_t n = root ? 1u : 0u;
    for (int i = 0; i < DEV_COUNT; i++) {
        const char *p = dev_table[i].path;
        const int under_dri = str_starts_with(p, "/dev/dri/");
        if (root ? !under_dri : under_dri) n++;
    }

    vx_dirent_t *e = (vx_dirent_t *)kmalloc_pool(
        (uint64_t)n * sizeof(vx_dirent_t), KPOOL_PAGED);
    if (!e) return -VXE_NOMEM;

    uint32_t k = 0;
    if (root) {
        for (uint64_t b = 0; b < sizeof(vx_dirent_t); b++)
            ((uint8_t *)&e[k])[b] = 0;
        e[k].type = VX_DT_DIR;
        str_copy(e[k].name, "dri", sizeof(e[k].name));
        e[k].namelen = 3;
        k++;
    }
    for (int i = 0; i < DEV_COUNT; i++) {
        const char *p = dev_table[i].path;
        const int under_dri = str_starts_with(p, "/dev/dri/");
        if (root ? under_dri : !under_dri) continue;

        for (uint64_t b = 0; b < sizeof(vx_dirent_t); b++)
            ((uint8_t *)&e[k])[b] = 0;
        /* The name is what follows the last separator. */
        int last = 0;
        for (int j = 0; p[j]; j++) if (p[j] == '/') last = j + 1;
        str_copy(e[k].name, p + last, sizeof(e[k].name));
        e[k].namelen = (uint32_t)str_len(p + last);
        e[k].type    = VX_DT_UNKNOWN;   /* a character device is neither */
        e[k].size    = dev_length(dev_table[i].id);
        k++;
    }

    vfs_desc_reset(d);
    d->kind     = FD_DIR;
    d->ents     = e;
    d->nents    = k;
    d->readable = 1;
    d->oflags   = oflags;
    d->cloexec  = (oflags & VX_O_CLOEXEC) ? 1 : 0;
    d->pos      = 0;
    d->sock     = -1;
    str_copy(d->path, abs, sizeof(d->path));
    return 0;
}

/*
 * Reading.
 *
 * Takes a kernel buffer and returns a byte count or a negated error,
 * exactly as vfs_file_read does, so the service routine in src/desktop.h
 * treats the two the same and the user-pointer checking stays in one
 * place.
 *
 * Neither random node blocks, and they are the same source. On Linux
 * /dev/random once blocked while the entropy pool refilled and
 * /dev/urandom did not; that distinction was removed from Linux itself
 * in 2020 because it caused more stalled boots than it prevented weak
 * keys. Here the source is RDSEED/RDRAND through vx_random, which either
 * produces a value or reports that it did not — there is no pool to
 * drain and nothing to wait for. A short read is possible and is
 * reported honestly, which is the same contract SYS_RANDOM has.
 */
static int64_t dev_read(vfs_desc_t *d, void *buf, uint32_t len) {
    if (!d->readable) return -VXE_BADF;
    if (!len) return 0;
    uint8_t *out = (uint8_t *)buf;

    switch (d->devid) {
    case DEV_NULL:
        return 0;                       /* end of file, immediately */

    case DEV_ZERO:
    case DEV_FULL:
        for (uint32_t i = 0; i < len; i++) out[i] = 0;
        d->pos += len;
        return (int64_t)len;

    case DEV_RANDOM:
    case DEV_URANDOM: {
        uint32_t done = 0;
        while (done < len) {
            uint32_t want = len - done;
            if (want > 64) want = 64;
            const uint32_t got = vx_random(out + done, want);
            done += got;
            if (got != want) break;     /* the generator ran dry */
        }
        d->pos += done;
        return (int64_t)done;
    }

    case DEV_TTY:
        /*
         * The same refusal SYS_READ gives for the console streams, and
         * for the same reason spelled out there: zero means end of file,
         * and a parser told it has reached the end of input it never
         * opened concludes the input was empty and carries on. The
         * keyboard belongs to the terminal window.
         */
        app_refuse("read: there is no console input in ring 3");
        return -VXE_IO;

    case DEV_DRI_CARD:
    case DEV_DRI_RENDER: {
        uint32_t cap = 0;
        const uint8_t *px = dev_surface_bytes(&cap);
        if (d->pos >= cap) return 0;
        uint32_t n = len;
        if (d->pos + n > cap) n = (uint32_t)(cap - d->pos);
        for (uint32_t i = 0; i < n; i++) out[i] = px[d->pos + i];
        d->pos += n;
        return (int64_t)n;
    }

    default:
        return -VXE_BADF;
    }
}

static int64_t dev_write(vfs_desc_t *d, const void *buf, uint32_t len) {
    if (!d->writable) return -VXE_BADF;
    if (!len) return 0;
    const uint8_t *in = (const uint8_t *)buf;

    switch (d->devid) {
    case DEV_NULL:
    case DEV_ZERO:
        /* Consumed. Reporting the full length rather than zero is the
         * whole point of the node: a caller that loops until everything
         * is written must terminate. */
        d->pos += len;
        return (int64_t)len;

    case DEV_FULL:
        /* The one node that exists in order to fail. */
        return -VXE_NOSPC;

    case DEV_RANDOM:
    case DEV_URANDOM:
        /*
         * Accepted and discarded. On Linux a write here stirs the
         * entropy pool without crediting it, which is to say it changes
         * nothing an attacker could not also change; here there is no
         * pool at all — vx_random goes to the processor's generator
         * every time — so there is nothing to stir. Accepting is right
         * because refusing would break the seeding code in several
         * libraries that write before they read.
         */
        d->pos += len;
        return (int64_t)len;

    case DEV_TTY: {
        /* Bounded and NUL-terminated, exactly as the console path in
         * SYS_WRITE does it, because term_print takes a string. */
        char line[256];
        uint32_t n = len > 255 ? 255 : len;
        for (uint32_t i = 0; i < n; i++) line[i] = (char)in[i];
        line[n] = '\0';
        if (!silent_launch) term_print(line);
#ifdef APP_SELFTEST
        serial_puts("[app] "); serial_puts(line);
#endif
        return (int64_t)len;
    }

    case DEV_DRI_CARD:
    case DEV_DRI_RENDER: {
        uint32_t cap = 0;
        uint8_t *px = dev_surface_bytes(&cap);
        if (d->pos >= cap) {
            /* Past the end of the window. Not an error and not a
             * silent success: the bytes go nowhere and the count says
             * so, which is what a short write means. */
            return 0;
        }
        uint32_t n = len;
        if (d->pos + n > cap) n = (uint32_t)(cap - d->pos);
        for (uint32_t i = 0; i < n; i++) px[d->pos + i] = in[i];
        d->pos += n;
        dev_surface_touched();
        return (int64_t)n;
    }

    default:
        return -VXE_BADF;
    }
}

/*
 * What stat says about a node.
 *
 * S_IFCHR and a size, and the two numbers a graphics stack matches on.
 * Filled into the same vx_stat_t the filesystem path fills, so the
 * translation in vls_core.c that turns it into a Linux struct stat needs
 * to know nothing about devices.
 */
static void dev_stat(const dev_node_t *n, vx_stat_t *st) {
    st->size     = dev_length(n->id);
    /* The device number in Linux's encoding, put where a caller looking
     * for an inode will at least find something stable rather than
     * zero — two opens of the same node agree, which is the only
     * property anything depends on. */
    st->ino      = ((uint64_t)n->major << 8) | n->minor;
    st->mode     = VX_S_IFCHR;
    st->nlink    = 1;
    st->mtime_ns = 0;
}

static void dev_stat_dir(vx_stat_t *st) {
    st->size     = 0;
    st->ino      = 0;
    st->mode     = VX_S_IFDIR;
    st->nlink    = 2;
    st->mtime_ns = 0;
}

#endif /* DEVFS_H */
