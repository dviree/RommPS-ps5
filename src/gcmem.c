#include "gcmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_DATA 5
#define NENTRIES 127

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* Dolphin's two checksums over big-endian words; 0xFFFF counts as 0. */
static void checksums(const unsigned char *p, size_t n, unsigned *c1, unsigned *c2) {
    unsigned a = 0, b = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        unsigned w = be16(p + i);
        a = (a + w) & 0xFFFF;
        b = (b + (w ^ 0xFFFF)) & 0xFFFF;
    }
    *c1 = a == 0xFFFF ? 0 : a;
    *c2 = b == 0xFFFF ? 0 : b;
}

static unsigned char *block(const gc_card *c, int n) { return c->data + (size_t)n * GC_BLOCK; }

/* Directory: checksums over 0..0x1FFB, at 0x1FFC; update counter at 0x1FFA. */
static int dir_ok(const unsigned char *d) {
    unsigned a, b;
    checksums(d, 0x1FFC, &a, &b);
    return a == be16(d + 0x1FFC) && b == be16(d + 0x1FFE);
}
/* Table: checksums over 0x4..0x1FFF, at 0x0; update counter at 0x4. */
static int bat_ok(const unsigned char *t) {
    unsigned a, b;
    checksums(t + 4, GC_BLOCK - 4, &a, &b);
    return a == be16(t) && b == be16(t + 2);
}

/* The current copy: the one that checks out, or the newer of two that do. */
static int pick(const unsigned char *x, const unsigned char *y, int ok_x, int ok_y, size_t counter_at, int ix, int iy) {
    if (ok_x && ok_y) return (short)be16(y + counter_at) > (short)be16(x + counter_at) ? iy : ix;
    return ok_x ? ix : ok_y ? iy : -1;
}

int gc_card_open(gc_card *c, const unsigned char *data, size_t size) {
    memset(c, 0, sizeof *c);
    if (size < (size_t)GC_BLOCK * (FIRST_DATA + 1) || size % GC_BLOCK) return -1;
    c->data = malloc(size);
    if (!c->data) return -1;
    memcpy(c->data, data, size);
    c->size = size;
    c->nblocks = (int)(size / GC_BLOCK);
    if (c->nblocks > 0xFFB + FIRST_DATA) c->nblocks = 0xFFB + FIRST_DATA;
    c->dir = pick(block(c, 1), block(c, 2), dir_ok(block(c, 1)), dir_ok(block(c, 2)), 0x1FFA, 1, 2);
    c->bat = pick(block(c, 3), block(c, 4), bat_ok(block(c, 3)), bat_ok(block(c, 4)), 4, 3, 4);
    if (c->dir < 0 || c->bat < 0) {
        gc_card_free(c);
        return -1;
    }
    return 0;
}

void gc_card_free(gc_card *c) {
    free(c->data);
    memset(c, 0, sizeof *c);
}

static unsigned char *entry(const gc_card *c, int i) { return block(c, c->dir) + i * GC_DENTRY; }
static int empty(const unsigned char *e) { return e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF; }
static unsigned char *map_at(const gc_card *c, int blk) { return block(c, c->bat) + 0xA + (size_t)(blk - FIRST_DATA) * 2; }
static int is_data(const gc_card *c, unsigned blk) { return blk >= FIRST_DATA && blk < (unsigned)c->nblocks; }

int gc_gci_valid(const unsigned char *g, size_t size) {
    if (size < GC_DENTRY || (size - GC_DENTRY) % GC_BLOCK || empty(g)) return 0;
    unsigned count = be16(g + 0x38);
    return count > 0 && (size_t)count * GC_BLOCK == size - GC_DENTRY;
}

static int same_id(const unsigned char *e, const char *id) { return strlen(id) >= 6 && !memcmp(e, id, 6); }

int gc_gci_same_save(const unsigned char *a, const unsigned char *b) {
    return !memcmp(a, b, 6) && !memcmp(a + 8, b + 8, 32);
}

void gc_gci_id(const unsigned char *g, char out[7]) {
    memcpy(out, g, 6);
    out[6] = 0;
}

/* Copies an entry's blocks after it, following the table. */
static unsigned char *to_gci(const gc_card *c, const unsigned char *e, size_t *size) {
    unsigned count = be16(e + 0x38), blk = be16(e + 0x36);
    if (!count || count > (unsigned)c->nblocks) return NULL;
    unsigned char *g = malloc(GC_DENTRY + (size_t)count * GC_BLOCK);
    if (!g) return NULL;
    memcpy(g, e, GC_DENTRY);
    for (unsigned k = 0; k < count; k++) {
        if (!is_data(c, blk)) {
            free(g);
            return NULL;
        }
        memcpy(g + GC_DENTRY + (size_t)k * GC_BLOCK, block(c, (int)blk), GC_BLOCK);
        blk = be16(map_at(c, (int)blk));
    }
    *size = GC_DENTRY + (size_t)count * GC_BLOCK;
    return g;
}

int gc_card_saves(const gc_card *c, const char *id, gc_save_fn fn, void *ud) {
    int n = 0;
    for (int i = 0; i < NENTRIES; i++) {
        const unsigned char *e = entry(c, i);
        if (empty(e) || !same_id(e, id)) continue;
        size_t size = 0;
        unsigned char *g = to_gci(c, e, &size);
        if (!g) continue;
        if (fn) fn(g, size, ud);
        free(g);
        n++;
    }
    return n;
}

/* Edits go to a copy of the current directory and table, written over the
 * older copies when done. */
typedef struct {
    unsigned char dir[GC_BLOCK], bat[GC_BLOCK];
} draft;

static void draft_start(const gc_card *c, draft *d) {
    memcpy(d->dir, block(c, c->dir), GC_BLOCK);
    memcpy(d->bat, block(c, c->bat), GC_BLOCK);
}

static void draft_commit(gc_card *c, draft *d) {
    unsigned a, b;
    put16(d->dir + 0x1FFA, (be16(d->dir + 0x1FFA) + 1) & 0xFFFF);
    checksums(d->dir, 0x1FFC, &a, &b);
    put16(d->dir + 0x1FFC, a);
    put16(d->dir + 0x1FFE, b);
    put16(d->bat + 4, (be16(d->bat + 4) + 1) & 0xFFFF);
    checksums(d->bat + 4, GC_BLOCK - 4, &a, &b);
    put16(d->bat, a);
    put16(d->bat + 2, b);
    c->dir = c->dir == 1 ? 2 : 1;
    c->bat = c->bat == 3 ? 4 : 3;
    memcpy(block(c, c->dir), d->dir, GC_BLOCK);
    memcpy(block(c, c->bat), d->bat, GC_BLOCK);
}

static unsigned char *dmap(const gc_card *c, draft *d, unsigned blk) {
    (void)c;
    return d->bat + 0xA + (size_t)(blk - FIRST_DATA) * 2;
}

/* Frees entry i's blocks and empties it. */
static void draft_drop(const gc_card *c, draft *d, int i) {
    unsigned char *e = d->dir + i * GC_DENTRY;
    unsigned count = be16(e + 0x38), blk = be16(e + 0x36), freed = 0;
    for (unsigned k = 0; k < count && is_data(c, blk); k++) {
        unsigned char *m = dmap(c, d, blk);
        blk = be16(m);
        put16(m, 0);
        freed++;
    }
    put16(d->bat + 6, be16(d->bat + 6) + freed);
    memset(e, 0xFF, GC_DENTRY);
}

int gc_card_remove(gc_card *c, const char *id) {
    draft d;
    draft_start(c, &d);
    int n = 0;
    for (int i = 0; i < NENTRIES; i++) {
        const unsigned char *e = d.dir + i * GC_DENTRY;
        if (empty(e) || !same_id(e, id)) continue;
        draft_drop(c, &d, i);
        n++;
    }
    if (n) draft_commit(c, &d);
    return n;
}

int gc_card_put(gc_card *c, const unsigned char *g, size_t size) {
    if (!gc_gci_valid(g, size)) return -1;
    draft d;
    draft_start(c, &d);
    int slot = -1;
    for (int i = 0; i < NENTRIES; i++) {
        unsigned char *e = d.dir + i * GC_DENTRY;
        if (!empty(e) && gc_gci_same_save(e, g)) draft_drop(c, &d, i);
        if (empty(e) && slot < 0) slot = i;
    }
    const unsigned count = be16(g + 0x38);
    if (slot < 0 || be16(d.bat + 6) < count) return -1;
    /* Free blocks from just after the last one given out, wrapping round,
     * as Dolphin hands them out. */
    unsigned *blocks = malloc(sizeof *blocks * count), found = 0;
    if (!blocks) return -1;
    unsigned start = be16(d.bat + 8) + 1;
    if (start < FIRST_DATA || start >= (unsigned)c->nblocks) start = FIRST_DATA;
    for (unsigned k = 0; k < (unsigned)c->nblocks - FIRST_DATA && found < count; k++) {
        unsigned blk = FIRST_DATA + (start - FIRST_DATA + k) % ((unsigned)c->nblocks - FIRST_DATA);
        if (be16(dmap(c, &d, blk)) == 0) blocks[found++] = blk;
    }
    if (found < count) {
        free(blocks);
        return -1;
    }
    for (unsigned k = 0; k < count; k++) {
        memcpy(block(c, (int)blocks[k]), g + GC_DENTRY + (size_t)k * GC_BLOCK, GC_BLOCK);
        put16(dmap(c, &d, blocks[k]), k + 1 < count ? blocks[k + 1] : 0xFFFF);
    }
    put16(d.bat + 6, be16(d.bat + 6) - count);
    put16(d.bat + 8, blocks[count - 1]);
    unsigned char *e = d.dir + slot * GC_DENTRY;
    memcpy(e, g, GC_DENTRY);
    put16(e + 0x36, blocks[0]);
    free(blocks);
    draft_commit(c, &d);
    return 0;
}

void gc_gci_name(const unsigned char *g, char *out, size_t n) {
    char name[33 * 6 + 1];
    size_t o = 0;
    for (int i = 0; i < 32 && g[8 + i]; i++) {
        unsigned char ch = g[8 + i];
        /* Dolphin's escaping of characters a file name can't have. */
        if (ch < 0x20 || strchr("\"*/:<>?\\|", ch))
            o += (size_t)snprintf(name + o, sizeof name - o, "__%02x__", ch);
        else
            name[o++] = (char)ch;
    }
    name[o] = 0;
    snprintf(out, n, "%.2s-%.4s-%s.gci", (const char *)g + 4, (const char *)g, name);
}
