/* GameCube memory cards: saves go on, come off and are replaced, and the
 * card stays one Dolphin reads (checksums, chains, free count).
 *   cc -Isrc tests/gcmem_test.c src/gcmem.c -o build/host/gcmem_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcmem.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8), p[1] = (unsigned char)v; }

static void sums(const unsigned char *p, size_t n, unsigned *a, unsigned *b) {
    unsigned x = 0, y = 0;
    for (size_t i = 0; i < n; i += 2) x = (x + be16(p + i)) & 0xFFFF, y = (y + (be16(p + i) ^ 0xFFFF)) & 0xFFFF;
    *a = x == 0xFFFF ? 0 : x, *b = y == 0xFFFF ? 0 : y;
}

/* A blank card as Dolphin formats one: header, two empty directories, two
 * tables with every data block free. */
static unsigned char *blank(int nblocks) {
    unsigned char *c = malloc((size_t)nblocks * GC_BLOCK);
    memset(c, 0xFF, (size_t)nblocks * GC_BLOCK);
    memset(c, 0, 0x26);
    put16(c + 0x22, (unsigned)nblocks / 16);
    unsigned a, b;
    sums(c, 0x1FC, &a, &b);
    put16(c + 0x1FC, a), put16(c + 0x1FE, b);
    for (int k = 1; k <= 2; k++) {
        unsigned char *d = c + k * GC_BLOCK;
        put16(d + 0x1FFA, (unsigned)k - 1);
        sums(d, 0x1FFC, &a, &b);
        put16(d + 0x1FFC, a), put16(d + 0x1FFE, b);
    }
    for (int k = 3; k <= 4; k++) {
        unsigned char *t = c + k * GC_BLOCK;
        memset(t, 0, GC_BLOCK);
        put16(t + 4, (unsigned)k - 3);
        put16(t + 6, (unsigned)nblocks - 5);
        put16(t + 8, 4);
        sums(t + 4, GC_BLOCK - 4, &a, &b);
        put16(t, a), put16(t + 2, b);
    }
    return c;
}

static unsigned char *gci(const char *id, const char *file, int blocks, unsigned char fill, size_t *size) {
    *size = GC_DENTRY + (size_t)blocks * GC_BLOCK;
    unsigned char *g = malloc(*size);
    memset(g, 0xFF, GC_DENTRY);
    memcpy(g, id, 6);
    memset(g + 8, 0, 32);
    memcpy(g + 8, file, strlen(file));
    put16(g + 0x36, 0x1234); /* where it sat on another card: ignored */
    put16(g + 0x38, (unsigned)blocks);
    for (int k = 0; k < blocks; k++) memset(g + GC_DENTRY + (size_t)k * GC_BLOCK, fill + k, GC_BLOCK);
    return g;
}

/* What Dolphin checks, from the current copies: checksums, every chain
 * inside the card and no block in two, the free count right. */
static void check_card(const gc_card *c) {
    const unsigned char *d = c->data + (size_t)c->dir * GC_BLOCK, *t = c->data + (size_t)c->bat * GC_BLOCK;
    unsigned a, b;
    sums(d, 0x1FFC, &a, &b);
    CHECK(a == be16(d + 0x1FFC) && b == be16(d + 0x1FFE));
    sums(t + 4, GC_BLOCK - 4, &a, &b);
    CHECK(a == be16(t) && b == be16(t + 2));
    char used[4096] = {0};
    int in_use = 0;
    for (int i = 0; i < 127; i++) {
        const unsigned char *e = d + i * GC_DENTRY;
        if (e[0] == 0xFF && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF) continue;
        unsigned blk = be16(e + 0x36), n = be16(e + 0x38);
        for (unsigned k = 0; k < n; k++) {
            CHECK(blk >= 5 && blk < (unsigned)c->nblocks);
            if (blk < 5 || blk >= (unsigned)c->nblocks) break;
            CHECK(!used[blk]);
            used[blk] = 1;
            in_use++;
            unsigned next = be16(t + 0xA + (blk - 5) * 2);
            if (k + 1 == n) CHECK(next == 0xFFFF);
            blk = next;
        }
    }
    int zeros = 0;
    for (int blk = 5; blk < c->nblocks; blk++) {
        unsigned m = be16(t + 0xA + (blk - 5) * 2);
        zeros += m == 0;
        CHECK((m == 0) == !used[blk]);
    }
    CHECK(be16(t + 6) == (unsigned)zeros);
    CHECK(zeros + in_use == c->nblocks - 5);
}

typedef struct { unsigned char *g[8]; size_t size[8]; int n; } got;
static void keep(const unsigned char *g, size_t size, void *ud) {
    got *x = ud;
    x->g[x->n] = malloc(size);
    memcpy(x->g[x->n], g, size);
    x->size[x->n++] = size;
}
static void got_free(got *x) { for (int i = 0; i < x->n; i++) free(x->g[i]); x->n = 0; }

int main(void) {
    const int nblocks = 64; /* a 59-block card */
    unsigned char *raw = blank(nblocks);
    gc_card c;
    CHECK(gc_card_open(&c, raw, (size_t)nblocks * GC_BLOCK) == 0);
    check_card(&c);

    size_t za, zb, zc;
    unsigned char *a = gci("GZLE01", "gczelda", 3, 0x10, &za), *b = gci("GMSE01", "super_mario_sunshine", 7, 0x40, &zb);
    CHECK(gc_gci_valid(a, za) && gc_gci_valid(b, zb));
    CHECK(gc_card_put(&c, a, za) == 0);
    CHECK(gc_card_put(&c, b, zb) == 0);
    check_card(&c);

    got x = {0};
    CHECK(gc_card_saves(&c, "GZLE01", keep, &x) == 1);
    /* The same save comes back, at its place on this card. */
    CHECK(x.n == 1 && x.size[0] == za && !memcmp(x.g[0] + GC_DENTRY, a + GC_DENTRY, za - GC_DENTRY));
    CHECK(x.n == 1 && !memcmp(x.g[0], a, 0x36) && !memcmp(x.g[0] + 0x38, a + 0x38, 8));
    got_free(&x);

    /* Same save again, bigger: replaced, not added. */
    unsigned char *a2 = gci("GZLE01", "gczelda", 5, 0x70, &zc);
    CHECK(gc_card_put(&c, a2, zc) == 0);
    check_card(&c);
    CHECK(gc_card_saves(&c, "GZLE01", keep, &x) == 1);
    CHECK(x.n == 1 && x.size[0] == zc && !memcmp(x.g[0] + GC_DENTRY, a2 + GC_DENTRY, zc - GC_DENTRY));
    got_free(&x);
    CHECK(gc_card_saves(&c, "GMSE01", keep, &x) == 1);
    CHECK(x.n == 1 && !memcmp(x.g[0] + GC_DENTRY, b + GC_DENTRY, zb - GC_DENTRY));
    got_free(&x);

    /* A second file of the same game sits beside the first. */
    size_t zd;
    unsigned char *d = gci("GZLE01", "gczelda_opt", 1, 0x90, &zd);
    CHECK(gc_card_put(&c, d, zd) == 0);
    CHECK(gc_card_saves(&c, "GZLE01", NULL, NULL) == 2);
    check_card(&c);

    /* Removing a game frees its blocks and leaves the others. */
    CHECK(gc_card_remove(&c, "GZLE01") == 2);
    CHECK(gc_card_saves(&c, "GZLE01", NULL, NULL) == 0);
    CHECK(gc_card_saves(&c, "GMSE01", NULL, NULL) == 1);
    check_card(&c);

    /* Too big for what's left: refused, card untouched. */
    size_t zbig;
    unsigned char *big = gci("GBIG01", "big", 60, 0x01, &zbig);
    unsigned char *before = malloc(c.size);
    memcpy(before, c.data, c.size);
    CHECK(gc_card_put(&c, big, zbig) == -1);
    CHECK(!memcmp(before, c.data, c.size));

    /* The card reopens from its bytes, with the newer copies current. */
    gc_card again;
    CHECK(gc_card_open(&again, c.data, c.size) == 0);
    CHECK(again.dir == c.dir && again.bat == c.bat);
    CHECK(gc_card_saves(&again, "GMSE01", NULL, NULL) == 1);
    check_card(&again);

    /* A spoilt current directory: the previous copy is used. */
    again.data[(size_t)again.dir * GC_BLOCK + 100] ^= 0xFF;
    gc_card fallback;
    CHECK(gc_card_open(&fallback, again.data, again.size) == 0);
    CHECK(fallback.dir != again.dir);
    gc_card_free(&fallback);

    char name[128];
    gc_gci_name(a, name, sizeof name);
    CHECK(!strcmp(name, "01-GZLE-gczelda.gci"));
    size_t zq;
    unsigned char *q = gci("GQQE01", "a/b?", 1, 0, &zq);
    gc_gci_name(q, name, sizeof name);
    CHECK(!strcmp(name, "01-GQQE-a__2f__b__3f__.gci"));

    CHECK(gc_card_open(&fallback, raw, 1000) == -1);
    CHECK(!gc_gci_valid(a, za - 1));

    gc_card_free(&again);
    gc_card_free(&c);
    free(raw), free(a), free(a2), free(b), free(d), free(big), free(before), free(q);
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    puts("gcmem: ok");
    return 0;
}
