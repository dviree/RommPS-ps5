/* Shared memory cards, synced both ways with RomM's memory card API (5.3.0+):
 * one card on RomM for every console and emulator that keeps it (memcard.c). */
#ifndef ROMM_MEMCARD_H
#define ROMM_MEMCARD_H

#include "cJSON.h"

int memcard_enabled(void);
/* Folders holding the cards, for the save watcher. Returns the count. */
int memcard_dirs(char dirs[][1024], int max);
/* Syncs every card that changed, here or on RomM. Returns how many went up;
 * *down says how many came down. upload_only: a game runs, nothing comes down. */
int memcard_sync(int upload_only, int *down);
/* Replaces a card with its latest version on RomM. */
int memcard_restore(int slot, char *err, int en);

/* Once, after an update to 1.3.1 or newer (one card per game the default
 * there): a console whose shared PS2 cards have synced with RomM keeps
 * syncing them whole, unless the user picks otherwise. Needs the state loaded. */
void memcard_keep_shared(void);
cJSON *memcard_status(void);

#endif
