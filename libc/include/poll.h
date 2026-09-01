#ifndef _POLL_H
#define _POLL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * poll.h — asking whether a descriptor is ready, which this system could
 * not answer until pipes existed to have a changing answer.
 *
 * ---- why poll and not select ----
 *
 * select() describes the descriptors it cares about as a bitmap indexed
 * by descriptor number: the caller clears three of them, sets bits, and
 * passes a maximum. That fixes a largest descriptor at compile time and
 * makes the cost of a call proportional to the highest number rather
 * than to how many are being watched. poll() takes an array of exactly
 * the ones it is interested in. There are sixty-four descriptors per
 * process here so neither would have been slow; poll is simply the
 * better interface, and it is the one GLib's main loop is built on.
 *
 * ---- what the answers mean here ----
 *
 * A pipe is the descriptor whose readiness actually changes, and it is
 * the reason this exists. For the rest the answer is a constant and it
 * is a *true* constant rather than a convenient one: a file, a directory
 * and a device node are always ready, because a read from them waits on
 * the disk and not on another program; the console is writable and never
 * readable, because ring 3 has no console input at all.
 *
 * A socket is reported ready and that is the honest limit of this
 * interface. lwIP knows when a connection has data and does not expose
 * it through the seam in src/vxnet.h, so a poll on a socket means "try
 * it" and the recv that follows blocks. Stated here rather than left to
 * be discovered by someone whose event loop stopped.
 *
 * ---- and the timeout ----
 *
 * A negative timeout means "wait indefinitely" and is bounded at sixty
 * seconds by the kernel. That is not a rounding of infinity: a park that
 * no wake can end is a thread this kernel cannot reap, and for some
 * descriptor kinds no wake exists. A caller that asked to wait forever
 * and gets zero after a minute loops and asks again, which every correct
 * poll caller already does.
 */

/* Linux's numbers, because a port writes POLLIN and would otherwise
 * assemble a set from a different one. */
#define POLLIN    0x001
#define POLLPRI   0x002
#define POLLOUT   0x004
#define POLLERR   0x008   /* output only */
#define POLLHUP   0x010   /* output only */
#define POLLNVAL  0x020   /* output only */
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;         /* negative to skip this entry without removing it */
    short events;     /* what the caller wants to know about             */
    short revents;    /* what happened; written by the call              */
};

/*
 * Returns the number of entries with a non-zero revents, or 0 if the
 * timeout expired with nothing ready, or -1 with errno.
 */
int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* _POLL_H */
