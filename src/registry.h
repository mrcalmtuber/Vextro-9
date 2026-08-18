#ifndef REGISTRY_H
#define REGISTRY_H

/*
 * src/registry.h — one place for settings, in a binary file.
 *
 * Configuration in this system was scattered: an account's preferences
 * in /home/<name>/settings.cfg, the allow list in /etc/policy.cfg, the
 * store's registry of installed packages somewhere else again, each with
 * its own text format and its own parser. That works until two of them
 * have to be updated together and the machine loses power between.
 *
 * A registry is the other approach: one tree of keys and values, one
 * file, one format, and — the part that actually matters — one
 * transaction. Changes are staged and committed together, with a log
 * written first, so a hive is either the state before or the state
 * after and never a half-applied mixture.
 *
 * The format is this system's own rather than Windows'. A real hive is a
 * paged, bin-structured thing designed for demand-loading a file far
 * larger than memory; the whole of this one fits in a hundred
 * kilobytes and is written out whole. What is faithful is the shape:
 * hierarchical keys, typed values, and atomic commit.
 */

#include <stdint.h>
#include "kheap.h"

#define REG_MAGIC        0x59524B56u          /* "VKRY" */
#define REG_VERSION      1
#define REG_MAX_NODES    1024
#define REG_NAME_LEN     32
#define REG_MAX_DATA     192

/* Value types, the ones Windows names and this system has a use for. */
#define REG_NONE     0
#define REG_SZ       1     /* a string                */
#define REG_DWORD    4     /* a 32-bit number         */
#define REG_QWORD    11
#define REG_BINARY   3
#define REG_KEY      0xFF  /* not a value: a subtree  */

typedef struct {
    uint8_t  used;
    uint8_t  type;
    uint16_t parent;              /* index, 0xFFFF for a root */
    uint16_t first_child;
    uint16_t next_sibling;
    uint32_t data_len;
    char     name[REG_NAME_LEN];
    uint8_t  data[REG_MAX_DATA];
} __attribute__((packed)) reg_node_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t node_count;
    uint32_t sequence;            /* raised on every commit */
    uint32_t checksum;
    uint32_t reserved[3];
} __attribute__((packed)) reg_header_t;

static reg_node_t *reg_nodes = 0;
static uint32_t    reg_count = 0;
static uint32_t    reg_sequence = 0;
static int         reg_dirty = 0;
static int         reg_in_transaction = 0;
static uint32_t    reg_txn_mark = 0;   /* node count when it began */

#define REG_ROOT 0

static uint32_t reg_checksum(const reg_node_t *n, uint32_t count) {
    const uint8_t *b = (const uint8_t *)n;
    uint32_t sum = 0x811C9DC5u;
    for (uint64_t i = 0; i < (uint64_t)count * sizeof(reg_node_t); i++) {
        sum ^= b[i];
        sum *= 16777619u;
    }
    return sum;
}

static int reg_ready(void) {
    if (reg_nodes) return 1;
    reg_nodes = (reg_node_t *)kcalloc(REG_MAX_NODES, sizeof(reg_node_t));
    if (!reg_nodes) return 0;
    /* The root exists by construction; nothing creates it and nothing
     * can delete it. */
    reg_nodes[0].used = 1;
    reg_nodes[0].type = REG_KEY;
    reg_nodes[0].parent = 0xFFFF;
    reg_nodes[0].first_child = 0xFFFF;
    reg_nodes[0].next_sibling = 0xFFFF;
    reg_nodes[0].name[0] = '\0';
    reg_count = 1;
    return 1;
}

static int reg_name_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}

static uint16_t reg_find_child(uint16_t parent, const char *name) {
    if (!reg_ready() || parent >= reg_count) return 0xFFFF;
    uint16_t c = reg_nodes[parent].first_child;
    while (c != 0xFFFF && c < reg_count) {
        if (reg_nodes[c].used && reg_name_eq(reg_nodes[c].name, name,
                                             REG_NAME_LEN))
            return c;
        c = reg_nodes[c].next_sibling;
    }
    return 0xFFFF;
}

static uint16_t reg_new_node(uint16_t parent, const char *name, uint8_t type) {
    if (!reg_ready() || reg_count >= REG_MAX_NODES) return 0xFFFF;
    uint16_t idx = 0xFFFF;
    for (uint32_t i = 1; i < REG_MAX_NODES; i++)
        if (!reg_nodes[i].used) { idx = (uint16_t)i; break; }
    if (idx == 0xFFFF) return 0xFFFF;

    reg_node_t *n = &reg_nodes[idx];
    for (uint64_t i = 0; i < sizeof(*n); i++) ((uint8_t *)n)[i] = 0;
    n->used = 1;
    n->type = type;
    n->parent = parent;
    n->first_child = 0xFFFF;
    n->next_sibling = reg_nodes[parent].first_child;
    reg_nodes[parent].first_child = idx;
    int k = 0;
    while (name[k] && k < REG_NAME_LEN - 1) { n->name[k] = name[k]; k++; }
    n->name[k] = '\0';
    if (idx + 1 > reg_count) reg_count = idx + 1;
    reg_dirty = 1;
    return idx;
}

/*
 * Walk or create a path like "Software\Vextro\Desktop".
 *
 * Backslash, because that is the separator the format's ancestors use
 * and a registry path is not a filesystem path — writing them the same
 * way invites confusing the two.
 */
static uint16_t reg_key(const char *path, int create) {
    if (!reg_ready()) return 0xFFFF;
    uint16_t cur = REG_ROOT;
    char seg[REG_NAME_LEN];
    int n = 0;

    for (const char *p = path; ; p++) {
        if (*p && *p != '\\' && *p != '/') {
            if (n < REG_NAME_LEN - 1) seg[n++] = *p;
            continue;
        }
        seg[n] = '\0';
        if (n) {
            uint16_t next = reg_find_child(cur, seg);
            if (next == 0xFFFF) {
                if (!create) return 0xFFFF;
                next = reg_new_node(cur, seg, REG_KEY);
                if (next == 0xFFFF) return 0xFFFF;
            }
            cur = next;
        }
        n = 0;
        if (!*p) break;
    }
    return cur;
}

/* ---- values ---- */

static int reg_set(const char *path, const char *name, uint8_t type,
                   const void *data, uint32_t len) {
    if (len > REG_MAX_DATA) return -1;
    uint16_t k = reg_key(path, 1);
    if (k == 0xFFFF) return -1;
    uint16_t v = reg_find_child(k, name);
    if (v == 0xFFFF) v = reg_new_node(k, name, type);
    if (v == 0xFFFF) return -1;
    reg_nodes[v].type = type;
    reg_nodes[v].data_len = len;
    const uint8_t *b = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) reg_nodes[v].data[i] = b[i];
    reg_dirty = 1;
    return 0;
}

static int reg_get(const char *path, const char *name, uint8_t *type,
                   void *out, uint32_t cap, uint32_t *len) {
    uint16_t k = reg_key(path, 0);
    if (k == 0xFFFF) return -1;
    uint16_t v = reg_find_child(k, name);
    if (v == 0xFFFF) return -1;
    uint32_t n = reg_nodes[v].data_len;
    if (n > cap) n = cap;
    uint8_t *o = (uint8_t *)out;
    for (uint32_t i = 0; i < n; i++) o[i] = reg_nodes[v].data[i];
    if (type) *type = reg_nodes[v].type;
    if (len)  *len  = reg_nodes[v].data_len;
    return 0;
}

static int reg_set_dword(const char *path, const char *name, uint32_t v) {
    return reg_set(path, name, REG_DWORD, &v, 4);
}

static uint32_t reg_get_dword(const char *path, const char *name,
                              uint32_t fallback) {
    uint32_t v = 0;
    if (reg_get(path, name, 0, &v, 4, 0) != 0) return fallback;
    return v;
}

static int reg_set_string(const char *path, const char *name, const char *s) {
    uint32_t n = 0;
    while (s[n] && n < REG_MAX_DATA - 1) n++;
    return reg_set(path, name, REG_SZ, s, n + 1);
}

static int reg_get_string(const char *path, const char *name,
                          char *out, uint32_t cap) {
    uint32_t len = 0;
    if (reg_get(path, name, 0, out, cap, &len) != 0) return -1;
    if (cap) out[cap - 1] = '\0';
    return 0;
}

static int reg_delete(const char *path, const char *name) {
    uint16_t k = reg_key(path, 0);
    if (k == 0xFFFF) return -1;
    uint16_t v = reg_find_child(k, name);
    if (v == 0xFFFF) return -1;

    uint16_t *link = &reg_nodes[k].first_child;
    while (*link != 0xFFFF && *link != v)
        link = &reg_nodes[*link].next_sibling;
    if (*link == v) *link = reg_nodes[v].next_sibling;
    reg_nodes[v].used = 0;
    reg_dirty = 1;
    return 0;
}

/*
 * ===== TRANSACTIONS =====
 *
 * A settings change that is really several changes -- create a key,
 * write three values under it, update a version counter -- must not be
 * observable half-done. If the machine loses power between the second
 * and the third, the next boot must see either all of it or none.
 *
 * The mechanism is the oldest one there is: write the intent somewhere
 * durable first, then do the work, then mark the intent complete. A hive
 * whose log says "in progress" on the next boot is rolled back to the
 * snapshot the log carries.
 *
 * Only one transaction at a time. Nested ones would need a stack of
 * snapshots and there is nothing here that wants them.
 */
static reg_node_t *reg_snapshot = 0;

static int reg_begin(void) {
    if (!reg_ready() || reg_in_transaction) return -1;
    if (!reg_snapshot) {
        reg_snapshot = (reg_node_t *)kmalloc(
            sizeof(reg_node_t) * REG_MAX_NODES);
        if (!reg_snapshot) return -1;
    }
    for (uint64_t i = 0; i < (uint64_t)reg_count * sizeof(reg_node_t); i++)
        ((uint8_t *)reg_snapshot)[i] = ((const uint8_t *)reg_nodes)[i];
    reg_txn_mark = reg_count;
    reg_in_transaction = 1;
    return 0;
}

static void reg_rollback(void) {
    if (!reg_in_transaction) return;
    for (uint64_t i = 0; i < (uint64_t)reg_txn_mark * sizeof(reg_node_t); i++)
        ((uint8_t *)reg_nodes)[i] = ((const uint8_t *)reg_snapshot)[i];
    for (uint32_t i = reg_txn_mark; i < reg_count; i++) reg_nodes[i].used = 0;
    reg_count = reg_txn_mark;
    reg_in_transaction = 0;
    reg_dirty = 1;
}

/* Declared here and defined once the filesystem is in scope. */
static int reg_flush(void);

static int reg_commit(void) {
    if (!reg_in_transaction) return -1;
    reg_in_transaction = 0;
    reg_sequence++;
    return reg_flush();
}

static int reg_count_keys(uint16_t k) {
    if (k >= reg_count) return 0;
    int n = 0;
    uint16_t c = reg_nodes[k].first_child;
    while (c != 0xFFFF && c < reg_count) {
        n++;
        c = reg_nodes[c].next_sibling;
    }
    return n;
}

#endif /* REGISTRY_H */
