#include "ra2.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "detect.h"
#include "library.h"
#include "playlist.h"
#include "profiles.h"
#include "state.h"
#include "sync.h"
#include "util.h"

#define RA_ROM_DIR "{root}/content/{platform}"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void rekey(const char *from, const char *to);
static time_t g_checked;
static cJSON *g_pending;

/* Which folders PS5 RetroArch at root keeps games in, by the major version
 * its webui/version.json gives: 0 before 2.0 (content/<our system name>), 2
 * for 2.x (the libretro database's: content/Sony - PlayStation), 3 for 3.0 and
 * newer (ES-DE's: content/psx, content/gc), which renames 2.x's on its first
 * start. */
int ra2_layout(const char *root) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, root, "webui/version.json");
    char *txt = read_file(p, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const char *release = jget_str(j, "release", ""); /* "v2.0.0-beta-2" */
    int major = 0;
    sscanf(release + (*release == 'v' || *release == 'V'), "%d", &major);
    cJSON_Delete(j);
    return major >= 3 ? 3 : major == 2 ? 2 : 0;
}

int ra2_is_v2(const char *root) { return ra2_layout(root) >= 2; }

/* 2.0 plays PS2 with PS5SX2, not LRPS2: its memory cards are as the core's
 * rule says (platform/retroarch_cores.json), not LRPS2's. */
static int to_ps5sx2(cJSON *p, const char *root) {
    char core[PATH_MAX_LEN];
    path_join(core, sizeof core, root, "cores/ps5sx2_libretro.info");
    if (!file_exists(core)) return 0;
    cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
        if (strcmp(profiles_system_key(e), "ps2") != 0 || !str_ieq(jget_str(e, "core_name", ""), "LRPS2")) continue;
        jset_str(e, "emulator", "ps5sx2");
        jset_str(e, "core_name", "PS5SX2");
        cJSON_DeleteItemFromObjectCaseSensitive(e, "save_exts");
        cJSON_DeleteItemFromObjectCaseSensitive(e, "sync_requires_option");
        detect_apply_core_rules(e, "ps2", "PS5SX2", 1);
        LOGI("RetroArch 2.0 at %s: PS2 plays with PS5SX2", root);
        return 1;
    }
    return 0;
}

int ra2_update_profile(cJSON *p, cJSON *moves) {
    const char *root = jget_str(p, "root", "");
    /* A RetroArch profile, its games in content/<system>: the preset's. */
    const int layout = jget_str(p, "retroarch_cfg", NULL) && !strcmp(jget_str(p, "rom_dir", ""), RA_ROM_DIR) ? ra2_layout(root) : 0;
    if (!layout) return 0;
    int changed = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(p, "platforms")) {
        const char *db = detect_db_name(e), *esde = detect_esde_name(e);
        const char *name = layout == 3 ? esde : db;
        char now[PATH_MAX_LEN], old[PATH_MAX_LEN], tpl[PATH_MAX_LEN], tpl_db[PATH_MAX_LEN], tpl_esde[PATH_MAX_LEN];
        if (!name) continue;
        snprintf(now, sizeof now, "%s/content/%s", root, name);
        if (!dir_exists(now)) continue; /* a system RetroArch has no folder for */
        /* Unless the user picked a folder of their own. */
        snprintf(old, sizeof old, "%s/content/%s", root, jget_str(e, "dir", profiles_system_key(e)));
        snprintf(tpl, sizeof tpl, "{root}/content/%s", name);
        snprintf(tpl_db, sizeof tpl_db, "{root}/content/%s", db ? db : "");
        snprintf(tpl_esde, sizeof tpl_esde, "{root}/content/%s", esde ? esde : "");
        const char *set = jget_str(e, "rom_dir", NULL);
        if (set && !strcmp(set, tpl)) continue; /* already */
        if (set && strcmp(set, old) && strcmp(set, RA_ROM_DIR) && strcmp(set, tpl_db) && strcmp(set, tpl_esde)) continue;
        /* 3.0 renamed 2.x's folder itself: our records of the games in it follow. */
        if (moves && set && !strcmp(set, tpl_db) && layout == 3) {
            char was[PATH_MAX_LEN];
            snprintf(was, sizeof was, "%s/content/%s", root, db);
            if (!dir_exists(was)) {
                cJSON *m = cJSON_CreateObject();
                cJSON_AddStringToObject(m, "from", was);
                cJSON_AddStringToObject(m, "to", now);
                cJSON_AddItemToArray(moves, m);
            }
        }
        jset_str(e, "rom_dir", tpl);
        changed++;
    }
    if (changed) LOGI("RetroArch %d.x at %s: %d system%s get their new folders", layout, root, changed, changed == 1 ? "" : "s");
    return changed + to_ps5sx2(p, root);
}

/* A game folder a download is still filling (its files arrive as .part). */
static int still_downloading(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int part = 0;
    while (d && !part && (de = readdir(d))) part = ext_in_list(de->d_name, ".part");
    if (d) closedir(d);
    return part;
}

/* What moves: everything but the downloads still arriving. */
static int movable(const char *dir, const char *name) {
    if (name[0] == '.' || ext_in_list(name, ".part,.tmp")) return 0;
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, dir, name);
    return !(dir_exists(p) && still_downloading(p));
}

/* Games in from that aren't in to yet. */
static int games_to_move(const char *from, const char *to, const profile_map *m) {
    DIR *d = opendir(from);
    struct dirent *de;
    int n = 0;
    while (d && (de = readdir(d))) {
        if (!movable(from, de->d_name) || ext_in_list(de->d_name, m->save_exts) ||
            ext_in_list(de->d_name, m->state_exts) || library_not_game(de->d_name))
            continue;
        char there[PATH_MAX_LEN];
        path_join(there, sizeof there, to, de->d_name);
        if (!file_exists(there) && !dir_exists(there)) n++;
    }
    if (d) closedir(d);
    return n;
}

void ra2_check(int now) {
    pthread_mutex_lock(&g_lock);
    const char *every = getenv("ROMM_SYNC_RA2_EVERY"); /* seconds, for tests */
    if (!now && time(NULL) - g_checked < (every && *every ? atoi(every) : 60)) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_checked = time(NULL);
    config_lock();
    int changed = 0;
    cJSON *p, *moves = cJSON_CreateArray();
    cJSON_ArrayForEach(p, g_cfg.profiles) changed += ra2_update_profile(p, moves);
    if (changed && g_cfg.profiles_custom) config_save();
    profile_map *maps = NULL;
    int n = profiles_expand(g_cfg.profiles, &maps);
    const int ready = g_cfg.setup_complete;
    config_unlock();
    const cJSON *mv;
    cJSON_ArrayForEach(mv, moves) {
        state_lock();
        rekey(jget_str(mv, "from", ""), jget_str(mv, "to", ""));
        state_save();
        state_unlock();
        LOGI("RetroArch moved %s to %s: the games' records follow", jget_str(mv, "from", ""), jget_str(mv, "to", ""));
    }
    cJSON_Delete(moves);

    cJSON_Delete(g_pending);
    g_pending = NULL;
    cJSON *folders = cJSON_CreateArray();
    int total = 0;
    for (int i = 0; ready && i < n; i++) {
        const profile_map *m = &maps[i];
        char content[PATH_MAX_LEN], from[PATH_MAX_LEN];
        snprintf(content, sizeof content, "%s/content/", m->root);
        const int layout = strncmp(m->rom_dir, content, strlen(content)) == 0 ? ra2_layout(m->root) : 0;
        if (!layout) continue;
        /* The games still in an older layout's folder: ours (content/<system>),
         * and with 3.0, 2.x's (content/<libretro database>) when it left one. */
        char sys[64];
        str_copy(sys, sizeof sys, m->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        cJSON *probe = cJSON_CreateObject();
        cJSON *slugs = cJSON_AddArrayToObject(probe, "romm");
        cJSON_AddItemToArray(slugs, cJSON_CreateString(sys));
        const char *db = layout == 3 ? detect_db_name(probe) : NULL;
        char olds[2][PATH_MAX_LEN];
        int no = 0;
        snprintf(olds[no++], PATH_MAX_LEN, "%s%s", content, m->platform_dir);
        if (db) snprintf(olds[no++], PATH_MAX_LEN, "%s%s", content, db);
        cJSON_Delete(probe);
        for (int k = 0; k < no; k++) {
            str_copy(from, sizeof from, olds[k]);
            int seen = 0;
            const cJSON *f;
            cJSON_ArrayForEach(f, folders) seen |= !strcmp(jget_str(f, "from", ""), from);
            const int games = !seen && strcmp(from, m->rom_dir) != 0 ? games_to_move(from, m->rom_dir, m) : 0;
            if (!games) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "from", from);
            cJSON_AddStringToObject(o, "to", m->rom_dir);
            cJSON_AddStringToObject(o, "root", m->root);
            char playlists[PATH_MAX_LEN] = "";
            profile_ra_dir(m, "playlist_directory", playlists, sizeof playlists);
            cJSON_AddStringToObject(o, "playlists", playlists);
            cJSON_AddNumberToObject(o, "games", games);
            cJSON_AddItemToArray(folders, o);
            total += games;
        }
    }
    if (total) {
        g_pending = cJSON_CreateObject();
        cJSON_AddNumberToObject(g_pending, "games", total);
        cJSON_AddItemToObject(g_pending, "folders", folders);
    } else {
        cJSON_Delete(folders);
    }
    free(maps);
    pthread_mutex_unlock(&g_lock);
}

cJSON *ra2_pending(void) {
    pthread_mutex_lock(&g_lock);
    cJSON *j = g_pending ? cJSON_Duplicate(g_pending, 1) : NULL;
    pthread_mutex_unlock(&g_lock);
    return j;
}

/* Records under from (games, and saves kept beside them) follow it to to. */
static void rekey(const char *from, const char *to) {
    static const char *const sections[] = {"roms", "saves", "states", "conflicts"};
    size_t fl = strlen(from);
    for (size_t s = 0; s < sizeof sections / sizeof *sections; s++) {
        cJSON *sec = state_section(sections[s]), *found = cJSON_CreateArray(), *e = sec->child;
        while (e) {
            cJSON *next = e->next;
            if (!strncmp(e->string, from, fl) && (!e->string[fl] || e->string[fl] == '/' || e->string[fl] == '#'))
                cJSON_AddItemToArray(found, cJSON_DetachItemViaPointer(sec, e));
            e = next;
        }
        while ((e = cJSON_DetachItemFromArray(found, 0))) {
            char key[PATH_MAX_LEN + 256];
            snprintf(key, sizeof key, "%s%s", to, e->string + fl);
            cJSON_DeleteItemFromObjectCaseSensitive(sec, key); /* an old record of something gone */
            cJSON_AddItemToObject(sec, key, e);
        }
        cJSON_Delete(found);
    }
}

/* Moves what's in from to to; how many failed. */
static int move_folder(const char *from, const char *to, int *moved) {
    int failed = 0;
    mkdir_p(to);
    DIR *d = opendir(from);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!movable(from, de->d_name)) continue;
        char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
        path_join(src, sizeof src, from, de->d_name);
        path_join(dst, sizeof dst, to, de->d_name);
        if (file_exists(dst) || dir_exists(dst)) continue; /* already there: left alone */
        if (rename(src, dst) != 0) {
            LOGW("could not move %s to %s", src, dst);
            failed++;
            continue;
        }
        state_lock();
        rekey(src, dst);
        state_unlock();
        (*moved)++;
    }
    if (d) closedir(d);
    rmdir(from); /* only once it's empty */
    LOGI("moved %s to %s%s", from, to, failed ? " (some failed)" : "");
    return failed;
}

int ra2_move_games(int *moved, char *err, int en) {
    *moved = 0;
    ra2_check(1);
    cJSON *pending = ra2_pending();
    if (!pending) {
        snprintf(err, (size_t)en, "no games to move");
        return -1;
    }
    int failed = 0;
    sync_pause();
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(pending, "folders")) {
        failed += move_folder(jget_str(f, "from", ""), jget_str(f, "to", ""), moved);
        /* RetroArch's history and playlists follow the games. */
        playlists_move(jget_str(f, "root", ""), jget_str(f, "playlists", ""), jget_str(f, "from", ""), jget_str(f, "to", ""));
    }
    state_lock();
    state_save();
    state_unlock();
    sync_resume();
    cJSON_Delete(pending);
    LOGI("moved %d game%s to RetroArch's new folders", *moved, *moved == 1 ? "" : "s");
    ra2_check(1);
    if (*moved) sync_request("games moved");
    if (failed) snprintf(err, (size_t)en, "%d couldn't be moved", failed);
    return failed ? -1 : 0;
}
