/* GameCube memory cards (Dolphin's .raw) and the .gci files of single saves.
 *
 * A card is 8 KiB blocks: 0 the header, 1 and 2 the directory and its
 * previous copy, 3 and 4 the block allocation table and its previous copy,
 * then the saves. Each directory entry (64 bytes) names a save: game code,
 * maker, file name, first block and block count; the table chains its
 * blocks. A .gci is one entry followed by its blocks.
 *
 * Changes are made the way Dolphin makes them: the current directory and
 * table are edited, their update counters go up by one, their checksums are
 * set, and they're written over the older copies, which become the backups. */
#ifndef ROMM_GCMEM_H
#define ROMM_GCMEM_H

#include <stddef.h>

#define GC_BLOCK 0x2000
#define GC_DENTRY 0x40

typedef struct {
    unsigned char *data;
    size_t size;
    int nblocks;
    int dir, bat; /* which copy is current: block 1 or 2, 3 or 4 */
} gc_card;

/* 0 when data (size bytes, copied) is a card whose header, directory and
 * table check out. */
int gc_card_open(gc_card *c, const unsigned char *data, size_t size);
void gc_card_free(gc_card *c);

/* A .gci in memory: 0 when it's one entry and as many blocks as it says. */
int gc_gci_valid(const unsigned char *gci, size_t size);

/* The saves of a game: entries whose game code and maker are id's six
 * characters ("GZLE01"). Calls fn with each as a .gci; returns how many. */
typedef void (*gc_save_fn)(const unsigned char *gci, size_t size, void *ud);
int gc_card_saves(const gc_card *c, const char *id, gc_save_fn fn, void *ud);

/* Takes the game's saves off the card. */
int gc_card_remove(gc_card *c, const char *id);

/* Puts a .gci on the card, over any save with the same game code, maker and
 * file name. 0, or -1 when it doesn't fit or the directory is full. */
int gc_card_put(gc_card *c, const unsigned char *gci, size_t size);

/* Dolphin's name for a .gci in a GCI folder: "01-GZLE-gczelda2.gci". */
void gc_gci_name(const unsigned char *gci, char *out, size_t n);

/* The game id ("GZLE01") a .gci belongs to. */
void gc_gci_id(const unsigned char *gci, char out[7]);

/* Whether two .gci are the same save (game code, maker and file name). */
int gc_gci_same_save(const unsigned char *a, const unsigned char *b);

#endif
