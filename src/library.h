/* Library browsing and the download queue. */
#ifndef ROMM_LIBRARY_H
#define ROMM_LIBRARY_H

#include "cJSON.h"

void library_init(void);
void library_wake(void); /* re-check the queue, e.g. after the concurrency setting changed */

cJSON *library_platforms(char *err, int en);
/* Links the games a system already has into its extra emulators, in the background. */
void library_link_extras(void);

/* An extra's games of its own, the only thing in the way of its mount
 * (profile_map.mount_blocked), by its root and system: "move" renames them into the main emulator's folder, "delete"
 * removes them. Games the main one has too are left: they're copies, cleared
 * once the mount is on. Returns how many, or -1 with err set. */
int library_own_games(const char *root, const char *system, const char *action, char *err, int en);

/* 1 for a file RomM lists as a game that isn't one (metadata.txt, artwork...). */
int library_not_game(const char *fs_name);
/* platform_id 0 searches every platform. */
cJSON *library_roms(int platform_id, const char *search, int offset, int limit, char *err, int en);
/* What RommPS shows about one game: release year, publishers, genres, players,
 * rating, summary, How Long to Beat, the user's last played, status and play time. */
cJSON *library_details(int rom_id, char *err, int en);
/* The games on this console, most recently played first. */
cJSON *library_installed(int limit);
/* Changes when a game appears on the console or goes from it, deleted by
 * hand included, so clients know to load their lists again. Cheap to ask
 * for: it looks at the disk at most every 15 s, and not while a game runs. */
unsigned library_installed_sig(void);
/* Whether a game is on the console now (RommPS asks as its sheet opens). */
int library_on_console(int rom_id);

/* name: the game's name as the client shows it ("Game #<id>" if "" until the job runs).
 * cover: the client's copy of RomM's path_cover_small, if it has one; the job re-reads it from RomM when it runs. */
int library_queue_rom(int rom_id, const char *name, const char *cover, char *err, int en);
int library_queue_firmware(int platform_id, char *err, int en);
int library_cancel(int id, char *err, int en);
int library_clear_finished(void);
int library_delete_rom(int rom_id, char *err, int en);
cJSON *library_downloads(void);
int library_busy(void); /* a download is queued or running */

#endif
