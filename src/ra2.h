/* PS5 RetroArch's game folders change with its versions: content/<our system
 * name> before 2.0, the libretro database's in 2.x (content/Sony - PlayStation
 * 2), ES-DE's from 3.0 (content/ps2, content/gc), which renames 2.x's folders
 * on its first start. RetroArch makes its folders itself. Each system with
 * one gets it (unless the user picked a folder), our records follow a folder
 * RetroArch renamed, and games left in an older folder move once the user
 * says so. */
#ifndef ROMM_RA2_H
#define ROMM_RA2_H

#include "cJSON.h"

/* RetroArch at root's folder layout: 0 (before 2.0), 2 or 3 (see above). */
int ra2_layout(const char *root);
/* RetroArch at root is 2.0 or newer, as its webui/version.json says. */
int ra2_is_v2(const char *root);

/* Points a RetroArch profile's systems at its version's folders, unless the
 * user picked others. How many changed. moves (or NULL) gets {"from", "to"}
 * for folders RetroArch renamed itself, whose records should follow. */
int ra2_update_profile(cJSON *profile, cJSON *moves);

/* Looks at the profiles and the old folders again: at most once a minute,
 * unless now. Takes the config lock. */
void ra2_check(int now);

/* The move waiting for the user, or NULL. Caller frees.
 *   {"games": 12, "folders": [{"from", "to", "games"}]} */
cJSON *ra2_pending(void);

/* Moves the games in the old folders to the new ones. 0 when all of them moved. */
int ra2_move_games(int *moved, char *err, int en);

#endif
