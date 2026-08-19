#ifndef NETX_H
#define NETX_H

/*
 * src/netx.h — the parts of a network stack that are not the protocol.
 *
 * The stack underneath speaks IPv4, ICMP, UDP, DNS and TCP, and it
 * speaks them to anybody. Everything here is about deciding *whether*,
 * *how fast*, and *on whose behalf* — which is the difference between a
 * host that is on a network and a host that can be left on one.
 *
 *   A firewall that tracks connections, so a reply is allowed because a
 *   request went out and not because its port number looked friendly.
 *   A translator, so more than one address can share this one.
 *   A window that grows, so a fast link is not throttled to 64 KB in
 *   flight by a field that was sized in 1981.
 *   Resolvers that fail over, so one unreachable server is not the end
 *   of name resolution.
 *   A clock that agrees with the rest of the world.
 */

#include <stdint.h>

/* ===== STATEFUL FIREWALL =====
 *
 * The distinction that matters is between a packet that starts something
 * and a packet that continues something. A stateless filter cannot tell
 * them apart and has to guess from port numbers, which is why the rules
 * for one always end up either too permissive or unusable.
 *
 * A connection is remembered when it goes out. A packet coming in is
 * allowed if it belongs to one of those, and otherwise has to satisfy a
 * rule on its own merits. That single idea is most of what a firewall
 * is.
 *
 * TCP gets more than membership: the sequence numbers are tracked, and a
 * segment claiming to belong to a connection but landing outside its
 * window is dropped. That is what stops an attacker who can guess the
 * addresses and ports from injecting a reset into somebody else's
 * connection -- they would also have to guess where in a 32-bit sequence
 * space the window currently is.
 */
#define FW_MAX_CONN  64
#define FW_MAX_RULES 16

#define FW_PROTO_ICMP 1
#define FW_PROTO_TCP  6
#define FW_PROTO_UDP  17

#define FW_ALLOW 0
#define FW_DENY  1

#define FW_DIR_IN  0
#define FW_DIR_OUT 1

typedef struct {
    uint8_t  used;
    uint8_t  proto;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint32_t last_seen;          /* frame ticks, for expiry */
    /* TCP only: where the window is, so an out-of-window segment can be
     * told from a legitimate one. */
    uint32_t send_next;          /* what we have sent up to     */
    uint32_t recv_next;          /* what we expect next          */
    uint32_t recv_window;
    uint8_t  established;
} fw_conn_t;

typedef struct {
    uint8_t  used;
    uint8_t  action;             /* FW_ALLOW or FW_DENY   */
    uint8_t  direction;
    uint8_t  proto;              /* 0 for any             */
    uint32_t addr, mask;         /* 0 for any             */
    uint16_t port_lo, port_hi;   /* 0,0 for any           */
} fw_rule_t;

static fw_conn_t fw_conns[FW_MAX_CONN];
static fw_rule_t fw_rules[FW_MAX_RULES];
static uint32_t  fw_clock = 0;
static uint64_t  fw_dropped = 0, fw_allowed = 0, fw_out_of_window = 0;
static int       fw_enabled = 1;

/* A connection is forgotten if nothing has used it for this long. Two
 * minutes of frames: long enough that a slow reply still finds its
 * connection, short enough that the table does not fill with the
 * remains of a port scan. */
#define FW_IDLE_TICKS (60 * 120)

static void fw_tick(void) {
    fw_clock++;
    for (int i = 0; i < FW_MAX_CONN; i++)
        if (fw_conns[i].used && fw_clock - fw_conns[i].last_seen > FW_IDLE_TICKS)
            fw_conns[i].used = 0;
}

static fw_conn_t *fw_find(uint8_t proto, uint32_t lip, uint16_t lport,
                          uint32_t rip, uint16_t rport) {
    for (int i = 0; i < FW_MAX_CONN; i++) {
        fw_conn_t *c = &fw_conns[i];
        if (!c->used || c->proto != proto) continue;
        if (c->local_ip == lip && c->local_port == lport &&
            c->remote_ip == rip && c->remote_port == rport) return c;
    }
    return 0;
}

/* Record an outgoing flow. Called by the stack when it sends. */
static fw_conn_t *fw_track(uint8_t proto, uint32_t lip, uint16_t lport,
                           uint32_t rip, uint16_t rport, uint32_t seq) {
    fw_conn_t *c = fw_find(proto, lip, lport, rip, rport);
    if (!c) {
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < FW_MAX_CONN; i++) {
            if (!fw_conns[i].used) { victim = i; break; }
            if (fw_conns[i].last_seen < oldest) {
                oldest = fw_conns[i].last_seen;
                victim = i;
            }
        }
        if (victim < 0) return 0;
        c = &fw_conns[victim];
        for (uint64_t k = 0; k < sizeof(*c); k++) ((uint8_t *)c)[k] = 0;
        c->used = 1;
        c->proto = proto;
        c->local_ip = lip; c->local_port = lport;
        c->remote_ip = rip; c->remote_port = rport;
        c->recv_window = 65535;
    }
    c->last_seen = fw_clock;
    if (proto == FW_PROTO_TCP) c->send_next = seq;
    return c;
}

static int fw_rule_matches(const fw_rule_t *r, uint8_t dir, uint8_t proto,
                           uint32_t addr, uint16_t port) {
    if (!r->used) return 0;
    if (r->direction != dir) return 0;
    if (r->proto && r->proto != proto) return 0;
    if (r->mask && (addr & r->mask) != (r->addr & r->mask)) return 0;
    if (r->port_hi && (port < r->port_lo || port > r->port_hi)) return 0;
    return 1;
}

/*
 * Should this arriving packet be accepted?
 *
 * Rules are consulted in order and the first match settles it, which is
 * the convention every firewall uses and the one that makes a ruleset
 * readable top to bottom. A packet matching nothing is allowed if it
 * belongs to a tracked connection and dropped otherwise -- default deny
 * inbound, which is the only default worth having.
 */
static int fw_allow_inbound(uint8_t proto, uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            uint32_t seq, uint32_t len) {
    if (!fw_enabled) return 1;

    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!fw_rule_matches(&fw_rules[i], FW_DIR_IN, proto, src_ip, dst_port))
            continue;
        if (fw_rules[i].action == FW_DENY) { fw_dropped++; return 0; }
        fw_allowed++;
        return 1;
    }

    fw_conn_t *c = fw_find(proto, dst_ip, dst_port, src_ip, src_port);
    if (!c) { fw_dropped++; return 0; }

    if (proto == FW_PROTO_TCP && c->established) {
        /*
         * The sequence number must land inside the window we advertised.
         * Comparison is by subtraction so that wrapping is handled: a
         * sequence space of 2^32 wraps in under an hour on a fast link,
         * and a naive "greater than" test rejects everything afterwards.
         */
        uint32_t off = seq - c->recv_next;
        if (off > c->recv_window + len) {
            fw_out_of_window++;
            fw_dropped++;
            return 0;
        }
    }
    c->last_seen = fw_clock;
    fw_allowed++;
    return 1;
}

static void fw_note_established(uint8_t proto, uint32_t lip, uint16_t lport,
                                uint32_t rip, uint16_t rport, uint32_t rcv_next) {
    fw_conn_t *c = fw_find(proto, lip, lport, rip, rport);
    if (!c) return;
    c->established = 1;
    c->recv_next = rcv_next;
}

static int fw_add_rule(uint8_t action, uint8_t dir, uint8_t proto,
                       uint32_t addr, uint32_t mask,
                       uint16_t lo, uint16_t hi) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (fw_rules[i].used) continue;
        fw_rules[i].used = 1;
        fw_rules[i].action = action;
        fw_rules[i].direction = dir;
        fw_rules[i].proto = proto;
        fw_rules[i].addr = addr;
        fw_rules[i].mask = mask;
        fw_rules[i].port_lo = lo;
        fw_rules[i].port_hi = hi;
        return i;
    }
    return -1;
}

static int fw_conn_count(void) {
    int n = 0;
    for (int i = 0; i < FW_MAX_CONN; i++) if (fw_conns[i].used) n++;
    return n;
}

/* ===== NETWORK ADDRESS TRANSLATION =====
 *
 * One address, many hosts behind it. A packet going out has its source
 * rewritten to this machine's address and its source port replaced with
 * one from a pool; the mapping is remembered so the reply can be turned
 * back again.
 *
 * The port is what carries the information. There is nowhere else to put
 * it: the reply comes back addressed to this machine and the only thing
 * distinguishing one inside host's traffic from another's is which port
 * it was sent from. That is why translation and the port pool are the
 * same mechanism rather than two.
 */
#define NAT_MAX_MAP 64
#define NAT_PORT_LO 49152
#define NAT_PORT_HI 65535

typedef struct {
    uint8_t  used;
    uint8_t  proto;
    uint32_t inside_ip;
    uint16_t inside_port;
    uint16_t outside_port;       /* what we rewrote it to */
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t last_seen;
} nat_map_t;

static nat_map_t nat_maps[NAT_MAX_MAP];
static uint16_t  nat_next_port = NAT_PORT_LO;
static uint32_t  nat_outside_ip = 0;
static uint64_t  nat_translated = 0;
static int       nat_enabled = 0;

static uint16_t nat_alloc_port(void) {
    for (int tries = 0; tries < (NAT_PORT_HI - NAT_PORT_LO); tries++) {
        uint16_t p = nat_next_port++;
        if (nat_next_port > NAT_PORT_HI) nat_next_port = NAT_PORT_LO;
        int taken = 0;
        for (int i = 0; i < NAT_MAX_MAP; i++)
            if (nat_maps[i].used && nat_maps[i].outside_port == p) taken = 1;
        if (!taken) return p;
    }
    return 0;
}

/* Outbound: rewrite the source, remember how. Returns the new port. */
static uint16_t nat_out(uint8_t proto, uint32_t in_ip, uint16_t in_port,
                        uint32_t rem_ip, uint16_t rem_port) {
    if (!nat_enabled) return in_port;

    for (int i = 0; i < NAT_MAX_MAP; i++) {
        nat_map_t *m = &nat_maps[i];
        if (!m->used || m->proto != proto) continue;
        if (m->inside_ip == in_ip && m->inside_port == in_port &&
            m->remote_ip == rem_ip && m->remote_port == rem_port) {
            m->last_seen = fw_clock;
            return m->outside_port;
        }
    }

    uint16_t port = nat_alloc_port();
    if (!port) return in_port;

    for (int i = 0; i < NAT_MAX_MAP; i++) {
        nat_map_t *m = &nat_maps[i];
        if (m->used) continue;
        m->used = 1;
        m->proto = proto;
        m->inside_ip = in_ip;
        m->inside_port = in_port;
        m->outside_port = port;
        m->remote_ip = rem_ip;
        m->remote_port = rem_port;
        m->last_seen = fw_clock;
        nat_translated++;
        return port;
    }
    return in_port;
}

/* Inbound: which inside host does this belong to? */
static int nat_in(uint8_t proto, uint16_t out_port, uint32_t rem_ip,
                  uint16_t rem_port, uint32_t *ip, uint16_t *port) {
    if (!nat_enabled) return 0;
    for (int i = 0; i < NAT_MAX_MAP; i++) {
        nat_map_t *m = &nat_maps[i];
        if (!m->used || m->proto != proto) continue;
        if (m->outside_port != out_port) continue;
        if (m->remote_ip != rem_ip || m->remote_port != rem_port) continue;
        m->last_seen = fw_clock;
        *ip = m->inside_ip;
        *port = m->inside_port;
        return 1;
    }
    return 0;
}

/* ===== TCP WINDOW SCALING =====
 *
 * The window field in a TCP header is sixteen bits, so without help a
 * sender may have at most 64 KB outstanding. On a link with any real
 * bandwidth-delay product that is the limit on throughput, and it has
 * nothing to do with how fast either end can go: a 100 Mbit link with 40
 * ms of latency needs half a megabyte in flight to stay busy, and 64 KB
 * gets an eighth of the line.
 *
 * The fix is an option, negotiated once in the handshake, giving a shift
 * to apply to every window afterwards. It is only legal in a SYN, and
 * both ends must offer it or neither uses it -- which is why the scale
 * is remembered per connection rather than assumed.
 */
typedef struct {
    uint8_t  in_use;
    uint8_t  send_scale;         /* what we apply to their window */
    uint8_t  recv_scale;         /* what we told them to apply    */
    uint32_t cwnd;               /* congestion window, in segments */
    uint32_t ssthresh;
    uint32_t rtt_ms;
    uint32_t rto_ms;
} tcp_window_t;

#define TCP_MSS 1460

static void tcp_window_init(tcp_window_t *w) {
    w->in_use = 0;
    w->send_scale = 0;
    /* Seven gives a 8 MB window, which is past anything this machine
     * will be asked to sustain and still inside the specification's
     * limit of fourteen. */
    w->recv_scale = 7;
    /* Slow start opens from ten segments, which is what every stack has
     * used since about 2013 -- one or two is a relic of links that no
     * longer exist and costs a small transfer most of its time. */
    w->cwnd = 10;
    w->ssthresh = 64;
    w->rtt_ms = 100;
    w->rto_ms = 1000;
}

/* The three bytes of the option, ready to append to a SYN. */
static int tcp_window_option(const tcp_window_t *w, uint8_t *out) {
    out[0] = 3;                   /* kind: window scale */
    out[1] = 3;                   /* length             */
    out[2] = w->recv_scale;
    return 3;
}

static void tcp_window_peer(tcp_window_t *w, uint8_t their_scale) {
    if (their_scale > 14) their_scale = 14;
    w->send_scale = their_scale;
    w->in_use = 1;
}

static uint32_t tcp_window_value(const tcp_window_t *w, uint16_t raw) {
    return w->in_use ? ((uint32_t)raw << w->send_scale) : raw;
}

/*
 * What to advertise, given how much room the receive buffer has. The
 * shift loses the low bits, so the value is rounded *down* -- rounding
 * up would advertise space that does not exist.
 */
static uint16_t tcp_window_advertise(const tcp_window_t *w, uint32_t space) {
    uint32_t v = w->in_use ? (space >> w->recv_scale) : space;
    return (uint16_t)(v > 65535 ? 65535 : v);
}

/* Congestion control, in the shape everything uses: exponential until
 * the threshold, linear after, halved on loss. */
static void tcp_on_ack(tcp_window_t *w) {
    if (w->cwnd < w->ssthresh) w->cwnd++;
    else if (w->cwnd < 65535)  w->cwnd += 1;
}

static void tcp_on_loss(tcp_window_t *w) {
    w->ssthresh = w->cwnd / 2;
    if (w->ssthresh < 2) w->ssthresh = 2;
    w->cwnd = w->ssthresh;
    w->rto_ms *= 2;
    if (w->rto_ms > 60000) w->rto_ms = 60000;
}

/* Smoothed round trip time, the Jacobson estimator: seven eighths of the
 * old value and an eighth of the new, with the timeout set well clear of
 * it so ordinary variation does not look like loss. */
static void tcp_on_rtt(tcp_window_t *w, uint32_t sample_ms) {
    w->rtt_ms = (w->rtt_ms * 7 + sample_ms) / 8;
    w->rto_ms = w->rtt_ms * 2 + 100;
    if (w->rto_ms < 200) w->rto_ms = 200;
}

/* ===== RESOLVERS THAT FAIL OVER =====
 *
 * One server was configured and one server was tried. If it did not
 * answer, name resolution was over -- which on a network where the
 * router is also the resolver means every name fails whenever the router
 * is busy.
 *
 * Several are held, tried in order, and one that fails is set aside for
 * a while rather than removed: a resolver that is down for a minute
 * should not be gone until reboot.
 */
#define DNS_MAX_SERVERS 4
#define DNS_PENALTY_TICKS (60 * 30)

typedef struct {
    uint32_t ip;
    uint32_t failed_at;
    uint32_t failures;
    uint32_t answers;
} dns_server_t;

static dns_server_t dns_servers[DNS_MAX_SERVERS];
static int dns_server_count = 0;

static void dns_add_server(uint32_t ip) {
    for (int i = 0; i < dns_server_count; i++)
        if (dns_servers[i].ip == ip) return;
    if (dns_server_count >= DNS_MAX_SERVERS) return;
    dns_servers[dns_server_count].ip = ip;
    dns_servers[dns_server_count].failed_at = 0;
    dns_servers[dns_server_count].failures = 0;
    dns_servers[dns_server_count].answers = 0;
    dns_server_count++;
}

/* The next server worth asking, or 0 if every one of them is in
 * penalty -- in which case the caller should try the first anyway,
 * because a stale penalty is better than refusing to resolve. */
static uint32_t dns_pick(int attempt) {
    if (!dns_server_count) return 0;
    for (int n = 0; n < dns_server_count; n++) {
        int i = (attempt + n) % dns_server_count;
        dns_server_t *s = &dns_servers[i];
        if (s->failed_at && fw_clock - s->failed_at < DNS_PENALTY_TICKS)
            continue;
        return s->ip;
    }
    return dns_servers[attempt % dns_server_count].ip;
}

static void dns_note_failure(uint32_t ip) {
    for (int i = 0; i < dns_server_count; i++)
        if (dns_servers[i].ip == ip) {
            dns_servers[i].failed_at = fw_clock;
            dns_servers[i].failures++;
        }
}

static void dns_note_answer(uint32_t ip) {
    for (int i = 0; i < dns_server_count; i++)
        if (dns_servers[i].ip == ip) {
            dns_servers[i].failed_at = 0;
            dns_servers[i].answers++;
        }
}

/* ===== NETWORK TIME =====
 *
 * The clock in this machine comes from the CMOS chip, which is set by
 * whoever last set it and drifts thereafter. NTP replaces that with a
 * time somebody else is responsible for being right about.
 *
 * The protocol's own epoch is 1900 rather than 1970, and the difference
 * -- seventy years including seventeen leap days -- is the constant
 * below. Getting it wrong puts the machine seventy years out, which is
 * at least an obvious kind of wrong.
 */
#define NTP_PORT 123
#define NTP_EPOCH_OFFSET 2208988800u

typedef struct {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec, ref_ts_frac;
    uint32_t orig_ts_sec, orig_ts_frac;
    uint32_t recv_ts_sec, recv_ts_frac;
    uint32_t xmit_ts_sec, xmit_ts_frac;
} __attribute__((packed)) ntp_packet_t;

static uint32_t ntp_last_unix = 0;
static int      ntp_synced = 0;

static void ntp_build_request(ntp_packet_t *p) {
    for (uint64_t i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    /* Leap indicator 0, version 4, mode 3 (client). */
    p->li_vn_mode = (0 << 6) | (4 << 3) | 3;
}

static uint32_t ntp_be32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

/*
 * Read a reply. Returns Unix seconds, or zero if the packet is not a
 * usable answer.
 *
 * Stratum zero is a "kiss of death" -- the server telling a client to go
 * away, usually for polling too often -- and its timestamp means
 * nothing. Accepting it sets the clock from a packet that was explicitly
 * refusing to answer.
 */
static uint32_t ntp_parse(const ntp_packet_t *p) {
    uint8_t mode = p->li_vn_mode & 7;
    uint8_t li   = (uint8_t)(p->li_vn_mode >> 6);
    if (mode != 4 && mode != 5) return 0;      /* not a server reply */
    if (p->stratum == 0 || p->stratum > 15) return 0;
    if (li == 3) return 0;                     /* clock not synchronised */

    uint32_t secs = ntp_be32(p->xmit_ts_sec);
    if (secs < NTP_EPOCH_OFFSET) return 0;
    return secs - NTP_EPOCH_OFFSET;
}

static void ntp_apply(uint32_t unix_secs) {
    if (!unix_secs) return;
    ntp_last_unix = unix_secs;
    ntp_synced = 1;
}

/* Unix seconds broken out, so the clock can be set without a date
 * library. Days-since-epoch to a civil date, by the usual method of
 * shifting the year to start in March so February's length is only ever
 * a property of the last month of the year. */
static void ntp_to_civil(uint32_t unix_secs, int *y, int *mo, int *d,
                         int *h, int *mi, int *s) {
    uint32_t days = unix_secs / 86400;
    uint32_t rem  = unix_secs % 86400;
    *h = (int)(rem / 3600);
    *mi = (int)((rem % 3600) / 60);
    *s = (int)(rem % 60);

    int32_t z = (int32_t)days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yr = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mm = mp + (mp < 10 ? 3 : (uint32_t)-9);

    *y = (int)(yr + (mm <= 2 ? 1 : 0));
    *mo = (int)mm;
    *d = (int)dd;
}

#endif /* NETX_H */
