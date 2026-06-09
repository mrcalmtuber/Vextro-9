#ifndef NETSTACK_H
#define NETSTACK_H

#include <stdint.h>

/* ===== SERIAL FORMATTING HELPERS ===== */

static void net_log(const char *msg) {
    serial_puts("[NET] ");
    serial_puts(msg);
    serial_putc('\n');
}

static void serial_put_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    serial_putc('0'); serial_putc('x');
    serial_putc(hex[(val >> 12) & 0xF]);
    serial_putc(hex[(val >> 8)  & 0xF]);
    serial_putc(hex[(val >> 4)  & 0xF]);
    serial_putc(hex[val & 0xF]);
}

static void serial_put_dec(uint16_t val) {
    char buf[6];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) { buf[i++] = (char)('0' + (val % 10)); val /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}

/* ===== BYTE ORDER ===== */

static inline uint16_t ntohs(uint16_t net) {
    return (uint16_t)((net >> 8) | (net << 8));
}

static inline uint16_t htons(uint16_t host) {
    return (uint16_t)((host >> 8) | (host << 8));
}

/* ===== ETHERNET FRAME ===== */

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

/* ===== ARP PROTOCOL ===== */

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

/* ===== ARP CACHE ===== */

#define ARP_CACHE_SIZE 16

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

/* ===== NETWORK CONFIGURATION ===== */

static uint8_t net_our_ip[4] = { 10, 0, 2, 15 };

/* ===== MAC COMPARISON ===== */

static int mac_match(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* ===== ARP CACHE OPERATIONS ===== */

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

__attribute__((unused))
static int arp_cache_lookup(uint32_t ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) out_mac[j] = arp_cache[i].mac[j];
            return 1;
        }
    }
    return 0;
}

/* ===== ARP REPLY TRANSMISSION ===== */

static void arp_send_reply(const uint8_t *dst_mac, const uint8_t *dst_ip,
                           const uint8_t *src_mac, const uint8_t *src_ip) {
    uint8_t frame[ETH_HEADER_SIZE + sizeof(struct arp_packet)];

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

    e1000_transmit(frame, (uint16_t)sizeof(frame));
    net_log("ARP Reply transmitted");
}

/* ===== PACKET HANDLERS ===== */

static void net_handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(struct arp_packet)) return;

    const struct arp_packet *arp = (const struct arp_packet *)data;

    if (ntohs(arp->hw_type)    != ARP_HW_ETHERNET) return;
    if (ntohs(arp->proto_type) != ARP_PROTO_IPV4)   return;
    if (arp->hw_len != 6 || arp->proto_len != 4)    return;

    uint32_t sender_ip;
    for (int i = 0; i < 4; i++)
        ((uint8_t *)&sender_ip)[i] = arp->sender_ip[i];
    arp_cache_update(sender_ip, arp->sender_mac);

    uint16_t opcode = ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST) {
        if (arp->target_ip[0] == net_our_ip[0] &&
            arp->target_ip[1] == net_our_ip[1] &&
            arp->target_ip[2] == net_our_ip[2] &&
            arp->target_ip[3] == net_our_ip[3]) {
            net_log("ARP Request for our IP - sending reply");
            arp_send_reply(arp->sender_mac, arp->sender_ip,
                           e1000_mac, net_our_ip);
        }
    } else if (opcode == ARP_OP_REPLY) {
        net_log("ARP Reply received - cache updated");
    }
}

static void net_handle_ipv4(const uint8_t *data, uint16_t len) {
    (void)data;
    (void)len;
}

/* ===== ETHERNET FRAME DISPATCHER ===== */

static void net_handle_ethernet(const uint8_t *frame, uint16_t len) {
    if (len < ETH_HEADER_SIZE) return;

    const struct eth_header *eth = (const struct eth_header *)frame;

    if (!mac_match(eth->dst_mac, e1000_mac) &&
        !mac_match(eth->dst_mac, BROADCAST_MAC))
        return;

    uint16_t ethertype = ntohs(eth->ethertype);

    serial_puts("[NET] Recv Frame - Type: ");
    serial_put_hex16(ethertype);
    serial_puts(", Length: ");
    serial_put_dec(len);
    serial_puts(" bytes\n");

    const uint8_t *payload    = frame + ETH_HEADER_SIZE;
    uint16_t       payload_len = len - ETH_HEADER_SIZE;

    switch (ethertype) {
    case ETHERTYPE_ARP:  net_handle_arp(payload, payload_len);  break;
    case ETHERTYPE_IPV4: net_handle_ipv4(payload, payload_len); break;
    default: break;
    }
}

/* ===== RX POLL — called from main loop ===== */

static void net_poll(void) {
    if (!e1000_found) return;

    uint8_t  *buf;
    uint16_t  len;
    while (e1000_rx_poll(&buf, &len))
        net_handle_ethernet(buf, len);
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

    net_log("Network stack initialized");

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

    net_log("Ethernet frame parser active");
    net_log("ARP responder active");
}

#endif /* NETSTACK_H */
