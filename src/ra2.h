/* RetroArch 2.0 on the PS5 keeps each system's games in a folder named after
 * its libretro database (content/Sony - PlayStation 2, content/Nintendo -
 * Game Boy Advance), where 1.x had content/ps2 and content/gba. It makes
 * those folders itself. Each system with one gets it (unless the user picked
 * a folder), and the games already in the old folders move there once the
 * user says so. */
#ifndef ROMM_RA2_H
#define ROMM_RA2_H

#include "cJSON.h"

/* RetroArch at root is 2.0 or newer, as its webui/version.json says. */
int ra2_is_v2(const char *root);

/* Points a RetroArch 2.0 profile's systems at their new folders, unless the
 * user picked others. How many changed. */
int ra2_update_profile(cJSON *profile);

/* Looks at the profiles and the old folders again: at most once a minute,
 * unless now. Takes the config lock. */
void ra2_check(int now);

/* The move waiting for the user, or NULL. Caller frees.
 *   {"games": 12, "folders": [{"from", "to", "games"}]} */
cJSON *ra2_pending(void);

/* Moves the games in the old folders to the new ones. 0 when all of them moved. */
int ra2_move_games(int *moved, char *err, int en);

#endif
