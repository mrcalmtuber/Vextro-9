#ifndef NETSTACK_H
#define NETSTACK_H

#include "tls.h"
#include "netx.h"

#include <stdint.h>

/*
 * Vextro 9 network stack.
 *
 * Layers: Ethernet / ARP / IPv4 / ICMP / UDP / TCP, plus a DNS resolver
 * and an asynchronous HTTP/1.0 client on top.  Everything is polled from
 * the main render loop (net_poll), no interrupts, no dynamic memory.
 *
 * Tuned for QEMU user-mode (slirp) networking:
 *   guest 10.0.2.15 / gateway 10.0.2.2 / DNS 10.0.2.3
 */

/* ===== SERIAL FORMATTING HELPERS ===== */

static void net_log(const char *msg) {
    serial_puts("[NET] ");
    serial_puts(msg);
    serial_putc('\n');
}

/* ===== BYTE ORDER ===== */

static inline uint16_t ntohs(uint16_t net) {
    return (uint16_t)((net >> 8) | (net << 8));
}

static inline uint16_t htons(uint16_t host) {
    return (uint16_t)((host >> 8) | (host << 8));
}

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}

static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* ===== NETWORK CONFIGURATION ===== */

static uint8_t net_our_ip[4]  = { 10, 0, 2, 15 };
static uint8_t net_gw_ip[4]   = { 10, 0, 2, 2 };
static uint8_t net_dns_ip[4]  = { 10, 0, 2, 3 };
static uint8_t net_mask[4]    = { 255, 255, 255, 0 };

static uint32_t net_ticks = 0;       /* advanced once per frame (~60 Hz) */

static inline uint32_t ip_u32(const uint8_t *ip) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
           ((uint32_t)ip[2] << 8)  | (uint32_t)ip[3];
}

__attribute__((unused))
static inline void ip_from_u32(uint32_t v, uint8_t *out) {
    out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);  out[3] = (uint8_t)v;
}

static void ip_to_str(const uint8_t *ip, char *out) {
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        if (v >= 100) out[pos++] = (char)('0' + v / 100);
        if (v >= 10)  out[pos++] = (char)('0' + (v / 10) % 10);
        out[pos++] = (char)('0' + v % 10);
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

/* Parse dotted-quad; returns 1 on success */
static int ip_parse(const char *s, uint8_t *out) {
    int part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            digits++;
            if (val > 255 || digits > 3) return 0;
        } else if (*s == '.' || *s == '\0') {
            if (digits == 0 || part > 3) return 0;
            out[part++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*s == '\0') break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

/* ===== CHECKSUM (RFC 1071) ===== */

static uint32_t csum_add(uint32_t sum, const uint8_t *d, int len) {
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)d[i] << 8) | d[i + 1];
    if (len & 1)
        sum += (uint32_t)d[len - 1] << 8;
    return sum;
}

static uint16_t csum_finish(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ===== ETHERNET ===== */

#define ETH_HEADER_SIZE  14
#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806

struct eth_header {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

static const uint8_t BROADCAST_MAC[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static int mac_match(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* Shared TX assembly buffer (single-threaded main loop) */
static uint8_t net_tx_frame[1600];

/* ===== ARP ===== */

#define ARP_HW_ETHERNET  1
#define ARP_PROTO_IPV4   0x0800
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

struct arp_packet {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

#define ARP_CACHE_SIZE 16

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static void arp_cache_update(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            arp_cache[i].valid = 1;
            return;
        }
    }
    arp_cache[0].ip = ip;
    for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = mac[j];
    arp_cache[0].valid = 1;
}

static int arp_cache_lookup(uint32_t ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) out_mac[j] = arp_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

static void arp_send_request(const uint8_t *target_ip) {
    uint8_t *frame = net_tx_frame;
    struct eth_header *eth = (struct eth_header *)frame;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src_mac[i] = e1000_mac[i];
    eth->ethertype = htons(ETHERTYPE_ARP);

    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HEADER_SIZE);
    arp->hw_type    = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ARP_PROTO_IPV4);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = htons(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = e1000_mac[i];
    for (int i = 0; i < 4; i++) arp->sender_ip[i]  = net_our_ip[i];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0;
    for (int i = 0; i < 4; i++) arp->target_ip[i]  = target_ip[i];

    e1000_transmit(frame, ETH_HEADER_SIZE + sizeof(struct arp_packet));
}

static void arp_send_reply(const uint8_t *dst_mac, const uint8_t *dst_ip,
                           const uint8_t *src_mac, const uint8_t *src_ip) {
    uint8_t *frame = net_tx_frame;
    struct eth_header *eth = (struct eth_header *)frame;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = src_mac[i];
    eth->ethertype = htons(ETHERTYPE_ARP);

    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HEADER_SIZE);
    arp->hw_type    = htons(ARP_HW_ETHERNET);
    arp->proto_type = htons(ARP_PROTO_IPV4);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->opcode     = htons(ARP_OP_REPLY);
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = src_mac[i];
    for (int i = 0; i < 4; i++) arp->sender_ip[i]  = src_ip[i];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = dst_mac[i];
    for (int i = 0; i < 4; i++) arp->target_ip[i]  = dst_ip[i];

    e1000_transmit(frame, ETH_HEADER_SIZE + sizeof(struct arp_packet));
}

/* Next-hop selection: on-subnet peers directly, everything else via GW */
static void net_next_hop(const uint8_t *dst_ip, uint8_t *hop_ip) {
    uint32_t d = ip_u32(dst_ip), o = ip_u32(net_our_ip), m = ip_u32(net_mask);
    if ((d & m) == (o & m))
        for (int i = 0; i < 4; i++) hop_ip[i] = dst_ip[i];
    else
        for (int i = 0; i < 4; i++) hop_ip[i] = net_gw_ip[i];
}

/* Resolve next-hop MAC; sends an ARP request (rate-limited) on miss.
 * Returns 1 when out_mac is valid. */
static uint32_t arp_last_req_tick = 0;

static int net_resolve_mac(const uint8_t *dst_ip, uint8_t *out_mac) {
    uint8_t hop[4];
    net_next_hop(dst_ip, hop);
    if (arp_cache_lookup(ip_u32(hop), out_mac))
        return 1;
    if (net_ticks - arp_last_req_tick > 15) {   /* ~4 req/sec max */
        arp_send_request(hop);
        arp_last_req_tick = net_ticks;
    }
    return 0;
}

/* ===== IPv4 ===== */

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ipv4_header {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t ident;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} __attribute__((packed));

static uint16_t ip_ident_counter = 1;

/* Build eth+IP headers in net_tx_frame, payload already copied at
 * net_tx_frame + ETH_HEADER_SIZE + 20.  Returns 0 on ARP miss. */
static int ipv4_send(const uint8_t *dst_ip, uint8_t proto, uint16_t payload_len) {
    uint8_t dst_mac[6];
    if (!net_resolve_mac(dst_ip, dst_mac))
        return 0;

    struct eth_header *eth = (struct eth_header *)net_tx_frame;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = e1000_mac[i];
    eth->ethertype = htons(ETHERTYPE_IPV4);

    struct ipv4_header *ip = (struct ipv4_header *)(net_tx_frame + ETH_HEADER_SIZE);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons((uint16_t)(20 + payload_len));
    ip->ident      = htons(ip_ident_counter++);
    ip->flags_frag = htons(0x4000);   /* DF */
    ip->ttl        = 64;
    ip->proto      = proto;
    ip->checksum   = 0;
    for (int i = 0; i < 4; i++) ip->src[i] = net_our_ip[i];
    for (int i = 0; i < 4; i++) ip->dst[i] = dst_ip[i];
    ip->checksum = htons(csum_finish(csum_add(0, (const uint8_t *)ip, 20)));

    e1000_transmit(net_tx_frame, (uint16_t)(ETH_HEADER_SIZE + 20 + payload_len));
    return 1;
}

/* TCP/UDP pseudo-header checksum */
static uint16_t l4_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                            uint8_t proto, const uint8_t *seg, int seg_len) {
    uint32_t sum = 0;
    sum = csum_add(sum, src_ip, 4);
    sum = csum_add(sum, dst_ip, 4);
    sum += proto;
    sum += (uint32_t)seg_len;
    sum = csum_add(sum, seg, seg_len);
    return csum_finish(sum);
}

/* ===== ICMP (echo request + reply) ===== */

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t ident;
    uint16_t seq;
} __attribute__((packed));

/* Outgoing ping state (used by the terminal `ping` command) */
static int      ping_active = 0;
static uint8_t  ping_target[4];
static uint16_t ping_seq = 0;
static uint32_t ping_sent_tick = 0;
static int      ping_replies = 0;
static int      ping_sent_count = 0;
static uint32_t ping_last_rtt = 0;      /* in ticks (~16.7ms each) */
static int      ping_got_reply = 0;     /* set when a reply arrives */

static void icmp_send_echo(const uint8_t *dst_ip, uint16_t seq) {
    uint8_t *pkt = net_tx_frame + ETH_HEADER_SIZE + 20;
    struct icmp_header *icmp = (struct icmp_header *)pkt;
    icmp->type = 8; icmp->code = 0;
    icmp->checksum = 0;
    icmp->ident = htons(0x5343);        /* 'SC' */
    icmp->seq = htons(seq);
    for (int i = 0; i < 24; i++)
        pkt[sizeof(struct icmp_header) + i] = (uint8_t)('A' + i);
    int len = (int)sizeof(struct icmp_header) + 24;
    icmp->checksum = htons(csum_finish(csum_add(0, pkt, len)));
    ipv4_send(dst_ip, IP_PROTO_ICMP, (uint16_t)len);
}

static void net_handle_icmp(const struct ipv4_header *ip,
                            const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct icmp_header)) return;
    const struct icmp_header *icmp = (const struct icmp_header *)data;

    if (icmp->type == 8) {
        /* Echo request → reply with same payload */
        uint8_t *pkt = net_tx_frame + ETH_HEADER_SIZE + 20;
        if (len > 1400) return;
        for (uint16_t i = 0; i < len; i++) pkt[i] = data[i];
        struct icmp_header *out = (struct icmp_header *)pkt;
        out->type = 0;
        out->checksum = 0;
        out->checksum = htons(csum_finish(csum_add(0, pkt, len)));
        ipv4_send(ip->src, IP_PROTO_ICMP, len);
    } else if (icmp->type == 0) {
        /* Echo reply for our ping */
        if (ping_active && ntohs(icmp->ident) == 0x5343) {
            ping_replies++;
            ping_got_reply = 1;
            ping_last_rtt = net_ticks - ping_sent_tick;
        }
    }
}

/* ===== UDP ===== */

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

static int udp_send(const uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port,
                    const uint8_t *payload, uint16_t payload_len) {
    uint8_t *seg = net_tx_frame + ETH_HEADER_SIZE + 20;
    struct udp_header *udp = (struct udp_header *)seg;
    uint16_t seg_len = (uint16_t)(sizeof(struct udp_header) + payload_len);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(seg_len);
    udp->checksum = 0;
    for (uint16_t i = 0; i < payload_len; i++)
        seg[sizeof(struct udp_header) + i] = payload[i];
    udp->checksum = htons(l4_checksum(net_our_ip, dst_ip, IP_PROTO_UDP,
                                      seg, seg_len));
    if (udp->checksum == 0) udp->checksum = 0xFFFF;
    return ipv4_send(dst_ip, IP_PROTO_UDP, seg_len);
}

/* ===== DNS RESOLVER (async, single query in flight) ===== */

#define DNS_STATE_IDLE     0
#define DNS_STATE_QUERYING 1
#define DNS_STATE_DONE     2
#define DNS_STATE_FAIL     3

static int      dns_state = DNS_STATE_IDLE;
static char     dns_name[128];
static uint8_t  dns_result[4];
static uint16_t dns_txid = 0;
static uint16_t dns_port = 0;
static uint32_t dns_sent_tick = 0;
static int      dns_retries = 0;

static int net_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void dns_send_query(void) {
    uint8_t q[300];
    int nlen = net_strlen(dns_name);
    if (nlen > 200) { dns_state = DNS_STATE_FAIL; return; }

    dns_txid = (uint16_t)(0x5000 | (net_ticks & 0xFFF));
    dns_port = (uint16_t)(0xC000 | (net_ticks & 0x0FFF));

    q[0] = (uint8_t)(dns_txid >> 8); q[1] = (uint8_t)dns_txid;
    q[2] = 0x01; q[3] = 0x00;         /* RD=1 */
    q[4] = 0; q[5] = 1;               /* 1 question */
    q[6] = 0; q[7] = 0;
    q[8] = 0; q[9] = 0;
    q[10] = 0; q[11] = 0;

    /* encode QNAME as length-prefixed labels */
    int qi = 12;
    int label_start = qi++;
    for (int i = 0; i <= nlen; i++) {
        char c = dns_name[i];
        if (c == '.' || c == '\0') {
            q[label_start] = (uint8_t)(qi - label_start - 1);
            if (c == '\0') break;
            label_start = qi++;
        } else {
            q[qi++] = (uint8_t)c;
        }
    }
    q[qi++] = 0;                      /* root label */
    q[qi++] = 0; q[qi++] = 1;         /* QTYPE A */
    q[qi++] = 0; q[qi++] = 1;         /* QCLASS IN */

    udp_send(net_dns_ip, dns_port, 53, q, (uint16_t)qi);
    dns_sent_tick = net_ticks;
}

static void dns_resolve_start(const char *name) {
    int i = 0;
    while (name[i] && i < 127) { dns_name[i] = name[i]; i++; }
    dns_name[i] = '\0';

    /* IP literal short-circuits DNS */
    if (ip_parse(dns_name, dns_result)) {
        dns_state = DNS_STATE_DONE;
        return;
    }
    dns_state = DNS_STATE_QUERYING;
    dns_retries = 0;
    dns_send_query();
}

/* Skip a (possibly compressed) DNS name, return new offset or -1 */
static int dns_skip_name(const uint8_t *p, int off, int len) {
    while (off < len) {
        uint8_t b = p[off];
        if (b == 0) return off + 1;
        if ((b & 0xC0) == 0xC0) return off + 2;
        off += b + 1;
    }
    return -1;
}

static void dns_handle_reply(const uint8_t *d, uint16_t len) {
    if (dns_state != DNS_STATE_QUERYING) return;
    if (len < 12) return;
    uint16_t txid = (uint16_t)((d[0] << 8) | d[1]);
    if (txid != dns_txid) return;

    uint16_t qdcount = (uint16_t)((d[4] << 8) | d[5]);
    uint16_t ancount = (uint16_t)((d[6] << 8) | d[7]);
    if ((d[3] & 0x0F) != 0 || ancount == 0) {   /* RCODE != 0 or no answers */
        dns_state = DNS_STATE_FAIL;
        return;
    }

    int off = 12;
    for (int i = 0; i < qdcount; i++) {
        off = dns_skip_name(d, off, len);
        if (off < 0) { dns_state = DNS_STATE_FAIL; return; }
        off += 4;
    }
    for (int i = 0; i < ancount && off + 10 < len; i++) {
        off = dns_skip_name(d, off, len);
        if (off < 0 || off + 10 > len) break;
        uint16_t rtype = (uint16_t)((d[off] << 8) | d[off + 1]);
        uint16_t rdlen = (uint16_t)((d[off + 8] << 8) | d[off + 9]);
        off += 10;
        if (rtype == 1 && rdlen == 4 && off + 4 <= len) {
            for (int j = 0; j < 4; j++) dns_result[j] = d[off + j];
            dns_state = DNS_STATE_DONE;
            return;
        }
        off += rdlen;
    }
    dns_state = DNS_STATE_FAIL;
}

static void dns_tick(void) {
    if (dns_state != DNS_STATE_QUERYING) return;
    if (net_ticks - dns_sent_tick > 120) {       /* ~2 s */
        if (++dns_retries > 3) {
            dns_state = DNS_STATE_FAIL;
            return;
        }
        dns_send_query();
    }
}

/* ===== TCP (single client connection, polled) ===== */

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT_1  3
#define TCP_FIN_WAIT_2  4
#define TCP_LAST_ACK    5

#define TCP_TX_MAX 1024

static int      tcp_state = TCP_CLOSED;
static uint8_t  tcp_remote_ip[4];
static uint16_t tcp_remote_port = 0;
static uint16_t tcp_local_port = 0;
static uint32_t tcp_snd_una = 0;    /* oldest unacked seq */
static uint32_t tcp_snd_nxt = 0;
static uint32_t tcp_rcv_nxt = 0;
static int      tcp_peer_fin = 0;   /* peer sent FIN (all data received) */
static int      tcp_error = 0;      /* RST or timeout */
static uint32_t tcp_last_tx_tick = 0;
static int      tcp_retries = 0;

/* Pending outbound data (single in-flight segment, enough for a request) */
static uint8_t  tcp_tx_buf[TCP_TX_MAX];
static int      tcp_tx_len = 0;

/* Receive sink — owner points this at its buffer before connecting */
static uint8_t *tcp_rx_sink = 0;
static int      tcp_rx_cap = 0;
static int      tcp_rx_len = 0;
static int      tcp_rx_truncated = 0;

static void tcp_emit(uint8_t flags, const uint8_t *payload, int payload_len) {
    uint8_t *seg = net_tx_frame + ETH_HEADER_SIZE + 20;
    struct tcp_header *tcp = (struct tcp_header *)seg;
    int hdr_len = 20;

    tcp->src_port = htons(tcp_local_port);
    tcp->dst_port = htons(tcp_remote_port);
    tcp->seq      = htonl(tcp_snd_una);
    tcp->ack      = htonl((flags & TCP_FLAG_ACK) ? tcp_rcv_nxt : 0);
    tcp->window   = htons(16384);
    tcp->checksum = 0;
    tcp->urgent   = 0;

    if (flags & TCP_FLAG_SYN) {
        /* MSS option: 1460 */
        seg[20] = 2; seg[21] = 4; seg[22] = 0x05; seg[23] = 0xB4;
        hdr_len = 24;
    }
    tcp->data_off = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags    = flags;

    for (int i = 0; i < payload_len; i++)
        seg[hdr_len + i] = payload[i];

    int seg_len = hdr_len + payload_len;
    tcp->checksum = htons(l4_checksum(net_our_ip, tcp_remote_ip, IP_PROTO_TCP,
                                      seg, seg_len));
    ipv4_send(tcp_remote_ip, IP_PROTO_TCP, (uint16_t)seg_len);
    tcp_last_tx_tick = net_ticks;
}

/* Pure ACK (does not consume sequence space, sent from snd_nxt) */
static void tcp_emit_ack(void) {
    uint32_t saved = tcp_snd_una;
    tcp_snd_una = tcp_snd_nxt;
    tcp_emit(TCP_FLAG_ACK, 0, 0);
    tcp_snd_una = saved;
}

static void tcp_open(const uint8_t *dst_ip, uint16_t dst_port,
                     uint8_t *rx_sink, int rx_cap) {
    for (int i = 0; i < 4; i++) tcp_remote_ip[i] = dst_ip[i];
    tcp_remote_port = dst_port;
    tcp_local_port  = (uint16_t)(0xC000 | ((net_ticks * 7919) & 0x3FFF));
    tcp_snd_una = (net_ticks * 2654435761u) ^ 0x53435254u;   /* ISS */
    tcp_snd_nxt = tcp_snd_una + 1;
    tcp_rcv_nxt = 0;
    tcp_peer_fin = 0;
    tcp_error = 0;
    tcp_retries = 0;
    tcp_tx_len = 0;
    tcp_rx_sink = rx_sink;
    tcp_rx_cap = rx_cap;
    tcp_rx_len = 0;
    tcp_rx_truncated = 0;

    tcp_state = TCP_SYN_SENT;
    tcp_emit(TCP_FLAG_SYN, 0, 0);
}

/* Queue request data (only valid once ESTABLISHED, single segment) */
static void tcp_send_data(const uint8_t *data, int len) {
    if (len > TCP_TX_MAX) len = TCP_TX_MAX;
    for (int i = 0; i < len; i++) tcp_tx_buf[i] = data[i];
    tcp_tx_len = len;
    tcp_retries = 0;
    tcp_emit(TCP_FLAG_PSH | TCP_FLAG_ACK, tcp_tx_buf, len);
    tcp_snd_nxt = tcp_snd_una + (uint32_t)len;
}

static void tcp_start_close(void) {
    if (tcp_state == TCP_ESTABLISHED) {
        tcp_emit(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        tcp_snd_nxt = tcp_snd_una + 1;
        tcp_state = tcp_peer_fin ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
    } else {
        tcp_state = TCP_CLOSED;
    }
}

static void tcp_abort(void) {
    if (tcp_state != TCP_CLOSED) {
        tcp_emit(TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0);
        tcp_state = TCP_CLOSED;
    }
}

static void net_handle_tcp(const struct ipv4_header *ip,
                           const uint8_t *seg, uint16_t seg_len) {
    if (tcp_state == TCP_CLOSED) return;
    if (seg_len < 20) return;

    const struct tcp_header *tcp = (const struct tcp_header *)seg;
    if (ntohs(tcp->dst_port) != tcp_local_port) return;
    if (ntohs(tcp->src_port) != tcp_remote_port) return;
    for (int i = 0; i < 4; i++)
        if (ip->src[i] != tcp_remote_ip[i]) return;

    uint8_t  flags = tcp->flags;
    uint32_t seq   = ntohl(tcp->seq);
    uint32_t ack   = ntohl(tcp->ack);
    int      hlen  = (tcp->data_off >> 4) * 4;
    if (hlen < 20 || hlen > seg_len) return;
    const uint8_t *payload = seg + hlen;
    int payload_len = seg_len - hlen;

    if (flags & TCP_FLAG_RST) {
        tcp_error = 1;
        tcp_state = TCP_CLOSED;
        return;
    }

    if (tcp_state == TCP_SYN_SENT) {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) &&
            ack == tcp_snd_nxt) {
            tcp_rcv_nxt = seq + 1;
            tcp_snd_una = tcp_snd_nxt;
            tcp_emit_ack();
            tcp_state = TCP_ESTABLISHED;
        }
        return;
    }

    /* ACK processing: advance snd_una */
    if (flags & TCP_FLAG_ACK) {
        int32_t adv = (int32_t)(ack - tcp_snd_una);
        if (adv > 0) {
            tcp_snd_una = ack;
            if (tcp_snd_una == tcp_snd_nxt) {
                tcp_tx_len = 0;         /* fully acked */
                tcp_retries = 0;
            }
        }
        if (tcp_state == TCP_FIN_WAIT_1 && tcp_snd_una == tcp_snd_nxt)
            tcp_state = TCP_FIN_WAIT_2;
        else if (tcp_state == TCP_LAST_ACK && tcp_snd_una == tcp_snd_nxt) {
            tcp_state = TCP_CLOSED;
            return;
        }
    }

    /* In-order data */
    if (seq == tcp_rcv_nxt) {
        if (payload_len > 0) {
            if (tcp_rx_sink) {
                int room = tcp_rx_cap - tcp_rx_len;
                int n = payload_len < room ? payload_len : room;
                for (int i = 0; i < n; i++)
                    tcp_rx_sink[tcp_rx_len + i] = payload[i];
                tcp_rx_len += n;
                if (n < payload_len) tcp_rx_truncated = 1;
            }
            tcp_rcv_nxt += (uint32_t)payload_len;
        }
        if (flags & TCP_FLAG_FIN) {
            tcp_rcv_nxt++;
            tcp_peer_fin = 1;
            if (tcp_state == TCP_FIN_WAIT_1 || tcp_state == TCP_FIN_WAIT_2) {
                tcp_emit_ack();
                tcp_state = TCP_CLOSED;
                return;
            }
        }
        if (payload_len > 0 || (flags & TCP_FLAG_FIN))
            tcp_emit_ack();
    } else if (payload_len > 0 || (flags & TCP_FLAG_FIN)) {
        /* Out of order — dup-ACK what we expect */
        tcp_emit_ack();
    }
}

static void tcp_tick(void) {
    if (tcp_state == TCP_CLOSED) return;

    /* Retransmission after ~1 s of silence with unacked data */
    int unacked = (tcp_snd_una != tcp_snd_nxt);
    if (unacked && net_ticks - tcp_last_tx_tick > 60) {
        if (++tcp_retries > 8) {
            tcp_error = 1;
            tcp_state = TCP_CLOSED;
            return;
        }
        if (tcp_state == TCP_SYN_SENT) {
            uint32_t iss = tcp_snd_nxt - 1;
            tcp_snd_una = iss;
            tcp_emit(TCP_FLAG_SYN, 0, 0);
        } else if (tcp_tx_len > 0) {
            tcp_emit(TCP_FLAG_PSH | TCP_FLAG_ACK, tcp_tx_buf, tcp_tx_len);
        } else {
            /* Unacked FIN */
            tcp_emit(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        }
    }
}

/* ===== HTTP/1.0 CLIENT (async state machine) ===== */

#define HTTP_IDLE       0
#define HTTP_RESOLVING  1
#define HTTP_CONNECTING 2
#define HTTP_REQUESTING 3
#define HTTP_RECEIVING  4
#define HTTP_DONE       5
#define HTTP_ERROR      6

#define HTTP_BUF_MAX  65536

/* There is one TCP connection, so whoever starts a transfer claims it and
 * the other pollers leave the response alone.  Callers set http_owner
 * immediately after http_get(). */
#define HTTP_OWNER_NONE    0
#define HTTP_OWNER_BROWSER 1
#define HTTP_OWNER_TERM    2
#define HTTP_OWNER_STORE   3

static int      http_owner = HTTP_OWNER_NONE;

static int      http_state = HTTP_IDLE;
static char     http_host[128];
static char     http_path[256];
static uint16_t http_port = 80;
static uint8_t  http_server_ip[4];
static uint8_t  http_raw[HTTP_BUF_MAX];
static int      http_status_code = 0;
static const uint8_t *http_body = 0;
static int      http_body_len = 0;
static char     http_err[64];
static int      http_redirects = 0;
static uint32_t http_start_tick = 0;

static void http_set_err(const char *msg) {
    int i = 0;
    while (msg[i] && i < 63) { http_err[i] = msg[i]; i++; }
    http_err[i] = '\0';
    http_state = HTTP_ERROR;
    tcp_abort();
}

static void http_begin(const char *host, uint16_t port, const char *path) {
    int i = 0;
    while (host[i] && i < 127) { http_host[i] = host[i]; i++; }
    http_host[i] = '\0';
    i = 0;
    while (path[i] && i < 255) { http_path[i] = path[i]; i++; }
    http_path[i] = '\0';
    if (http_path[0] == '\0') { http_path[0] = '/'; http_path[1] = '\0'; }
    http_port = port;

    http_status_code = 0;
    http_body = 0;
    http_body_len = 0;
    http_err[0] = '\0';
    http_start_tick = net_ticks;

    if (!e1000_found) { http_set_err("no network adapter"); return; }

    tcp_abort();                       /* drop any previous connection */
    http_state = HTTP_RESOLVING;
    dns_resolve_start(http_host);
}

/* Public entry: fresh request resets the redirect budget */
static void http_get(const char *host, uint16_t port, const char *path) {
    http_redirects = 0;
    http_begin(host, port, path);
}

static int http_find_header(const char *name, char *out, int out_max) {
    /* Case-insensitive scan of raw headers for "name:" */
    int hdr_end = -1;
    for (int i = 0; i + 3 < tcp_rx_len; i++) {
        if (http_raw[i] == '\r' && http_raw[i+1] == '\n' &&
            http_raw[i+2] == '\r' && http_raw[i+3] == '\n') {
            hdr_end = i;
            break;
        }
    }
    if (hdr_end < 0) hdr_end = tcp_rx_len;

    int nlen = net_strlen(name);
    for (int i = 0; i < hdr_end - nlen - 1; i++) {
        if (i > 0 && http_raw[i-1] != '\n') continue;
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char a = (char)http_raw[i + j];
            char b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { match = 0; break; }
        }
        if (!match || http_raw[i + nlen] != ':') continue;
        int p = i + nlen + 1;
        while (p < hdr_end && (http_raw[p] == ' ' || http_raw[p] == '\t')) p++;
        int o = 0;
        while (p < hdr_end && http_raw[p] != '\r' && http_raw[p] != '\n' &&
               o < out_max - 1)
            out[o++] = (char)http_raw[p++];
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* Parse "http://host[:port]/path" (also accepts bare host/path).
 * Returns 0 for unsupported schemes (e.g. https). */
static int http_parse_url(const char *url, char *host, int host_max,
                          uint16_t *port, char *path, int path_max) {
    const char *p = url;

    /* scheme */
    int has_scheme = 0;
    for (int i = 0; url[i]; i++) {
        if (url[i] == ':' && url[i+1] == '/' && url[i+2] == '/') {
            has_scheme = 1;
            break;
        }
        if (url[i] == '/' || url[i] == '.') break;
    }
    if (has_scheme) {
        if (!(p[0]=='h' && p[1]=='t' && p[2]=='t' && p[3]=='p' &&
              p[4]==':' && p[5]=='/' && p[6]=='/'))
            return 0;
        p += 7;
    }

    int hi = 0;
    *port = 80;
    while (*p && *p != '/' && *p != ':' && hi < host_max - 1)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0) return 0;

    if (*p == ':') {
        p++;
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v > 0 && v < 65536) *port = (uint16_t)v;
    }

    int pi = 0;
    if (*p == '\0') {
        path[pi++] = '/';
    } else {
        while (*p && pi < path_max - 1) path[pi++] = *p++;
    }
    path[pi] = '\0';
    return 1;
}

static void http_finish_response(void) {
    /* Parse status line: HTTP/1.x NNN */
    if (tcp_rx_len < 12) { http_set_err("empty response"); return; }

    http_status_code = 0;
    int sp = 0;
    while (sp < tcp_rx_len && http_raw[sp] != ' ') sp++;
    sp++;
    for (int i = 0; i < 3 && sp + i < tcp_rx_len; i++) {
        uint8_t c = http_raw[sp + i];
        if (c < '0' || c > '9') break;
        http_status_code = http_status_code * 10 + (c - '0');
    }

    /* Redirect */
    if ((http_status_code == 301 || http_status_code == 302 ||
         http_status_code == 303 || http_status_code == 307) &&
        http_redirects < 4) {
        char loc[256];
        if (http_find_header("Location", loc, sizeof(loc))) {
            char host[128], path[256];
            uint16_t port;
            if (loc[0] == '/') {
                /* relative redirect: same host */
                http_redirects++;
                http_begin(http_host, http_port, loc);
                return;
            }
            if (http_parse_url(loc, host, sizeof(host), &port,
                               path, sizeof(path))) {
                http_redirects++;
                http_begin(host, port, path);
                return;
            }
            http_set_err("redirect to https not supported");
            return;
        }
    }

    /* Locate body */
    http_body = 0;
    http_body_len = 0;
    for (int i = 0; i + 3 < tcp_rx_len; i++) {
        if (http_raw[i] == '\r' && http_raw[i+1] == '\n' &&
            http_raw[i+2] == '\r' && http_raw[i+3] == '\n') {
            http_body = http_raw + i + 4;
            http_body_len = tcp_rx_len - i - 4;
            break;
        }
    }
    if (!http_body) {
        http_body = http_raw;
        http_body_len = tcp_rx_len;
    }
    http_state = HTTP_DONE;
}

static void http_tick(void) {
    switch (http_state) {
    case HTTP_RESOLVING:
        if (dns_state == DNS_STATE_DONE) {
            for (int i = 0; i < 4; i++) http_server_ip[i] = dns_result[i];
            tcp_open(http_server_ip, http_port, http_raw, HTTP_BUF_MAX);
            http_state = HTTP_CONNECTING;
        } else if (dns_state == DNS_STATE_FAIL) {
            http_set_err("host not found");
        }
        break;

    case HTTP_CONNECTING:
        if (tcp_state == TCP_ESTABLISHED) {
            char req[512];
            int p = 0;
            const char *parts[7];
            parts[0] = "GET ";
            parts[1] = http_path;
            parts[2] = " HTTP/1.0\r\nHost: ";
            parts[3] = http_host;
            parts[4] = "\r\nUser-Agent: Vextro/9.0\r\nAccept: text/html, text/plain\r\n";
            parts[5] = "Connection: close\r\n\r\n";
            parts[6] = 0;
            for (int i = 0; parts[i]; i++)
                for (int j = 0; parts[i][j] && p < 511; j++)
                    req[p++] = parts[i][j];
            tcp_send_data((const uint8_t *)req, p);
            http_state = HTTP_REQUESTING;
        } else if (tcp_error || tcp_state == TCP_CLOSED) {
            http_set_err("connection refused");
        } else if (net_ticks - http_start_tick > 600) {
            http_set_err("connection timed out");
        }
        break;

    case HTTP_REQUESTING:
    case HTTP_RECEIVING:
        if (tcp_error) {
            if (tcp_rx_len > 0) http_finish_response();
            else http_set_err("connection reset");
            break;
        }
        if (tcp_rx_len > 0)
            http_state = HTTP_RECEIVING;
        if (tcp_peer_fin) {
            tcp_start_close();
            http_finish_response();
            break;
        }
        if (tcp_state == TCP_CLOSED) {
            if (tcp_rx_len > 0) http_finish_response();
            else http_set_err("connection closed early");
            break;
        }
        if (net_ticks - http_start_tick > 1800)   /* 30 s hard cap */
            http_set_err("request timed out");
        break;

    default:
        break;
    }
}

/* ===== IPv4 RX DISPATCH ===== */

static void net_handle_ipv4(const uint8_t *data, uint16_t len) {
    if (len < 20) return;
    const struct ipv4_header *ip = (const struct ipv4_header *)data;
    if ((ip->ver_ihl >> 4) != 4) return;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;

    /* Ignore fragments (offset != 0 or MF set) */
    uint16_t ff = ntohs(ip->flags_frag);
    if ((ff & 0x2000) || (ff & 0x1FFF)) return;

    /* Only unicast to us (slirp doesn't broadcast anything we need) */
    int for_us = 1;
    for (int i = 0; i < 4; i++)
        if (ip->dst[i] != net_our_ip[i]) { for_us = 0; break; }
    if (!for_us) return;

    uint16_t total = ntohs(ip->total_len);
    if (total > len) total = len;
    const uint8_t *payload = data + ihl;
    uint16_t payload_len = (uint16_t)(total - ihl);

    switch (ip->proto) {
    case IP_PROTO_ICMP:
        net_handle_icmp(ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        net_handle_tcp(ip, payload, payload_len);
        break;
    case IP_PROTO_UDP: {
        if (payload_len < sizeof(struct udp_header)) break;
        const struct udp_header *udp = (const struct udp_header *)payload;
        uint16_t dport = ntohs(udp->dst_port);
        uint16_t sport = ntohs(udp->src_port);
        uint16_t ulen  = ntohs(udp->length);
        if (ulen < sizeof(struct udp_header) || ulen > payload_len) break;
        if (sport == 53 && dport == dns_port)
            dns_handle_reply(payload + sizeof(struct udp_header),
                             (uint16_t)(ulen - sizeof(struct udp_header)));
        break;
    }
    default:
        break;
    }
}

/* ===== ARP RX ===== */

static void net_handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct arp_packet)) return;

    const struct arp_packet *arp = (const struct arp_packet *)data;

    if (ntohs(arp->hw_type)    != ARP_HW_ETHERNET) return;
    if (ntohs(arp->proto_type) != ARP_PROTO_IPV4)   return;
    if (arp->hw_len != 6 || arp->proto_len != 4)    return;

    arp_cache_update(ip_u32(arp->sender_ip), arp->sender_mac);

    uint16_t opcode = ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST) {
        if (arp->target_ip[0] == net_our_ip[0] &&
            arp->target_ip[1] == net_our_ip[1] &&
            arp->target_ip[2] == net_our_ip[2] &&
            arp->target_ip[3] == net_our_ip[3]) {
            arp_send_reply(arp->sender_mac, arp->sender_ip,
                           e1000_mac, net_our_ip);
        }
    }
}

/* ===== ETHERNET FRAME DISPATCHER ===== */

static void net_handle_ethernet(const uint8_t *frame, uint16_t len) {
    if (len < ETH_HEADER_SIZE) return;

    const struct eth_header *eth = (const struct eth_header *)frame;

    if (!mac_match(eth->dst_mac, e1000_mac) &&
        !mac_match(eth->dst_mac, BROADCAST_MAC))
        return;

    uint16_t ethertype = ntohs(eth->ethertype);
    const uint8_t *payload    = frame + ETH_HEADER_SIZE;
    uint16_t       payload_len = (uint16_t)(len - ETH_HEADER_SIZE);

    switch (ethertype) {
    case ETHERTYPE_ARP:  net_handle_arp(payload, payload_len);  break;
    case ETHERTYPE_IPV4: net_handle_ipv4(payload, payload_len); break;
    default: break;
    }
}

/* ===== POLL — one call per main-loop frame ===== */

static void net_poll(void) {
    if (!e1000_found) return;

    net_ticks++;

    uint8_t  *buf;
    uint16_t  len;
    while (e1000_rx_poll(&buf, &len))
        net_handle_ethernet(buf, len);

    dns_tick();
    tcp_tick();
    http_tick();
}

/* ===== INITIALIZATION ===== */

static void netstack_init(void) {
    if (!e1000_found) {
        net_log("No NIC found - network stack disabled");
        return;
    }

    e1000_read_mac();

    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;

    /* Warm the ARP cache for gateway + DNS so first request is fast */
    arp_send_request(net_gw_ip);

    net_log("Network stack initialized (IPv4/ICMP/UDP/DNS/TCP/HTTP)");

    static const char hex[] = "0123456789ABCDEF";
    serial_puts("[NET] MAC: ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) serial_putc(':');
        serial_putc(hex[(e1000_mac[i] >> 4) & 0xF]);
        serial_putc(hex[e1000_mac[i] & 0xF]);
    }
    serial_putc('\n');

    serial_puts("[NET] IP:  ");
    for (int i = 0; i < 4; i++) {
        if (i > 0) serial_putc('.');
        serial_put_dec(net_our_ip[i]);
    }
    serial_putc('\n');
}

#endif /* NETSTACK_H */
