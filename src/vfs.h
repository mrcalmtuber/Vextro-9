#ifndef VEXTRO_VFS_H
#define VEXTRO_VFS_H

/*
 * src/vfs.h — what a file descriptor is on this system.
 *
 * Included from src/desktop.h: after the filesystem layer it reads
 * through, after the accounts it asks permission of, and after
 * uac_prompt, whose question-on-the-screen machinery the network guard
 * below borrows. It is a header rather than an object for the same
 * reason the rest of that file is — it owns mutable state, and
 * include/kernel_shared.h sets out at length why a header that owns
 * state must appear in exactly one translation unit's include closure.
 *
 * ============================================================
 *  1. WHY THERE WAS NOTHING HERE BEFORE
 * ============================================================
 *
 * A program in ring 3 had exactly one way to touch the filesystem:
 * SYS_FS_WRITE, which takes a path and a buffer and replaces a whole
 * file, behind a prompt. There was no open, no read, no directory. And
 * it had no way at all to reach the network, notwithstanding that this
 * kernel has run lwIP and Mbed TLS since src/vxnet.h was written and
 * the browser two windows over is fetching pages over TLS 1.3 right
 * now.
 *
 * Neither gap was an oversight. Both were the absence of the same
 * missing idea. A file read a window at a time has a *position*; a
 * connection has a peer and a state machine; and neither can be named
 * by a call that takes a path and returns. A descriptor is a name for
 * state the kernel holds on a program's behalf between two calls — and
 * once a table of those exists, `read` and `recv` stop being two
 * problems and become one operation applied to two kinds of entry.
 *
 * ============================================================
 *  2. WHERE THE TABLE LIVES, AND WHAT THAT DECIDES
 * ============================================================
 *
 * On the address space, not the thread. include/kernel_shared.h carries
 * the long form; the short form is that the address space is where a
 * process is on this system, and putting the table there makes the three
 * POSIX rules fall out rather than having to be implemented:
 *
 *   threads share descriptors    — clone shares the address space
 *   forks copy them              — fork makes a new one
 *   they close on the last exit  — `refs` is what says which exit that is
 *
 * ============================================================
 *  3. THE FOUR THINGS A DESCRIPTOR CAN BE
 * ============================================================
 *
 *   FD_CONSOLE   0, 1 and 2. Every process has them, and none of them
 *                is looked up in the table — see vfs_get.
 *   FD_FILE      an open file, resolved to an MFT record once.
 *   FD_DIR       a directory, enumerated once at open.
 *   FD_SOCK      a TCP connection, or a TLS session over one.
 *
 * ============================================================
 *  4. WRITING, AND WHY IT IS A BUFFER
 * ============================================================
 *
 * The NTFS writer here creates a record with its data attribute already
 * sized: fs_write_file *replaces* a file rather than updating one in
 * place, and says so where it is defined. There is no "write forty
 * bytes at offset nine hundred" for a descriptor to call.
 *
 * So a descriptor opened for writing carries the file's contents in a
 * kernel buffer, writes land in that, and the whole image goes to
 * fs_write_file when the descriptor is closed or fsync'd. That is what
 * the filesystem underneath can actually do, and saying so here is
 * better than a write() that appears to work and loses everything
 * between the last flush and a power cut.
 *
 * Three consequences, all real, all better read than discovered:
 *
 *   A file open for writing is capped at VFS_WBUF_MAX. The image grows
 *   by doubling and refuses past that with ENOSPC rather than
 *   truncating, because a short write reporting success is the worst of
 *   the available answers.
 *
 *   Two processes writing one file each write their whole image at
 *   close, and the second wins entirely. This is a single-user desktop
 *   and it is what SYS_FS_WRITE already did.
 *
 *   The MFT record behind an open file can go stale. fs_open resolves a
 *   path to a record once and fs_pread reads through that record
 *   thereafter — the comment on fs_open already warns that a handle is a
 *   key to the file for as long as it is held. Replace that file (which
 *   on this volume means deleting the record and making another) and the
 *   descriptor keeps reading the old one. Nothing in this system does
 *   that to a file another program has open; this is the sentence that
 *   says so rather than the bug report that finds out.
 *
 * A read-only descriptor buffers nothing and goes to the disk a window
 * at a time through fs_pread, which is what makes a 900 MB archive
 * readable on a machine that cannot hold one.
 *
 * ============================================================
 *  5. PARKING, AND THE THREE HAZARDS IT CREATES
 * ============================================================
 *
 * Interrupts are masked for the whole of a system call, and src/syscall.h
 * leans on it: nothing else on this processor runs, so a pointer checked
 * at the top of a handler is still valid at the bottom.
 *
 * A socket call breaks that, necessarily. connect, send and recv park
 * the calling thread — that is what a blocking socket *is* — and every
 * other thread on the machine runs while it is parked. uac_guard has the
 * same property and the long note on SYS_FS_WRITE works through what it
 * costs. Three things follow, and all three are handled below rather
 * than hoped about:
 *
 *   (a) A sibling can close the descriptor underneath a parked call.
 *       lwip_close on a socket another thread is blocked in is a use of
 *       freed memory. So a descriptor counts the calls standing in it,
 *       and close refuses a busy one with EBUSY. That is a deviation
 *       from POSIX, which says the behaviour is undefined; a defined
 *       refusal is the better of the two.
 *
 *   (b) A sibling can munmap the buffer. The kernel would then read or
 *       write an unmapped user page from ring 0 — a kernel fault, not a
 *       program's bug. So socket payloads are staged through a kernel
 *       bounce buffer owned by the descriptor: copied in before the
 *       park, copied out after it, with the user range re-checked on the
 *       way out.
 *
 *   (c) A *shared* staging buffer is not safe across a park. This is the
 *       lesson SYS_FS_WRITE learned the hard way — see the note there
 *       about a second program overwriting a frozen first one's bytes.
 *       The bounce buffers here belong to a descriptor and a direction,
 *       and a second concurrent call in the same direction on the same
 *       descriptor is refused rather than allowed to share one. Two
 *       threads reading one socket at once is not a thing a correct
 *       program does; two threads reading two sockets is, and that
 *       works.
 *
 * ============================================================
 *  6. CLEANING UP WITHOUT FREEZING THE INTERFACE
 * ============================================================
 *
 * sched_reap runs on the compositor thread, inside preempt_disable, in
 * the middle of the frame loop. Closing a TLS session from there would
 * put a close_notify record on the wire and wait for the stack to drain
 * it — seconds, on a bad connection, during which the machine draws
 * nothing. Flushing a dirty file from there would write MFT records
 * through the journal, which is worse.
 *
 * So a dying process's table is *detached* at reap and queued, and a
 * kernel thread does the flushing and closing at ordinary priority.
 * Reap stays a pointer swap.
 */

#include "syscall.h"

/*
 * Sixty-four descriptors per process.
 *
 * The table is about thirty-five kilobytes and it is not free — but it
 * is allocated by the first open rather than at spawn, so every program
 * written before today pays nothing for it, and a browser wants rather
 * more than sixteen.
 *
 * The paged pool, deliberately. Nothing here is touched by an interrupt
 * handler, which is the property the non-paged pool exists to provide;
 * and swap_victim_ok() only ever considers frames whose reverse-map
 * entry names a *user* address, so a classification of "paged" on this
 * side is a hint to a future reclaimer rather than something that can
 * vanish underneath a system call.
 */
#define FD_MAX          64

/* Bounce buffers, per socket and per direction, taken on first use. One
 * call moves at most this much and reports a short count, which is what
 * send() and recv() are defined to be allowed to do and what every
 * correct caller already loops around. */
#define VFS_SOCK_BOUNCE (8 * 1024)

/*
 * The most one file read or write moves in a single call.
 *
 * Not a limit on how large a file may be — a caller loops, and every
 * caller already does, because a short read is what the end of a file
 * looks like. It is a limit on how long one system call may take, and
 * src/syscall.h is explicit that interrupts are masked for the whole of
 * one: "every service below is bounded and short". A megabyte off an
 * NTFS volume is a few milliseconds and keeps that promise; a request
 * for four gigabytes into a mapping would not.
 */
#define VFS_IO_MAX      (1024u * 1024u)

/* Where a write-back image starts and how far it may grow. The floor
 * keeps a program that writes a byte at a time from reallocating on
 * every call; the ceiling is the largest file this system's read path
 * can hold in one piece. */
#define VFS_WBUF_MIN    4096u
#define VFS_WBUF_MAX    ((uint32_t)FS_FILEBUF_MAX)

/* How many names one directory descriptor may hold. The listing is
 * taken whole at open — see vfs_open_dir for why — and grows by
 * doubling from a page's worth, so a directory of three names costs a
 * page and not a megabyte. Past this it is refused with EOVERFLOW
 * rather than silently short, because a truncated directory listing is
 * how a program concludes a file is not there. */
#define VFS_DIR_MAX     4096u

enum {
    FD_FREE = 0,
    FD_CONSOLE,
    FD_FILE,
    FD_DIR,
    FD_SOCK,
    /* A node under /dev. Its own kind rather than a file with a flag,
     * because every operation differs: it is opened without consulting
     * the volume, read and written by src/devfs.h, never written back,
     * and duplicated across a fork by copying eight bytes. See the note
     * at the top of that file for which eight nodes exist and why. */
    FD_DEV,
    /*
     * One end of a pipe. The kind that finally made this system's
     * descriptor table look like a Unix one, and the last of the four
     * things libc/include/unistd.h listed as absent.
     *
     * Its own kind for the reason FD_DEV is: every operation differs.
     * There is no file behind it and no volume, the two ends share one
     * buffer and are counted separately, a read blocks when it is empty
     * and a write blocks when it is full, and a fork *shares* it rather
     * than duplicating it — which is the whole point, since a pipe with
     * a private copy on each side of a fork is two pipes and no channel.
     */
    FD_PIPE
};

/*
 * ============================================================
 *  a buffer with two ends
 * ============================================================
 *
 * ---- why this did not exist until now ----
 *
 * Because nothing could be at the other end of it. src/vfs.h and
 * libc/include/unistd.h both said so for as long as there was no way to
 * start a program from a program: "a pipe would have nobody at the other
 * end". fork has existed since ring 3 did, and exec and wait arrived
 * with the Linux subset, so the sentence stopped being true and this is
 * the consequence.
 *
 * What actually forced it was a port. libgpg-error's spawn-posix.c calls
 * pipe() unconditionally, and visibility.c — the one file that defines
 * every public gpgrt_ name, including the locks libgcrypt needs —
 * references the spawn module, so the pipe comes with the lock whether
 * or not anything spawns. There was no way to link libgcrypt without it.
 *
 * ---- the shape ----
 *
 * One ring, two descriptors, and two counts rather than one. A reader
 * seeing an empty ring must be able to tell "wait, somebody may still
 * write" from "stop, nobody ever will", and a single reference count
 * cannot express that. So writers and readers are counted apart: an
 * empty ring with no writers is end-of-file, and a full ring with no
 * readers is EPIPE.
 *
 * The buffer is one page from the paged pool. That is Unix's own
 * historical size and it is enough that a program which writes a line
 * and then reads the answer never blocks; a program that streams
 * megabytes through it blocks and unblocks, which is what a pipe is for.
 */
#define VFS_PIPE_SIZE 4096

struct vfs_pipe {
    uint8_t *buf;
    uint32_t head;          /* where the next byte is written  */
    uint32_t tail;          /* where the next byte is read     */
    uint32_t len;           /* how many bytes are in it        */
    uint16_t readers;
    uint16_t writers;
};

typedef struct {
    uint8_t  kind;
    uint8_t  cloexec;
    uint8_t  writable;      /* opened with write intent, so wbuf is live */
    uint8_t  readable;
    uint8_t  dirty;         /* the image differs from what is on disk    */
    uint8_t  tls;           /* FD_SOCK: `sock` is a vxsec slot, not lwIP */
    uint8_t  connected;
    uint8_t  busy;          /* a call is parked in this descriptor       */
    uint8_t  busy_rx;
    uint8_t  busy_tx;
    uint8_t  devid;         /* FD_DEV: which node, DEV_* in devfs.h     */
    uint8_t  pipe_w;        /* FD_PIPE: this is the writing end          */
    uint32_t oflags;

    uint64_t pos;           /* file byte offset, or directory index      */

    /* FD_FILE: the resolved handle, and the write-back image. */
    fs_file_t file;
    uint8_t  *wbuf;
    uint32_t  wlen;
    uint32_t  wcap;

    /* FD_DIR: the whole listing, taken at open. */
    vx_dirent_t *ents;
    uint32_t     nents;

    /*
     * FD_PIPE: the rings this end reads from and writes to.
     *
     * Two pointers rather than one, because a socketpair is the same
     * object with both filled in. A pipe's read end has only `prd`, its
     * write end has only `pwr`, and each end of a socketpair has both —
     * crossed, so that what one writes the other reads. Owned by neither
     * descriptor and freed by whichever holder is the last to let go.
     */
    struct vfs_pipe *prd;
    struct vfs_pipe *pwr;

    /* FD_SOCK: the lwIP descriptor or the vxsec slot, and the staging. */
    int      sock;
    uint8_t *rx;
    uint8_t *tx;

    /* Absolute, normalised and settled at open. Kept because the write
     * side needs somewhere to put the image back, and because a refusal
     * that names the file is worth the bytes. */
    char     path[FS_PATH_MAX];
} vfs_desc_t;

struct proc_files {
    vfs_desc_t d[FD_MAX];
};

/* ============================================================
 *  the janitor
 * ============================================================
 *
 * Tables detached from processes that have ended, waiting to be flushed
 * and closed somewhere it is safe to block. Sixteen deep — more dying
 * processes than this system can have at once, since there are only
 * SCHED_MAX_THREADS threads in total — and an overflow is handled by
 * doing the work inline rather than by dropping it, because dropping it
 * loses a file somebody wrote.
 */
#define VFS_RETIRE_MAX 16

static struct proc_files *vfs_retired[VFS_RETIRE_MAX];
static volatile int       vfs_retire_head = 0;
static volatile int       vfs_retire_tail = 0;
static int                vfs_janitor_up  = 0;

static void vfs_desc_reset(vfs_desc_t *d) {
    for (uint64_t i = 0; i < sizeof(*d); i++) ((uint8_t *)d)[i] = 0;
    d->sock = -1;
}

/* Everything one descriptor owns, given back. Safe on a free slot, and
 * safe to call twice. May block — a dirty file is written here — so it
 * runs on the janitor's thread or on a caller that has established it
 * may block, and never inside the frame. */
static void vfs_close_desc(vfs_desc_t *d) {
    if (!d) return;
    if (d->kind == FD_FREE || d->kind == FD_CONSOLE) {
        vfs_desc_reset(d);
        return;
    }

    if (d->kind == FD_FILE) {
        if (d->dirty && d->wbuf) {
            /* Through fs_write_file rather than the driver, so the
             * profile boundary and the pagefile guard apply to a
             * descriptor exactly as they apply to SYS_FS_WRITE. The
             * elevation this needed was granted at open; nothing here
             * asks a question, which is what makes it safe on a thread
             * that has no program to freeze. */
            if (fs_write_file(d->path, d->wbuf, d->wlen) != 0) {
                serial_puts("[vfs] could not write back ");
                serial_puts(d->path);
                serial_putc('\n');
            }
        }
        if (d->wbuf) kfree(d->wbuf);
    } else if (d->kind == FD_DIR) {
        if (d->ents) kfree(d->ents);
    } else if (d->kind == FD_PIPE) {
        /*
         * One end goes back, not the pipe.
         *
         * Which end matters: a reader closing is what turns a later
         * write into EPIPE, and a writer closing is what turns a later
         * read into end-of-file rather than an indefinite wait. So the
         * counts are decremented separately and the waiters on the other
         * side are woken either way — a reader parked on an empty ring
         * has to find out that no writer is left, and it can only find
         * out by being woken and looking.
         */
        struct vfs_pipe *ends[2] = { d->prd, d->pwr };
        for (int e = 0; e < 2; e++) {
            struct vfs_pipe *p = ends[e];
            if (!p) continue;
            if (e == 0) { if (p->readers) p->readers--; }
            else        { if (p->writers) p->writers--; }
            sched_wake_chan(p, 1);
            if (!p->readers && !p->writers) {
                if (p->buf) kfree(p->buf);
                kfree(p);
                /* A socketpair holds the same ring twice only if both
                 * of its ends are this descriptor, which cannot happen;
                 * but the second slot must not be freed twice if a
                 * future caller ever aliases them. */
                if (ends[1 - e] == p) ends[1 - e] = 0;
            }
        }
    } else if (d->kind == FD_SOCK) {
        if (d->sock >= 0) {
            if (d->tls) vxsec_close(d->sock);
            else        vxnet_close(d->sock);
        }
        if (d->rx) kfree(d->rx);
        if (d->tx) kfree(d->tx);
    }

    vfs_desc_reset(d);
}

static void vfs_table_destroy(struct proc_files *f) {
    if (!f) return;
    for (int i = 0; i < FD_MAX; i++) vfs_close_desc(&f->d[i]);
    kfree(f);
}

static void vfs_janitor(void) {
    for (;;) {
        while (vfs_retire_tail != vfs_retire_head) {
            const int t = vfs_retire_tail;
            struct proc_files *f = vfs_retired[t];
            vfs_retired[t] = 0;
            vfs_retire_tail = (t + 1) % VFS_RETIRE_MAX;
            vfs_table_destroy(f);
        }
        /* A bounded park rather than an indefinite one, for the reason
         * every other wait in this kernel is bounded: a wake arriving
         * between the queue test above and the sleep below is a wake
         * that is lost, and re-testing on a timer turns that from a file
         * never written into a file written a quarter of a second late. */
        sched_block_on((void *)&vfs_retired, 250);
    }
}

static void vfs_janitor_start(void) {
    if (vfs_janitor_up) return;
    if (sched_spawn_kernel(vfs_janitor, "vfsjanitor", PRIO_NORMAL))
        vfs_janitor_up = 1;
}

/*
 * Hand a dead process's table to the janitor.
 *
 * Called from app_reaped, which runs on the compositor thread inside the
 * frame's critical section — so this must not block, and does not: it is
 * a store and a wake. If the queue is full, or the janitor never started
 * (which is the case during the boot self-tests, before the compositor
 * exists), the work is done here instead. That is a deliberate choice of
 * a stall over a loss: the alternative is a file a program wrote and
 * closed that never reaches the disk.
 */
static void vfs_retire_table(struct proc_files *f) {
    if (!f) return;

    const int h = vfs_retire_head;
    const int n = (h + 1) % VFS_RETIRE_MAX;
    if (vfs_janitor_up && n != vfs_retire_tail) {
        vfs_retired[h] = f;
        vfs_retire_head = n;
        sched_wake_chan((void *)&vfs_retired, 1);
        return;
    }
    vfs_table_destroy(f);
}

/* ============================================================
 *  the table
 * ============================================================ */

/*
 * This process's table, made if it does not have one yet.
 *
 * Descriptors 0, 1 and 2 are marked in it but are never read *from* it:
 * the console is answered before the table is consulted, so a program
 * that has never opened anything still has a stdout without paying
 * thirty-five kilobytes for one. What the entries do is stop vfs_alloc
 * handing out 0.
 */
static struct proc_files *vfs_files(addr_space_t *as, int create) {
    if (!as) return 0;
    if (as->files || !create) return as->files;

    struct proc_files *f =
        (struct proc_files *)kmalloc_pool(sizeof(struct proc_files),
                                          KPOOL_PAGED);
    if (!f) return 0;
    for (int i = 0; i < FD_MAX; i++) vfs_desc_reset(&f->d[i]);
    for (int i = 0; i < 3; i++) f->d[i].kind = FD_CONSOLE;
    as->files = f;
    return f;
}

/*
 * What descriptor `fd` is, without needing a table to exist.
 *
 * The console is the reason this is separate from vfs_get. 0, 1 and 2
 * are open in every process from the moment it starts, and a program
 * that only ever prints must not pay thirty-five kilobytes for the
 * privilege — so with no table at all, the low three are FD_CONSOLE and
 * everything else is FD_FREE.
 *
 * Once a table exists the table is the truth, including about the low
 * three: closing fd 0 and opening a file has been how a program
 * redirects its own input since before any of this, and it works here
 * because close writes FD_FREE into the entry and vfs_alloc then hands
 * that number back out.
 */
static int vfs_kind_of(addr_space_t *as, int64_t fd) {
    if (fd < 0 || fd >= FD_MAX) return FD_FREE;
    if (!as) return FD_FREE;
    if (!as->files) return fd < 3 ? FD_CONSOLE : FD_FREE;
    return as->files->d[fd].kind;
}

static vfs_desc_t *vfs_get(addr_space_t *as, int64_t fd) {
    if (!as || fd < 0 || fd >= FD_MAX) return 0;
    struct proc_files *f = as->files;
    if (!f) return 0;
    vfs_desc_t *d = &f->d[fd];
    return d->kind == FD_FREE ? 0 : d;
}

/* The lowest descriptor not in use, which is what POSIX promises and
 * what a program that redirects with `close(3); open(...)` depends on.
 * Never 0, 1 or 2, which are reserved above. */
static int vfs_alloc(addr_space_t *as) {
    struct proc_files *f = vfs_files(as, 1);
    if (!f) return -VXE_NOMEM;
    for (int i = 3; i < FD_MAX; i++)
        if (f->d[i].kind == FD_FREE) return i;
    return -VXE_MFILE;
}

/*
 * A child's copy of its parent's descriptors.
 *
 * Files and directories duplicate cleanly: the resolved handle is
 * copied, the listing is copied, and a write-back image is copied so
 * the two halves do not scribble on one buffer. Sockets are the hard
 * case, and are handled by not duplicating them.
 */
static int vfs_clone_table(addr_space_t *child, addr_space_t *parent) {
    struct proc_files *src = parent ? parent->files : 0;
    if (!src) return 0;

    struct proc_files *dst = vfs_files(child, 1);
    if (!dst) return -1;

    for (int i = 0; i < FD_MAX; i++) {
        vfs_desc_t *s = &src->d[i];
        vfs_desc_t *o = &dst->d[i];
        if (s->kind == FD_FREE || s->kind == FD_CONSOLE) continue;

        /*
         * ---- why a fork does not inherit a connection ----
         *
         * A TCP socket is one endpoint of one conversation. Two
         * processes holding it both read the same stream, and each byte
         * goes to whichever asked first — so a forked pair sharing a
         * connection do not each get the response, they get alternating
         * halves of it, and it presents as a server that has gone mad.
         * Duplicating the *slot* instead would be worse: two lwIP
         * descriptors for one connection, and two closes of it.
         *
         * Unix answers this by sharing one open file description between
         * the two and making the author deal with it. This system has
         * nowhere to put a shared description — the table *is* the
         * description — so the honest answer is that the child does not
         * get the socket, and finds the descriptor closed rather than
         * subtly wrong.
         */
        if (s->kind == FD_SOCK) continue;

        /*
         * A pipe is *shared* across a fork, and this is the one kind
         * where that is the whole point rather than a convenience. A
         * child with a private copy of the ring is not at the other end
         * of anything; the classic use — parent reads, child writes — is
         * exactly two processes holding two ends of one buffer.
         *
         * So the pointer is copied and the count on this end is raised,
         * before the general copy below, which would otherwise leave
         * both descriptors pointing at a ring that thinks it has one
         * holder.
         */
        if (s->kind == FD_PIPE) {
            if (s->prd) s->prd->readers++;
            if (s->pwr) s->pwr->writers++;
        }

        *o = *s;
        o->wbuf = 0; o->ents = 0; o->rx = 0; o->tx = 0;
        o->wcap = 0;
        o->busy = o->busy_rx = o->busy_tx = 0;

        if (s->kind == FD_FILE && s->wbuf && s->wcap) {
            o->wbuf = (uint8_t *)kmalloc_pool(s->wcap, KPOOL_PAGED);
            if (!o->wbuf) { vfs_desc_reset(o); return -1; }
            for (uint32_t k = 0; k < s->wlen; k++) o->wbuf[k] = s->wbuf[k];
            o->wcap = s->wcap;
        }

        if (s->kind == FD_DIR && s->ents && s->nents) {
            const uint64_t bytes = (uint64_t)s->nents * sizeof(vx_dirent_t);
            o->ents = (vx_dirent_t *)kmalloc_pool(bytes, KPOOL_PAGED);
            if (!o->ents) { vfs_desc_reset(o); return -1; }
            for (uint64_t k = 0; k < bytes; k++)
                ((uint8_t *)o->ents)[k] = ((const uint8_t *)s->ents)[k];
        }
    }
    return 0;
}

/* ============================================================
 *  may this program leave the machine?
 * ============================================================
 *
 * Not uac_guard, and the reason is written beside the flag this records
 * into: uac_guard grants by elevating the token, which covers writing
 * files, changing the registry and writing raw sectors together. "May
 * this fetch a page" and "may this overwrite the partition table" are
 * not the same question and must not share a bit.
 *
 * Four differences from uac_guard, each deliberate:
 *
 *   It does not require an administrator. Browsing is not
 *   administration, and an account that may not install software may
 *   perfectly well read a web page.
 *
 *   It records a refusal as well as a grant. A program told no makes its
 *   next attempt a millisecond later; without remembering, that is a
 *   second prompt, and a stream of prompts is how a person is trained to
 *   click yes.
 *
 *   With nobody signed in there is nobody to ask, so the answer is no,
 *   immediately. That is the state the boot self-tests run in, and it is
 *   also the state a machine sitting at its login screen is in — which
 *   is exactly when an unattended program should not be opening
 *   connections.
 *
 *   Loopback never reaches here. A connection to 127.0.0.0/8 does not
 *   leave the machine, so there is nothing to ask about.
 */
static int net_guard(const char *host) {
    addr_space_t *as = vmm_current;

    /* A kernel caller: the browser, the package store, the clock. There
     * is a person driving it and the interface asked them whatever it
     * needed to. */
    if (!as || !cur_thread || !sched_running) return 1;

    if (as->net_ok == NET_ALLOWED) return 1;
    if (as->net_ok == NET_REFUSED) {
        app_refuse("connect: this program has already been refused the "
                   "network");
        return 0;
    }

    /* The policy setting uac_guard reads, meaning the same thing here. */
    if (uac_level == UAC_NEVER) {
        as->net_ok = NET_ALLOWED;
        return 1;
    }

    if (user_current < 0) {
        serial_puts("[net] refused (nobody is signed in): ");
        serial_puts(cur_thread->name);
        serial_puts(" wanted to reach ");
        serial_puts(host);
        serial_putc('\n');
        as->net_ok = NET_REFUSED;
        return 0;
    }

    const int ok = uac_prompt("reach the network", host);
    as->net_ok = ok ? NET_ALLOWED : NET_REFUSED;

    serial_puts(ok ? "[net] granted: " : "[net] denied: ");
    serial_puts(cur_thread->name);
    serial_puts(" -> ");
    serial_puts(host);
    serial_putc('\n');

    if (!ok) {
        char note[NOTIFY_TEXT];
        str_copy(note, "Denied the network: ", sizeof(note));
        str_append(note, cur_thread->name, sizeof(note));
        notify_push(NOTE_WARN, note);
    }
    return ok;
}

/* 127.0.0.0/8: never asked about, because it never leaves. */
static int vfs_is_loopback(const uint8_t ip[4]) {
    return ip[0] == 127;
}

/*
 * A dotted quad, parsed here rather than resolved.
 *
 * The order matters: a name has to be recognised as a literal address
 * *before* net_guard sees it, because resolving a name is itself a
 * question asked of a server somewhere else. Without this, connecting to
 * 127.0.0.1 would send a DNS query about "127.0.0.1" to the outside
 * world in order to find out whether it was allowed to talk to the
 * outside world.
 *
 * Returns 1 and fills `out` for a well-formed address, 0 for anything
 * else — including "127.0.0.1.example.com", which is a name and is
 * treated as one.
 */
static int vfs_parse_ipv4(const char *s, uint8_t out[4]) {
    if (!s) return 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (++digits > 3 || v > 255) return 0;
            s++;
        }
        out[part] = (uint8_t)v;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    return *s == '\0';
}

/* "localhost" without asking anybody. Every system resolves it to the
 * loopback address out of a local file; this one has no such file, and a
 * DNS query for it would be both slow and a leak of the fact that
 * something here is starting up. */
static int vfs_host_is_loopback(const char *host, uint8_t out[4]) {
    if (str_eq(host, "localhost")) {
        out[0] = 127; out[1] = 0; out[2] = 0; out[3] = 1;
        return 1;
    }
    if (vfs_parse_ipv4(host, out) && vfs_is_loopback(out)) return 1;
    return 0;
}

/* ============================================================
 *  opening
 * ============================================================ */

/* ============================================================
 *  reading and writing a pipe
 * ============================================================
 *
 * Both block, and blocking is the substance of the thing rather than an
 * implementation detail: a pipe is how one program waits for another
 * without either of them polling. The park is the same bounded one the
 * futex and wait4 use — sched_block_on with a timeout, re-entered by the
 * caller's own loop — so a missed wake costs a few milliseconds instead
 * of becoming a thread this kernel can never reap.
 *
 * Interrupts are masked for the whole of a system call, and these are
 * called from inside one, so the ring cannot change underneath the
 * arithmetic between two statements. What *can* happen is that the
 * thread parks, at which point another thread runs and may fill or drain
 * it — which is why every field is re-read after every park rather than
 * kept in a local across one.
 */
static int64_t vfs_pipe_read(vfs_desc_t *d, void *buf, uint32_t len) {
    struct vfs_pipe *p = d->prd;
    if (!d->readable || !p) return -VXE_BADF;
    if (!len) return 0;

    for (;;) {
        if (p->len) break;
        /*
         * Empty. Whether that is "wait" or "stop" is the one question a
         * single reference count could not have answered: with no writer
         * left, nothing will ever arrive, and answering zero is
         * end-of-file exactly as it is on a file that has been read to
         * its end. With a writer still holding the other end, this
         * thread waits.
         */
        if (!p->writers) return 0;
        sched_block_on(p, 50);
    }

    uint32_t n = len;
    if (n > p->len) n = p->len;
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % VFS_PIPE_SIZE;
    }
    p->len -= n;

    /* A writer parked on a full ring now has room. Woken whether or not
     * one is actually waiting, because finding out would cost more than
     * the call it saves. */
    sched_wake_chan(p, 1);
    return (int64_t)n;
}

static int64_t vfs_pipe_write(vfs_desc_t *d, const void *buf, uint32_t len) {
    struct vfs_pipe *p = d->pwr;
    if (!d->writable || !p) return -VXE_BADF;
    if (!len) return 0;

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < len) {
        /*
         * Nobody to read it. EPIPE rather than a wait, because a wait
         * would be forever — and on a system with signals this is also
         * where SIGPIPE would be raised. It is not raised here, and that
         * is a decision rather than an omission: the signal's default
         * action is to kill the process, which for a program that
         * checks its return value would turn a handled error into a
         * death. A program that wants the signal can ask for the
         * behaviour by checking for EPIPE, which it has to do anyway.
         */
        if (!p->readers) return done ? (int64_t)done : -32 /* EPIPE */;

        if (p->len == VFS_PIPE_SIZE) {
            /*
             * Full. A short write is legal once anything has been
             * transferred — POSIX only promises atomicity below PIPE_BUF
             * — but blocking is what a caller expects and what makes the
             * common "write a line, read the answer" pattern work
             * without a loop on their side.
             */
            sched_block_on(p, 50);
            continue;
        }

        uint32_t room = VFS_PIPE_SIZE - p->len;
        uint32_t n = len - done;
        if (n > room) n = room;
        for (uint32_t i = 0; i < n; i++) {
            p->buf[p->head] = in[done + i];
            p->head = (p->head + 1) % VFS_PIPE_SIZE;
        }
        p->len += n;
        done += n;
        sched_wake_chan(p, 1);
    }
    return (int64_t)done;
}

/*
 * Both ends at once.
 *
 * The two descriptors are taken before either is filled in, because
 * vfs_alloc hands back the lowest free number and taking the second
 * after the first is initialised would be fine — but taking both first
 * makes the failure path one branch instead of two. `flags` carries
 * O_CLOEXEC, which is the only one pipe2 has that this system can
 * honour: O_NONBLOCK would need a non-blocking read, and there is no
 * readiness interface behind these descriptors to build one on.
 */
static struct vfs_pipe *vfs_ring_new(void) {
    struct vfs_pipe *p = (struct vfs_pipe *)kmalloc(sizeof(struct vfs_pipe));
    if (!p) return 0;
    for (uint64_t i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    p->buf = (uint8_t *)kmalloc_pool(VFS_PIPE_SIZE, KPOOL_PAGED);
    if (!p->buf) { kfree(p); return 0; }
    return p;
}

static void vfs_ring_free(struct vfs_pipe *p) {
    if (!p) return;
    if (p->buf) kfree(p->buf);
    kfree(p);
}

/*
 * Both ends at once, for a pipe and for a socketpair.
 *
 * `two_way` is the whole difference. A pipe is one ring: the first
 * descriptor reads it and the second writes it, and each end can do only
 * its own half. A socketpair is two rings crossed: each descriptor reads
 * one and writes the other, so both ends can do both — which is what
 * makes it a *pair of sockets* rather than a pipe, and is the only thing
 * a program uses one for.
 *
 * The two descriptors are taken before either is filled in, so the
 * failure path is one branch rather than two. `flags` carries O_CLOEXEC,
 * which is the only one this system can honour: O_NONBLOCK would need a
 * non-blocking read, and blocking is what these descriptors are for.
 */
static int vfs_pipe_create(addr_space_t *as, int fds[2], uint32_t flags,
                           int two_way) {
    if (!as) return -VXE_PERM;

    const int afd = vfs_alloc(as);
    if (afd < 0) return afd;
    /* Claimed so the second allocation cannot hand back the same number;
     * filled in properly below. */
    as->files->d[afd].kind = FD_PIPE;

    const int bfd = vfs_alloc(as);
    if (bfd < 0) { vfs_desc_reset(&as->files->d[afd]); return bfd; }

    struct vfs_pipe *ab = vfs_ring_new();          /* a writes, b reads */
    struct vfs_pipe *ba = two_way ? vfs_ring_new() : 0;
    if (!ab || (two_way && !ba)) {
        vfs_ring_free(ab);
        vfs_ring_free(ba);
        vfs_desc_reset(&as->files->d[afd]);
        vfs_desc_reset(&as->files->d[bfd]);
        return -VXE_NOMEM;
    }

    const uint8_t ce = (flags & VX_O_CLOEXEC) ? 1 : 0;

    vfs_desc_t *a = &as->files->d[afd];
    vfs_desc_reset(a);
    a->kind = FD_PIPE; a->cloexec = ce; a->sock = -1;
    vfs_desc_t *b = &as->files->d[bfd];
    vfs_desc_reset(b);
    b->kind = FD_PIPE; b->cloexec = ce; b->sock = -1;

    if (two_way) {
        a->prd = ba; a->pwr = ab;
        b->prd = ab; b->pwr = ba;
        a->readable = a->writable = 1;
        b->readable = b->writable = 1;
        ab->writers = 1; ab->readers = 1;
        ba->writers = 1; ba->readers = 1;
        str_copy(a->path, "socketpair:0", sizeof(a->path));
        str_copy(b->path, "socketpair:1", sizeof(b->path));
    } else {
        /* fds[0] reads and fds[1] writes, which is the order pipe(2) has
         * had since the seventh edition and which every caller assumes
         * without checking. */
        a->prd = ab; a->readable = 1;
        b->pwr = ab; b->writable = 1; b->pipe_w = 1;
        ab->readers = 1; ab->writers = 1;
        str_copy(a->path, "pipe:read", sizeof(a->path));
        str_copy(b->path, "pipe:write", sizeof(b->path));
    }

    fds[0] = afd;
    fds[1] = bfd;
    return 0;
}

/*
 * Is this descriptor ready?
 *
 * The whole of the readiness interface this system has, and it is
 * deliberately answered per-kind rather than by asking a driver, because
 * for four of the six kinds the answer is a constant that is *true*
 * rather than a convenient lie: a file, a directory and a device node
 * are always ready — a read from them completes without waiting on
 * anything but the disk — and the console has no input at all, so it is
 * writable and never readable.
 *
 * A pipe is the interesting one and the reason this exists. A socket is
 * reported ready and that is the honest limit of it: lwIP's readiness is
 * not exposed through vxnet, so a poll on a socket says "try", and the
 * recv that follows blocks. Said here rather than discovered.
 */
#define VFS_READY_READ  1
#define VFS_READY_WRITE 2
#define VFS_READY_HUP   4

static int vfs_ready(addr_space_t *as, int fd) {
    const int kind = vfs_kind_of(as, fd);
    if (kind == FD_FREE) return -1;
    if (kind == FD_CONSOLE) return VFS_READY_WRITE;

    vfs_desc_t *d = vfs_get(as, fd);
    if (!d) return -1;

    if (kind == FD_PIPE) {
        int r = 0;
        if (d->prd) {
            /* Data, or no writer left — and the second is readable in
             * the sense that matters: a read returns immediately, with
             * zero, which is what the caller needs to be told. */
            if (d->prd->len) r |= VFS_READY_READ;
            if (!d->prd->writers) r |= (VFS_READY_READ | VFS_READY_HUP);
        }
        if (d->pwr) {
            if (d->pwr->len < VFS_PIPE_SIZE) r |= VFS_READY_WRITE;
            if (!d->pwr->readers) r |= VFS_READY_HUP;
        }
        if (!d->prd && !d->pwr) r = VFS_READY_HUP;
        return r;
    }

    int r = VFS_READY_WRITE;
    if (d->readable) r |= VFS_READY_READ;
    return r;
}

/*
 * The nodes under /dev, which are not on the volume and must answer
 * before it is consulted. Included here rather than beside vfs.h in
 * src/desktop.h because it needs vfs_desc_t and vfs_desc_reset above and
 * has to be complete before vfs_open below — a device node is a name the
 * filesystem has never heard of, so the lookup is short-circuited rather
 * than having its failure patched up afterwards.
 */
#include "devfs.h"

/*
 * The listing, taken whole.
 *
 * A directory here is an NTFS B-tree and enumerating it is a walk with
 * disk reads in it. Doing that once and answering out of a buffer makes
 * readdir() cheap, and — the part that matters more — makes the sequence
 * *stable*: a program reading a directory while something is created in
 * it gets a consistent answer rather than a name repeated or skipped.
 */
typedef struct {
    vx_dirent_t *ents;
    uint32_t     n;
    uint32_t     cap;
    int          failed;
} vfs_dir_build_t;

static void vfs_dir_collect(void *ctx, const char *name, uint32_t size,
                            int is_dir) {
    vfs_dir_build_t *b = (vfs_dir_build_t *)ctx;
    if (b->failed) return;

    if (b->n >= b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 16;
        if (cap > VFS_DIR_MAX) { b->failed = 1; return; }
        vx_dirent_t *grown =
            (vx_dirent_t *)kmalloc_pool((uint64_t)cap * sizeof(vx_dirent_t),
                                        KPOOL_PAGED);
        if (!grown) { b->failed = 1; return; }
        for (uint64_t i = 0; i < (uint64_t)b->n * sizeof(vx_dirent_t); i++)
            ((uint8_t *)grown)[i] = ((const uint8_t *)b->ents)[i];
        if (b->ents) kfree(b->ents);
        b->ents = grown;
        b->cap  = cap;
    }

    vx_dirent_t *e = &b->ents[b->n];
    for (uint64_t i = 0; i < sizeof(*e); i++) ((uint8_t *)e)[i] = 0;
    e->size = size;
    e->type = is_dir ? VX_DT_DIR : VX_DT_REG;
    str_copy(e->name, name, (int)sizeof(e->name));
    e->namelen = (uint32_t)str_len(e->name);
    b->n++;
}

static int vfs_open_dir(vfs_desc_t *d, const char *abs, uint32_t oflags) {
    if ((oflags & VX_O_ACCMODE) != VX_O_RDONLY) return -VXE_ISDIR;

    vfs_dir_build_t b;
    b.ents = 0; b.n = 0; b.cap = 0; b.failed = 0;

    if (fs_list_ctx(abs, vfs_dir_collect, &b) != 0) {
        if (b.ents) kfree(b.ents);
        return -VXE_NOENT;
    }
    if (b.failed) {
        if (b.ents) kfree(b.ents);
        return -VXE_OVERFLOW;
    }

    d->ents     = b.ents;
    d->nents    = b.n;
    d->kind     = FD_DIR;
    d->readable = 1;
    d->oflags   = oflags;
    d->cloexec  = (oflags & VX_O_CLOEXEC) ? 1 : 0;
    d->pos      = 0;
    d->sock     = -1;
    str_copy(d->path, abs, sizeof(d->path));
    return 0;
}

static int vfs_open_reg(vfs_desc_t *d, const char *abs, uint32_t oflags,
                        int exists) {
    const uint32_t acc = oflags & VX_O_ACCMODE;
    const int wants_write = (acc == VX_O_WRONLY || acc == VX_O_RDWR);

    if (!exists && !(oflags & VX_O_CREAT)) return -VXE_NOENT;
    if (exists && (oflags & VX_O_CREAT) && (oflags & VX_O_EXCL))
        return -VXE_EXIST;
    if (wants_write && !fs_writable()) return -VXE_ROFS;

    str_copy(d->path, abs, sizeof(d->path));
    d->oflags   = oflags;
    d->readable = (acc == VX_O_RDONLY || acc == VX_O_RDWR) ? 1 : 0;
    d->writable = wants_write ? 1 : 0;
    d->cloexec  = (oflags & VX_O_CLOEXEC) ? 1 : 0;
    d->pos      = 0;
    d->sock     = -1;

    if (exists && !(oflags & VX_O_TRUNC)) {
        if (fs_open(abs, &d->file) != 0) return -VXE_NOENT;
    } else {
        /* Nothing on the disk to read through: it either does not exist
         * yet or is about to be replaced entirely. */
        d->file.valid = 0;
        d->file.size  = 0;
    }

    if (!wants_write) {
        d->kind = FD_FILE;
        return 0;
    }

    /*
     * The write-back image.
     *
     * Seeded with what is on the disk unless the caller asked for the
     * file to be emptied. O_WRONLY without O_APPEND seeds too, which is
     * not what a Unix filesystem does and is what this one must: a
     * write of forty bytes to a file of nine hundred has to produce a
     * file of nine hundred, and the only way to reach that through a
     * writer that replaces whole records is to have the other eight
     * hundred and sixty in hand.
     */
    uint32_t seed = 0;
    if (exists && !(oflags & VX_O_TRUNC)) {
        if (d->file.size > (uint64_t)VFS_WBUF_MAX) return -VXE_OVERFLOW;
        seed = (uint32_t)d->file.size;
    }

    uint32_t cap = seed < VFS_WBUF_MIN ? VFS_WBUF_MIN : seed;
    d->wbuf = (uint8_t *)kmalloc_pool(cap, KPOOL_PAGED);
    if (!d->wbuf) return -VXE_NOMEM;
    d->wcap = cap;
    d->wlen = 0;

    if (seed) {
        uint32_t got = 0;
        if (fs_pread(&d->file, 0, d->wbuf, seed, &got) != 0) {
            kfree(d->wbuf);
            d->wbuf = 0;
            d->wcap = 0;
            return -VXE_IO;
        }
        d->wlen = got;
    }
    if (oflags & VX_O_APPEND) d->pos = d->wlen;

    /* A file created by this open exists as far as the program is
     * concerned from this moment, though nothing has reached the disk
     * yet: O_CREAT|O_EXCL is a claim, and a second open before the first
     * close has to see it. Marking it dirty is also what makes an empty
     * creat() produce an empty file rather than nothing at all. */
    if (!exists || (oflags & VX_O_TRUNC)) d->dirty = 1;

    d->kind = FD_FILE;
    return 0;
}

/*
 * Resolve, check, and fill in a descriptor. Everything about *policy*
 * happened before this was called — the service routine in src/desktop.h
 * asks uac_guard when there is write intent, because that question has
 * to be asked while there is still a ring-3 thread to freeze.
 *
 * Returns 0, or a negated error number.
 */
static int vfs_open(vfs_desc_t *d, const char *abs, uint32_t oflags) {
    /*
     * The device nodes first, and *before* fs_stat rather than after it
     * fails.
     *
     * Order is the whole of the semantics here. Asking the volume first
     * and falling back would mean a file called \dev\null on the disk
     * shadowed the device — which is a way for a program to be handed a
     * file where it asked for a sink, and a way for anything that can
     * create a file to change what another program's redirection does.
     * /dev is a name this system reserves, and reserving it means
     * answering for it first.
     */
    if (dev_claims(abs)) {
        if (dev_is_dir(abs)) {
            if (oflags & (VX_O_CREAT | VX_O_TRUNC)) return -VXE_ISDIR;
            return dev_open_dir(d, abs, oflags);
        }
        if (oflags & VX_O_DIRECTORY) return -VXE_NOTDIR;
        return dev_open(d, abs, oflags);
    }

    uint64_t size = 0;
    int is_dir = 0;
    const int exists = fs_stat(abs, &size, &is_dir);

    if ((oflags & VX_O_DIRECTORY) && (!exists || !is_dir)) return -VXE_NOTDIR;
    if (exists && is_dir) return vfs_open_dir(d, abs, oflags);
    return vfs_open_reg(d, abs, oflags, exists);
}

/* ============================================================
 *  reading and writing a descriptor
 * ============================================================
 *
 * Both take a kernel buffer. The service routines in src/desktop.h are
 * what touch user memory, before and after, so the checking and the
 * staging happen in one place and every path through them is visible in
 * one screenful.
 */

static int64_t vfs_file_read(vfs_desc_t *d, void *buf, uint32_t len) {
    if (!d->readable) return -VXE_BADF;
    if (!len) return 0;

    /* A file being written is answered out of its own image, not off the
     * disk: the two differ by everything written since it was opened,
     * and a read that missed those would be a read of the past. */
    if (d->writable && d->wbuf) {
        if (d->pos >= d->wlen) return 0;
        uint32_t n = (uint32_t)(d->wlen - d->pos);
        if (n > len) n = len;
        const uint8_t *src = d->wbuf + d->pos;
        for (uint32_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = src[i];
        d->pos += n;
        return n;
    }

    if (!d->file.valid) return 0;
    uint32_t got = 0;
    if (fs_pread(&d->file, d->pos, buf, len, &got) != 0) return -VXE_IO;
    d->pos += got;
    return got;
}

static int vfs_wbuf_reserve(vfs_desc_t *d, uint64_t need) {
    if (need <= d->wcap) return 0;
    if (need > (uint64_t)VFS_WBUF_MAX) return -VXE_NOSPC;

    uint64_t cap = d->wcap ? d->wcap : VFS_WBUF_MIN;
    while (cap < need) cap *= 2;
    if (cap > (uint64_t)VFS_WBUF_MAX) cap = VFS_WBUF_MAX;

    uint8_t *grown = (uint8_t *)kmalloc_pool(cap, KPOOL_PAGED);
    if (!grown) return -VXE_NOMEM;
    for (uint32_t i = 0; i < d->wlen; i++) grown[i] = d->wbuf[i];
    if (d->wbuf) kfree(d->wbuf);
    d->wbuf = grown;
    d->wcap = (uint32_t)cap;
    return 0;
}

static int64_t vfs_file_write(vfs_desc_t *d, const void *buf, uint32_t len) {
    if (!d->writable || !d->wbuf) return -VXE_BADF;
    if (!len) return 0;

    if (d->oflags & VX_O_APPEND) d->pos = d->wlen;

    const uint64_t end = d->pos + len;
    const int rc = vfs_wbuf_reserve(d, end);
    if (rc != 0) return rc;

    /* A seek past the end followed by a write leaves a hole, and a hole
     * in a file that is going to be written out whole has to be
     * something. Zero, as every filesystem that supports the operation
     * answers. */
    for (uint64_t i = d->wlen; i < d->pos; i++) d->wbuf[i] = 0;

    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) d->wbuf[d->pos + i] = src[i];

    d->pos = end;
    if (end > d->wlen) d->wlen = (uint32_t)end;
    d->dirty = 1;
    return (int64_t)len;
}

/* Put a dirty image on the disk without closing the descriptor, and stop
 * it being written a second time at close if nothing else changes. */
static int vfs_flush(vfs_desc_t *d) {
    if (d->kind != FD_FILE) return 0;
    if (!d->dirty || !d->wbuf) return 0;
    if (fs_write_file(d->path, d->wbuf, d->wlen) != 0) return -VXE_IO;
    d->dirty = 0;
    return 0;
}

static int64_t vfs_seek(vfs_desc_t *d, int64_t off, int whence) {
    if (d->kind == FD_SOCK || d->kind == FD_CONSOLE) return -VXE_SPIPE;

    int64_t end;
    /* A device node's end is as long as the window for the two graphics
     * nodes and zero for the rest — which is what makes SEEK_END on
     * /dev/zero answer zero, exactly as it does on Linux, rather than
     * reading a file size out of a union nothing filled in. */
    if (d->kind == FD_DEV)                 end = (int64_t)dev_length(d->devid);
    else if (d->kind == FD_DIR)            end = (int64_t)d->nents;
    else if (d->writable && d->wbuf)       end = (int64_t)d->wlen;
    else                                   end = (int64_t)d->file.size;

    int64_t want;
    switch (whence) {
    case VX_SEEK_SET: want = off; break;
    case VX_SEEK_CUR: want = (int64_t)d->pos + off; break;
    case VX_SEEK_END: want = end + off; break;
    default: return -VXE_INVAL;
    }
    if (want < 0) return -VXE_INVAL;

    /* Seeking past the end of a readable file is allowed and reads
     * nothing; past the end of a writable one is allowed and makes a
     * hole when something is written there. Both are what lseek
     * promises. What is refused is a position no write-back image on
     * this system could reach. */
    if (d->kind == FD_FILE && d->writable && want > (int64_t)VFS_WBUF_MAX)
        return -VXE_NOSPC;

    d->pos = (uint64_t)want;
    return want;
}

#ifdef APP_SELFTEST
/*
 * ============================================================
 *  an echo server, so that a ring-3 socket has somebody to talk to
 * ============================================================
 *
 * The socket calls can be checked for their refusals, for their argument
 * validation, and for coming back rather than hanging, without anything
 * on the far end. What cannot be checked that way is the thing they
 * exist for: that bytes go out and come back.
 *
 * And that is the part most worth checking, because it is the most
 * intricate code in this file. sys_sock_send and sys_sock_recv stage
 * through a bounce buffer, park, and copy out with the user range
 * re-checked afterwards -- see hazards (a), (b) and (c) at the head of
 * this file. None of that runs unless a transfer completes.
 *
 * There is no way to be the far end from ring 3: bind, listen and accept
 * are deliberately not exported, because nothing in ring 3 on this
 * system is a server. The kernel has them, and the remote desktop in
 * src/net/rdp.c already uses them exactly this way. So the far end is a
 * kernel thread, the traffic never leaves the machine, and the test
 * works in a room with no network.
 */
#define VFS_ECHO_PORT 7777

static int vfs_echo_ready = 0;

static void vfs_echo_server(void) {
    const int s = vxnet_listen(VFS_ECHO_PORT, 4);
    if (s < 0) {
        serial_puts("[vfs] echo server: could not listen\n");
        return;
    }
    vfs_echo_ready = 1;
    serial_puts("[vfs] echo server listening on 127.0.0.1:7777\n");

    for (;;) {
        uint8_t peer[4] = { 0, 0, 0, 0 };
        const int c = vxnet_accept(s, peer);
        if (c < 0) continue;

        /* Read and write it straight back until the peer stops. A
         * short read is the end of the stream, which is what the
         * client's close looks like from here. */
        for (;;) {
            uint8_t buf[512];
            const int n = vxnet_recv(c, buf, (int)sizeof(buf));
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                const int w = vxnet_send(c, buf + off, n - off);
                if (w <= 0) { off = n; break; }
                off += w;
            }
        }
        vxnet_close(c);
    }
}

static void vfs_echo_start(void) {
    if (!vxnet_up()) {
        serial_puts("[vfs] echo server not started: the network is down\n");
        return;
    }
    sched_spawn_kernel(vfs_echo_server, "vfsecho", PRIO_NORMAL);
    /* Give it long enough to bind before the first client tries. The
     * client retries anyway, so this only keeps the log tidy. */
    for (int i = 0; i < 40 && !vfs_echo_ready; i++) sched_sleep_ms(25);
}

/*
 * The write-back path, driven from the kernel.
 *
 * apps/fdtest.c cannot do this. Opening a file for writing goes through
 * uac_guard, which with nobody signed in refuses immediately — and a
 * boot self-test runs before there is an account to sign in to, let
 * alone somebody at the keyboard to answer. That refusal is itself
 * asserted over there and is the right behaviour; what it leaves
 * unchecked is everything after it.
 *
 * So the same descriptor operations are driven here, where there is no
 * ring-3 program for the guard to be about. What is being checked is the
 * part this file is responsible for: that a write lands in the image,
 * that a read of the same descriptor sees it, that closing puts it on
 * the disk through fs_write_file, and that what comes back off the disk
 * is what went in.
 */
static void vfs_selftest(void) {
    const char *path = "/vfs-selftest.tmp";
    static const char payload[] = "written through a descriptor\n";
    const uint32_t len = (uint32_t)(sizeof(payload) - 1);
    int checks = 0, failures = 0;

    #define VFS_OK(what, cond) do {                                       \
        checks++;                                                         \
        if (!(cond)) { failures++; serial_puts("FAIL  "); }               \
        else serial_puts(" ok   ");                                       \
        serial_puts(what); serial_putc('\n');                             \
    } while (0)

    if (!fs_writable()) {
        serial_puts("[vfs] selftest skipped: no writable volume\n");
        return;
    }

    vfs_desc_t d;
    vfs_desc_reset(&d);

    VFS_OK("open for writing",
           vfs_open(&d, path, VX_O_WRONLY | VX_O_CREAT | VX_O_TRUNC) == 0);
    VFS_OK("it is a file with an image",
           d.kind == FD_FILE && d.writable && d.wbuf);

    VFS_OK("write reports every byte",
           vfs_file_write(&d, payload, len) == (int64_t)len);
    VFS_OK("the image holds them", d.wlen == len && d.dirty);

    /* A read of the same descriptor is answered out of the image, not
     * off the disk -- the two differ by everything written since it was
     * opened, and a read that missed those would be a read of the past. */
    d.pos = 0;
    d.readable = 1;
    {
        char back[64];
        for (uint32_t i = 0; i < sizeof(back); i++) back[i] = 0;
        const int64_t got = vfs_file_read(&d, back, (uint32_t)sizeof(back));
        int same = (got == (int64_t)len);
        for (uint32_t i = 0; same && i < len; i++)
            if (back[i] != payload[i]) same = 0;
        VFS_OK("reading it back through the same descriptor", same);
    }

    /* Seeking past the end and writing leaves a hole, which has to be
     * zero rather than whatever was in the buffer. */
    {
        VFS_OK("seek past the end",
               vfs_seek(&d, (int64_t)len + 8, VX_SEEK_SET) == (int64_t)len + 8);
        VFS_OK("write after it", vfs_file_write(&d, "X", 1) == 1);
        VFS_OK("the hole is zero-filled",
               d.wlen == len + 9 && d.wbuf[len] == 0 &&
               d.wbuf[len + 7] == 0 && d.wbuf[len + 8] == 'X');
    }

    /* Truncate back, then flush and close: this is where it reaches the
     * disk. */
    {
        const int rc = vfs_wbuf_reserve(&d, len);
        VFS_OK("reserving what is already there is free", rc == 0);
        d.wlen = len;
        d.dirty = 1;
    }

    VFS_OK("fsync writes it out", vfs_flush(&d) == 0);
    VFS_OK("and the descriptor is no longer dirty", !d.dirty);

    vfs_close_desc(&d);
    VFS_OK("close releases the image", d.kind == FD_FREE && d.wbuf == 0);

    /* Off the disk, through the ordinary read path, which knows nothing
     * about any of the above. */
    {
        uint64_t got = 0;
        const void *disk = fs_read_file(path, &got);
        int same = (disk && got == len);
        for (uint32_t i = 0; same && i < len; i++)
            if (((const char *)disk)[i] != payload[i]) same = 0;
        VFS_OK("what is on the disk is what was written", same);
    }

    VFS_OK("and it can be deleted again", fs_delete(path) == 0);

    serial_puts("[vfs] selftest: ");
    serial_put_dec((uint32_t)checks);
    serial_puts(" checks, ");
    serial_put_dec((uint32_t)failures);
    serial_puts(failures ? " failures - FAILED\n" : " failures - all passed\n");

    #undef VFS_OK
}
#endif /* APP_SELFTEST */

static void vfs_fill_stat(vx_stat_t *st, uint64_t size, int is_dir,
                          uint64_t ino) {
    for (uint64_t i = 0; i < sizeof(*st); i++) ((uint8_t *)st)[i] = 0;
    st->size     = size;
    st->ino      = ino;
    st->mode     = is_dir ? VX_S_IFDIR : VX_S_IFREG;
    st->nlink    = 1;
    st->mtime_ns = 0;         /* see the note in src/syscall.h */
}

/* ============================================================
 *  sockets
 * ============================================================ */

/* A bounce buffer for one direction, taken on first use. Failure is
 * ENOMEM at the call rather than at socket(), because a program that
 * opens eight sockets and uses two should pay for two. */
static int vfs_sock_stage(uint8_t **slot) {
    if (*slot) return 0;
    *slot = (uint8_t *)kmalloc_pool(VFS_SOCK_BOUNCE, KPOOL_PAGED);
    return *slot ? 0 : -VXE_NOMEM;
}

#endif /* VEXTRO_VFS_H */
