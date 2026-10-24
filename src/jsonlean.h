/* Drops object members by name from JSON as it streams in, so a big reply
 * (RomM's game lists, heavy with RetroAchievements and scraper metadata) never
 * has to be held whole. The output is the same JSON without those members. */
#ifndef ROMM_JSONLEAN_H
#define ROMM_JSONLEAN_H

#include <stddef.h>

#define JLEAN_DEPTH 64
#define JLEAN_KEY 64

typedef struct {
    const char *const *drop; /* names to drop, NULL-terminated */
    unsigned char obj[JLEAN_DEPTH];  /* per level: 1 object, 0 array */
    unsigned char kept[JLEAN_DEPTH]; /* per object: a member was written */
    int depth, deep;                 /* deep: levels past JLEAN_DEPTH, passed through */
    int in_str, esc, expect_key;
    int in_key, key_len, key_long;
    char key[JLEAN_KEY + 2];
    int skipping, skip_started, skip_nest, skip_str, skip_esc;
} jlean;

void jlean_init(jlean *j, const char *const *drop);

/* Filters n bytes of input, calling put for the bytes that stay (at most
 * n + JLEAN_KEY + 3 per call). put returns 0 to stop; so does jlean_feed. */
typedef int (*jlean_put)(void *ud, const char *bytes, size_t n);
int jlean_feed(jlean *j, const char *in, size_t n, jlean_put put, void *ud);

#endif
