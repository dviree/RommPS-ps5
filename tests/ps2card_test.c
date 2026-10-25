/* PS2 memory card saves through sigil: a game's saves come off a folder card
 * as one unit, go back on another card beside other games' saves, and the
 * unit is the same bytes every time (its hash decides whether to upload).
 *   make unit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    /* An empty folder in slot 1 (PS5SX2 makes memcards/MemoryCard empty) is
     * the folder card the save goes on, not the file card in slot 2. */
    char e[PATH_MAX_LEN], mc2[PATH_MAX_LEN];
    path_join(e, sizeof e, root, "e");
    ps2_cards ce = {.slot = {"MemoryCard", "Mcd002.ps2"}};
    str_copy(ce.dir, sizeof ce.dir, e);
    path_join(p, sizeof p, e, "MemoryCard");
    CHECK(mkdir_parent(p) == 0 && mkdir(p, 0777) == 0);
    path_join(mc2, sizeof mc2, e, "Mcd002.ps2");
    unsigned char *erased = malloc(l1);
    CHECK(erased != NULL);
    if (erased) {
        memset(erased, 0xFF, l1);
        CHECK(write_file_atomic(mc2, erased, l1) == 0);
    }
    CHECK(ps2card_restore(&ce, game, serial, u1, l1) == 0);
    path_join(p, sizeof p, e, "MemoryCard/BASLUS-21059/BASLUS-21059");
    CHECK(same_file(p, data));
    size_t l5 = 0;
    char *slot2 = read_file(mc2, &l5);
    CHECK(slot2 && erased && l5 == l1 && !memcmp(slot2, erased, l1));
    free(slot2);
    free(erased);

    /* One card per game: the game's saves on the card it played on come onto
     * its own card, a file that isn't there yet; the old card stays as it was. */
    ps2_cards from = {.slot = {"Mcd001.ps2", ""}}, to = {.slot = {"Tekken 5 (USA).ps2", ""}};
    str_copy(from.dir, sizeof from.dir, f);
    str_copy(to.dir, sizeof to.dir, f);
    char old_card[PATH_MAX_LEN], own_card[PATH_MAX_LEN];
    path_join(old_card, sizeof old_card, f, "Mcd001.ps2");
    path_join(own_card, sizeof own_card, f, "Tekken 5 (USA).ps2");
    size_t lo = 0;
    char *before = read_file(old_card, &lo);
    unsigned char *u6 = NULL;
    size_t l6 = 0;
    char h6[33], id6[33], h7[33], id7[33];
    CHECK(ps2card_collect(&from, game, serial, &u6, &l6, h6, art, id6) == 0 && u6);
    CHECK(u6 && ps2card_restore(&to, game, serial, u6, l6) == 0);
    CHECK(file_exists(own_card) && ps2card_file_has(own_card, serial));
    unsigned char *u7 = NULL;
    size_t l7 = 0;
    CHECK(ps2card_collect(&to, game, serial, &u7, &l7, h7, art, id7) == 0 && !strcmp(id7, id1));
    size_t la = 0;
    char *after = read_file(old_card, &la);
    CHECK(before && after && lo == la && !memcmp(before, after, lo));
    free(before);
    free(after);
    free(u6);
    free(u7);

    /* A game's own card made blank, with a shared card in slot 2: what comes
     * from RomM lands on its own card, and slot 2's stays as it was. */
    char g[PATH_MAX_LEN], blank[PATH_MAX_LEN], shared2[PATH_MAX_LEN];
    path_join(g, sizeof g, root, "g");
    path_join(blank, sizeof blank, g, "Tekken 5 (USA).ps2");
    path_join(shared2, sizeof shared2, g, "Mcd002.ps2");
    CHECK(ps2card_blank(blank) == 0);
    CHECK(file_size(blank) == 8650752 && !ps2card_file_has(blank, serial));
    unsigned char *none2 = NULL;
    size_t ln2 = 0;
    char hn2[33], idn2[33];
    ps2_cards cg = {.slot = {"Tekken 5 (USA).ps2", "Mcd002.ps2"}};
    str_copy(cg.dir, sizeof cg.dir, g);
    CHECK(ps2card_collect(&cg, game, serial, &none2, &ln2, hn2, art, idn2) == 0 && !none2); /* formatted, empty */
    CHECK(ps2card_blank(shared2) == 0);
    ps2_cards other = {.slot = {"Mcd002.ps2", ""}};
    str_copy(other.dir, sizeof other.dir, g);
    put(root, "o/BASLUS-20000/BASLUS-20000", "other game");
    char of[PATH_MAX_LEN], oz[PATH_MAX_LEN];
    path_join(of, sizeof of, root, "o/BASLUS-20000/BASLUS-20000");
    path_join(oz, sizeof oz, root, "o.zip");
    zip_member om[] = {{of, "BASLUS-20000/BASLUS-20000"}};
    CHECK(zip_write(oz, om, 1) == 0);
    size_t ozl = 0;
    unsigned char *ozd = (unsigned char *)read_file(oz, &ozl);
    CHECK(ozd && ps2card_restore(&other, "/games/Other.iso", "SLUS-20000", ozd, ozl) == 0);
    free(ozd);
    size_t l2b = 0;
    char *s2before = read_file(shared2, &l2b);
    CHECK(ps2card_restore(&cg, game, serial, u1, l1) == 0);
    CHECK(ps2card_file_has(blank, serial) && !ps2card_file_has(shared2, serial));
    size_t l2a = 0;
    char *s2after = read_file(shared2, &l2a);
    CHECK(s2before && s2after && l2b == l2a && !memcmp(s2before, s2after, l2b));
    free(s2before);
    free(s2after);

    /* The settings naming the cards: PS5SX2's flat lines (the last one wins)... */
    char *v = NULL;
    char got[256], ini[PATH_MAX_LEN];
    const char *flat = "# Tekken 5\nMemoryCards/Slot1_Filename=Mcd001.ps2\nupscale_multiplier=2\n"
                       "MemoryCards/Slot1_Filename = Other.ps2\nMemoryCards/Slot2_Enable=false\n";
    CHECK(ps2card_setting(flat, 0, "Slot1_Filename", got, sizeof got) == 0 && !strcmp(got, "Other.ps2"));
    CHECK(ps2card_setting(flat, 0, "Slot2_Enable", got, sizeof got) == 0 && !strcmp(got, "false"));
    CHECK(ps2card_setting(flat, 0, "Slot2_Filename", got, sizeof got) != 0);
    CHECK(ps2card_setting(flat, 1, "Slot1_Filename", got, sizeof got) != 0); /* not an ini section */
    path_join(ini, sizeof ini, root, "settings/Tekken 5 (USA).ini");
    put(root, "settings/Tekken 5 (USA).ini", flat);
    CHECK(ps2card_set_setting(ini, 0, "Slot1_Filename", "Tekken 5 (USA).ps2") == 0);
    v = read_file(ini, NULL);
    CHECK(v && !strcmp(v, "# Tekken 5\nupscale_multiplier=2\nMemoryCards/Slot2_Enable=false\n"
                          "MemoryCards/Slot1_Filename=Tekken 5 (USA).ps2\n"));
    free(v);
    /* ... a file that isn't there yet, in a folder that isn't either ... */
    path_join(ini, sizeof ini, root, "new/settings/Game.ini");
    CHECK(ps2card_set_setting(ini, 0, "Slot1_Filename", "Game.ps2") == 0);
    CHECK(same_file(ini, "MemoryCards/Slot1_Filename=Game.ps2\n"));
    /* ... and PCSX2's ini, under [MemoryCards]. */
    const char *pini = "[EmuCore]\nEnableCheats = true\n\n[MemoryCards]\nSlot1_Filename = Mcd001.ps2\nSlot2_Filename = B.ps2\n";
    CHECK(ps2card_setting(pini, 1, "Slot1_Filename", got, sizeof got) == 0 && !strcmp(got, "Mcd001.ps2"));
    CHECK(ps2card_setting(pini, 1, "EnableCheats", got, sizeof got) != 0); /* not a card setting */
    path_join(ini, sizeof ini, root, "gamesettings/SLUS-21059_12345678.ini");
    put(root, "gamesettings/SLUS-21059_12345678.ini", pini);
    CHECK(ps2card_set_setting(ini, 1, "Slot1_Filename", "Tekken 5 (USA).ps2") == 0);
    CHECK(same_file(ini, "[EmuCore]\nEnableCheats = true\n\n[MemoryCards]\nSlot1_Filename = Tekken 5 (USA).ps2\n"
                         "Slot2_Filename = B.ps2\n"));
    put(root, "gamesettings/a.ini", "[EmuCore]\nEnableCheats = true");
    path_join(ini, sizeof ini, root, "gamesettings/a.ini");
    CHECK(ps2card_set_setting(ini, 1, "Slot1_Filename", "A.ps2") == 0);
    CHECK(same_file(ini, "[EmuCore]\nEnableCheats = true\n\n[MemoryCards]\nSlot1_Filename = A.ps2\n"));
    put(root, "gamesettings/b.ini", "[MemoryCards]");
    path_join(ini, sizeof ini, root, "gamesettings/b.ini");
    CHECK(ps2card_set_setting(ini, 1, "Slot1_Filename", "B.ps2") == 0);
    CHECK(same_file(ini, "[MemoryCards]\nSlot1_Filename = B.ps2\n"));
    path_join(ini, sizeof ini, root, "gamesettings/c.ini");
    CHECK(ps2card_set_setting(ini, 1, "Slot1_Filename", "C.ps2") == 0);
    CHECK(same_file(ini, "[MemoryCards]\nSlot1_Filename = C.ps2\n"));

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
