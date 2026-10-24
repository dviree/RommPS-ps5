/* PS2 memory card saves through sigil: a game's saves come off a folder card
 * as one unit, go back on another card beside other games' saves, and the
 * unit is the same bytes every time (its hash decides whether to upload).
 *   make unit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ps2card.h"
#include "util.h"
#include "zip.h"

static int failures;
#define CHECK(x)                                                                 \
    do {                                                                         \
        if (!(x)) {                                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);              \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void put(const char *dir, const char *name, const char *text) {
    char path[PATH_MAX_LEN];
    path_join(path, sizeof path, dir, name);
    mkdir_parent(path);
    write_file_atomic(path, text, strlen(text));
}

static int same_file(const char *path, const char *text) {
    size_t n = 0;
    char *got = read_file(path, &n);
    int same = got && n == strlen(text) && !memcmp(got, text, n);
    free(got);
    return same;
}

/* A folder card in dir/name: the superblock sigil formats, as PCSX2 would. */
static void folder_card(const char *dir, const char *name) {
    char card[PATH_MAX_LEN];
    path_join(card, sizeof card, dir, name);
    put(card, "_pcsx2_superblock", "");
}

int main(void) {
    char root[] = "/tmp/ps2card_test.XXXXXX";
    if (!mkdtemp(root)) return 1;
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN], f[PATH_MAX_LEN], zip[PATH_MAX_LEN], p[PATH_MAX_LEN];
    path_join(a, sizeof a, root, "a");
    path_join(b, sizeof b, root, "b");
    path_join(f, sizeof f, root, "f");
    path_join(zip, sizeof zip, root, "saves.zip");
    const char *game = "/games/Tekken 5 (USA).iso", *serial = "SLUS-21059";
    const char *data = "tekken save data", *icon = "PS2D icon.sys stand-in";

    CHECK(ps2card_folder_is_game("BASLUS-21059", serial));
    CHECK(ps2card_folder_is_game("BASLUS-21059TK5", serial));
    CHECK(!ps2card_folder_is_game("BASLUS-21060", serial));
    CHECK(!ps2card_folder_is_game("BADATA-SYSTEM", serial));

    /* A zip of save folders, as clients sent before sigil's units. */
    char src[PATH_MAX_LEN], f1[PATH_MAX_LEN], f2[PATH_MAX_LEN];
    path_join(src, sizeof src, root, "src");
    put(src, "BASLUS-21059/BASLUS-21059", data);
    put(src, "BASLUS-21059/icon.sys", icon);
    path_join(f1, sizeof f1, src, "BASLUS-21059/BASLUS-21059");
    path_join(f2, sizeof f2, src, "BASLUS-21059/icon.sys");
    zip_member m[] = {{f1, "BASLUS-21059/BASLUS-21059"}, {f2, "BASLUS-21059/icon.sys"}};
    CHECK(zip_write(zip, m, 2) == 0);
    size_t zlen = 0;
    unsigned char *z = (unsigned char *)read_file(zip, &zlen);

    /* It goes on folder card A. */
    ps2_cards ca = {.slot = {"Mcd001.ps2", "Mcd002.ps2"}};
    str_copy(ca.dir, sizeof ca.dir, a);
    folder_card(a, "Mcd001.ps2");
    CHECK(z && ps2card_restore(&ca, game, serial, z, zlen) == 0);
    path_join(p, sizeof p, a, "Mcd001.ps2/BASLUS-21059/BASLUS-21059");
    CHECK(same_file(p, data));
    path_join(p, sizeof p, a, "Mcd001.ps2/BASLUS-21059/icon.sys");
    CHECK(same_file(p, icon));

    /* It comes off as one card, the same bytes each time. */
    unsigned char *u1 = NULL, *u2 = NULL;
    size_t l1 = 0, l2 = 0;
    char h1[33], h2[33], art[256], md5[33], id1[33], id2[33];
    CHECK(ps2card_collect(&ca, game, serial, &u1, &l1, h1, art, id1) == 0);
    CHECK(u1 && l1 == 8650752); /* 8 MB with ECC */
    CHECK(!strcmp(art, "Tekken 5 (USA).ps2"));
    md5_hex(u1, l1, md5);
    CHECK(!strcmp(h1, md5)); /* RomM's content_hash of a lone file */
    CHECK(id1[0] && strcmp(id1, h1) != 0);
    CHECK(ps2card_collect(&ca, game, serial, &u2, &l2, h2, art, id2) == 0);
    CHECK(l1 == l2 && !strcmp(h1, h2) && !strcmp(id1, id2));

    /* The same saves with other timestamps (another console's _pcsx2_index):
     * another unit, one identity. */
    char index[PATH_MAX_LEN], hs[33], ids[33];
    path_join(index, sizeof index, a, "Mcd001.ps2/BASLUS-21059/_pcsx2_index");
    char *txt = read_file(index, NULL), *tm = txt ? strstr(txt, "timeModified: ") : NULL;
    CHECK(tm != NULL);
    if (tm) {
        tm += strlen("timeModified: ");
        *tm = *tm == '1' ? '2' : '1';
        CHECK(write_file_atomic(index, txt, strlen(txt)) == 0);
    }
    free(txt);
    unsigned char *us = NULL;
    size_t ls = 0;
    CHECK(ps2card_collect(&ca, game, serial, &us, &ls, hs, art, ids) == 0);
    CHECK(strcmp(hs, h1) != 0 && !strcmp(ids, id1));
    free(us);
    CHECK(ps2card_restore(&ca, game, serial, u1, l1) == 0); /* the same saves: nothing to put back */

    /* Another game has nothing on it. */
    unsigned char *none = NULL;
    size_t nl = 0;
    char hn[33], idn[33];
    CHECK(ps2card_collect(&ca, "/games/Other.iso", "SLUS-20000", &none, &nl, hn, art, idn) == 0);
    CHECK(!none && !hn[0] && !idn[0]);

    /* Card B, its superblock copied from a formatted card (as from another
     * console), keeps another game's save beside it. */
    ps2_cards cb = {.slot = {"Mcd001.ps2", ""}};
    str_copy(cb.dir, sizeof cb.dir, b);
    char sb_from[PATH_MAX_LEN], sb_to[PATH_MAX_LEN];
    path_join(sb_from, sizeof sb_from, a, "Mcd001.ps2/_pcsx2_superblock");
    path_join(sb_to, sizeof sb_to, b, "Mcd001.ps2/_pcsx2_superblock");
    CHECK(mkdir_parent(sb_to) == 0 && copy_file(sb_from, sb_to) == 0);
    path_join(p, sizeof p, b, "Mcd001.ps2");
    put(p, "BASLUS-20000/BASLUS-20000", "other game");
    CHECK(ps2card_restore(&cb, game, serial, u1, l1) == 0);
    path_join(p, sizeof p, b, "Mcd001.ps2/BASLUS-21059/BASLUS-21059");
    CHECK(same_file(p, data));
    path_join(p, sizeof p, b, "Mcd001.ps2/BASLUS-20000/BASLUS-20000");
    CHECK(same_file(p, "other game"));
    unsigned char *u3 = NULL;
    size_t l3 = 0;
    char h3[33], id3[33];
    CHECK(ps2card_collect(&cb, game, serial, &u3, &l3, h3, art, id3) == 0);
    CHECK(!strcmp(h3, h1) && !strcmp(id3, id1));

    /* A file card holding the save gives the same unit, and takes it back. */
    ps2_cards cf = {.slot = {"Mcd001.ps2", ""}};
    str_copy(cf.dir, sizeof cf.dir, f);
    path_join(p, sizeof p, f, "Mcd001.ps2");
    CHECK(mkdir_parent(p) == 0 && write_file_atomic(p, u1, l1) == 0);
    CHECK(ps2card_file_has(p, serial));
    CHECK(!ps2card_file_has(p, "SLUS-20000"));
    unsigned char *u4 = NULL;
    size_t l4 = 0;
    char h4[33], id4[33];
    CHECK(ps2card_collect(&cf, game, serial, &u4, &l4, h4, art, id4) == 0);
    CHECK(!strcmp(h4, h1) && !strcmp(id4, id1));
    CHECK(ps2card_restore(&cf, game, serial, u1, l1) == 0);
    free(u4);

    free(z);
    free(u1);
    free(u2);
    free(u3);
    char cmd[PATH_MAX_LEN + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", root);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("ps2card: ok\n");
    return failures != 0;
}
