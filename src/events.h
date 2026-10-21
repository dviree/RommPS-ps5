/* The sync log: one entry per file a sync moved or left alone on purpose,
 * newest last, the last EVENTS_MAX kept in events.json.
 *
 *   {"t": 1791936000, "sync": "game exited", "rom_id": 878,
 *    "kind": "save" | "state" | "card" | "bios" | "game" | "emulator",
 *    "action": "uploaded" | "downloaded" | "conflict" | "resolved" |
 *              "skipped" | "restored" | "backed up" | "failed" |
 *              "moved" | "removed",
 *    "path": "/data/.../Game.srm", "emulator": "mgba", "detail": "..."}
 *
 * "bios" (rom_id 0): a BIOS file put in or removed, "detail" its name. "game":
 * a game moved to the main emulator, or an extra's copy removed. "emulator"
 * (rom_id 0): a mount that couldn't go on. The game's name and platform are
 * filled in when the log is read. */
#ifndef ROMM_EVENTS_H
#define ROMM_EVENTS_H

#include "cJSON.h"

#define EVENTS_MAX 500

void events_load(void);
void events_save(void); /* after each sync; cheap when nothing changed */

/* Names the sync the next entries come from ("startup", "game exited"...). */
void events_set_sync(const char *reason);

void events_add(const char *kind, const char *action, int rom_id, const char *path, const char *emulator,
                const char *detail);

/* Counts the entries added so far; with events_games, the games a sync touched. */
unsigned events_seq(void);
/* The games of the saves and states moved since events_seq() was "since":
 * "Wind Waker, Mario Kart and 2 more" (at most "max" names). 0 if none. */
int events_games(unsigned since, int max, char *out, size_t n);

/* Newest first; rom_id 0 for every game. Caller frees. */
cJSON *events_json(int rom_id, int limit);

#endif
