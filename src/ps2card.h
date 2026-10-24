/* PS2 memory card saves, one game at a time, in the unit RomM's other clients
 * use (rommapp/argosy-sigil, docs/platforms/ps2.md): an 8 MB .ps2 card with
 * ECC holding only the game's save folders. The cards in the slots can be
 * file cards or PCSX2 folder cards (a directory of save folders and
 * _pcsx2_superblock); sigil reads and writes both. */
#ifndef ROMM_PS2CARD_H
#define ROMM_PS2CARD_H

#include <stddef.h>

#include "util.h"

typedef struct {
    char dir[PATH_MAX_LEN]; /* the emulator's memory card folder */
    char slot[2][256];      /* the cards in slots 1 and 2, by file or folder name ("" for none) */
} ps2_cards;

/* The game's saves (serial "SLUS-21059") off the slot cards, as the unit RomM
 * keeps. 0 with *unit NULL when the game has none; *unit is malloc'd. hash is
 * RomM's content_hash of the unit, artifact the name it travels under
 * ("Tekken 5 (USA).ps2", from content_path). identity is over the saves
 * themselves: the same saves with other timestamps (each console's
 * _pcsx2_index) or on another card give other units, but one identity. */
int ps2card_collect(const ps2_cards *c, const char *content_path, const char *serial, unsigned char **unit,
                    size_t *len, char hash[33], char artifact[256], char identity[33]);

/* Puts a unit from RomM (sigil's card, or a zip of save folders) on the slot
 * cards in place of the game's saves there. Other games' saves stay. */
int ps2card_restore(const ps2_cards *c, const char *content_path, const char *serial, const unsigned char *unit,
                    size_t len);

/* Whether the file card at path holds a save of the game. */
int ps2card_file_has(const char *path, const char *serial);

/* Whether a card folder entry ("BASLUS-21059TEKKEN") is a save of the game. */
int ps2card_folder_is_game(const char *name, const char *serial);

#endif
