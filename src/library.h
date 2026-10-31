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

/* RomM's kinds of collections: the user's own, saved filters (smart) and the
 * ones RomM makes from metadata (virtual). Only the kinds turned on in
 * library.c are offered; the others are refused like an unknown name. */
typedef enum { COLLECTION_NONE = -1, COLLECTION_REGULAR, COLLECTION_SMART, COLLECTION_VIRTUAL } collection_kind;
/* "regular", "smart", "virtual" -> its kind, COLLECTION_NONE for anything else or a kind not offered. */
collection_kind library_collection_kind(const char *name);
/* 1 when id can name one of kind's collections (regular and smart ones' are numbers). */
int library_collection_id_ok(collection_kind kind, const char *id);
/* The collections that hold games, of every kind offered (only's, unless it's
 * COLLECTION_NONE): [{kind, id, name, description, rom_count, cover, owner}],
 * and a virtual one's type ("genre", "franchise"...). By kind, then type; in
 * each the user's own first, then other users' public ones, each by name.
 * owner is "" for the user's own. Ids are strings, as virtual collections' are.
 * refused: RomM didn't let this pairing read them (paired before 1.4.0). */
cJSON *library_collections(collection_kind only, int *refused, char *err, int en);

/* Which games a list holds: a platform's (0 for every platform), narrowed to
 * a collection's when collection is set. */
typedef struct {
    int platform_id;
    collection_kind collection; /* COLLECTION_NONE for no collection */
    char collection_id[512]; /* a virtual one's is base64 of its name and type */
} library_scope;

/* 1 for a file RomM lists as a game that isn't one (metadata.txt, artwork...). */
int library_not_game(const char *fs_name);
cJSON *library_roms(const library_scope *scope, const char *search, int offset, int limit, char *err, int en);
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
/* A game kept as a folder of discs: the discs on the console now, [1, 3];
 * NULL for any other game. */
cJSON *library_discs_here(int rom_id);
/* One disc of a game kept as a folder of discs: its files go, the game stays
 * (all of it with its last disc). Saves are kept. */
int library_delete_disc(int rom_id, int disc, char *err, int en);

/* disc: one disc of a game kept as a folder of discs, 0 for all of it.
 * name: the name the client shows; cover: its copy of RomM's path_cover_small. */
int library_queue_rom(int rom_id, int disc, const char *name, const char *cover, char *err, int en);
/* Every game of a collection (or platform) not on the console yet, queued.
 * Games already here, archived or of a platform no emulator plays count as
 * skipped. 0, or -1 with err set. */
int library_queue_scope(const library_scope *scope, int *queued, int *skipped, char *err, int en);
int library_queue_firmware(int platform_id, char *err, int en);
int library_cancel(int id, char *err, int en);
/* A failed or cancelled download, queued again. 0, or -1 with err set. */
int library_retry(int id, char *err, int en);
int library_clear_finished(void);
int library_delete_rom(int rom_id, char *err, int en);
cJSON *library_downloads(void);
int library_busy(void); /* a download is queued or running */

#endif
