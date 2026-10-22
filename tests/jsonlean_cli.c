/* Feeds stdin through the JSON filter (src/jsonlean.c) in chunks of the given
 * sizes, dropping the given names; for tests/jsonlean_test.py. */
#include <stdio.h>
#include <stdlib.h>

#include "jsonlean.h"

static int put(void *ud, const char *b, size_t n) {
    (void)ud;
    return fwrite(b, 1, n, stdout) == n;
}

int main(int argc, char **argv) {
    /* jsonlean_cli SEED NAME... */
    if (argc < 2) return 2;
    srand((unsigned)atoi(argv[1]));
    static char in[1 << 22];
    const size_t len = fread(in, 1, sizeof in, stdin);
    jlean j;
    jlean_init(&j, (const char *const *)(argv + 2));
    for (size_t at = 0; at < len;) {
        size_t n = 1 + (size_t)rand() % 97;
        if (n > len - at) n = len - at;
        if (!jlean_feed(&j, in + at, n, put, NULL)) return 1;
        at += n;
    }
    return 0;
}
