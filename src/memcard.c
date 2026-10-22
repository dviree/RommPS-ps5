#include "memcard.h"

#include "events.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"
#include "zip.h"

/* Shared memory cards: the ones an emulator lists in "memcards" (PS5SX2's
 * Mcd001.ps2 and Mcd002.ps2, the Flycasts' vmu_save_A1.bin...), while their
 * system uses shared cards (profiles.c, card_methods).
 *
 * A card on RomM is one card for every console that keeps it: emulator
 * "rommps-<system>", named after the file ("Mcd001.ps2"; RetroArch's cores
 * "RetroArch Mcd001.ps2"), so PS4SX2 on a PS4 and PS5SX2 on a PS5 play on the
 * same card, and RetroArch's PS5SX2 on its own. It syncs both ways: the side that changed goes to the
 * other; when both did, the newer wins and the other stays (this console's in
 * backups/memcards, RomM's in the card's history). A card isn't replaced
 * while a game runs. RomM takes cards as zips: each holds the one file. */
#define MAX_CARDS 16

typedef struct {
    char path[PATH_MAX_LEN];
    char key[48];      /* RomM's emulator field: "rommps-ps2" */
    char name[64];     /* the card's name on RomM: the file's ("Mcd001.ps2") */
    char label[64];    /* the emulator, for the UIs */
} card;

static int cards_list(card *out, int max) {
    config_lock();
    profile_map *maps = NULL;
    int cnt = profiles_expand(g_cfg.profiles, &maps);
    config_unlock();
    int n = 0;
    for (int i = 0; i < cnt && n < max; i++) {
        const profile_map *m = &maps[i];
        if (m->sync_off || !m->memcards[0]) continue;
        /* Only while the system uses shared cards and this emulator keeps them. */
        if (m->cards_mode[0] && (!m->cards_active || strcmp(m->cards_mode, "shared") != 0)) continue;
        char sys[64];
        str_copy(sys, sizeof sys, m->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        char list[sizeof m->memcards], *save = NULL;
        str_copy(list, sizeof list, m->memcards);
        for (char *t = strtok_r(list, ",", &save); t && n < max; t = strtok_r(NULL, ",", &save)) {
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= !strcmp(out[k].path, t);
            if (dup) continue;
            card *c = &out[n++];
            str_copy(c->path, sizeof c->path, t);
            snprintf(c->key, sizeof c->key, "rommps-%s", sys);
            /* RetroArch's cores apart from the standalone emulators: two
             * different emulators on one console never share a card unasked. */
            snprintf(c->name, sizeof c->name, "%s%s", m->core_name[0] ? "RetroArch " : "", path_basename(t));
            str_copy(c->label, sizeof c->label, m->profile_name);
        }
    }
    free(maps);
    return n;
}

int memcard_enabled(void) {
    card cards[MAX_CARDS];
    return config_server_at_least(5, 3, 0) && cards_list(cards, MAX_CARDS) > 0;
}

int memcard_dirs(char dirs[][1024], int max) {
    if (max < 1 || !memcard_enabled()) return 0;
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS), nd = 0;
    for (int i = 0; i < n && nd < max; i++) {
        char dir[1024];
        str_copy(dir, sizeof dir, cards[i].path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        int dup = 0;
        for (int k = 0; k < nd; k++) dup |= !strcmp(dirs[k], dir);
        if (!dup) str_copy(dirs[nd++], 1024, dir);
    }
    return nd;
}

/* "rommps-ps2|Mcd001.ps2": what a state entry's card was found as. Entries
 * from before 1.3 have none: their card was this console's own. */
static void card_identity(const card *cd, char *out, size_t n) { snprintf(out, n, "%s|%s", cd->key, cd->name); }

/* The card's id on RomM, 0 if it has none yet (create: make it). */
static int find_card(const card *cd, int create) {
    char api[128];
    long st = 0;
    char *q = http_escape(cd->key);
    snprintf(api, sizeof api, "/api/memory-cards?emulator=%s", q);
    free(q);
    cJSON *list = romm_call("GET", api, NULL, &st);
    int id = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, list) if (!id && !strcmp(jget_str(c, "name", ""), cd->name)) id = (int)jget_num(c, "id", 0);
    cJSON_Delete(list);
    if (id || !create) return id;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", cd->name);
    cJSON_AddStringToObject(body, "emulator", cd->key);
    cJSON *made = romm_call("POST", "/api/memory-cards", body, &st);
    cJSON_Delete(body);
    id = (st == 200 || st == 201) ? (int)jget_num(made, "id", 0) : 0;
    cJSON_Delete(made);
    if (id) LOGI("created memory card \"%s\" (%s) on RomM", cd->name, cd->key);
    return id;
}

/* The card's newest version on RomM that can be downloaded: 0 if none, -1 if
 * the card is gone (404), -2 if RomM couldn't be asked. Newest is the highest
 * id, whatever order RomM lists them in. */
static int latest_version(int card_id, time_t *at) {
    char api[96];
    long st = 0;
    snprintf(api, sizeof api, "/api/memory-cards/%d/versions", card_id);
    cJSON *list = romm_call("GET", api, NULL, &st);
    int id = st == 404 ? -1 : st == 200 ? 0 : -2;
    const cJSON *v;
    cJSON_ArrayForEach(v, list) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "missing_from_fs"))) continue;
        const int vid = (int)jget_num(v, "id", 0);
        if (vid <= id) continue;
        id = vid;
        *at = iso8601_parse(jget_str(v, "created_at", ""));
    }
    cJSON_Delete(list);
    return id;
}

static uint32_t le16(const unsigned char *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* A card file that's one byte over and over: one never formatted (PCSX2
 * makes them all 0xFF), or zeroed. */
static int unformatted(FILE *fp) {
    unsigned char b[65536];
    int first = -1;
    size_t n;
    rewind(fp);
    while ((n = fread(b, 1, sizeof b, fp)) > 0)
        for (size_t i = 0; i < n; i++) {
            if (first < 0) first = b[i];
            if (b[i] != first || (first != 0xff && first != 0)) return 0;
        }
    return first >= 0;
}

/* How many saves a card holds: 0 for a freshly formatted or never formatted
 * one, -1 when it's no card we can read (then it's never called blank). PS1
 * cards, PS2 cards (with or without ECC) and Dreamcast VMUs. */
static int card_saves(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    unsigned char b[2048];
    const int64_t size = file_size(path);
    int saves = -1;
    size_t n = fread(b, 1, sizeof b, fp);
    if (n == sizeof b && b[0] == 'M' && b[1] == 'C' && size == 128 * 1024) {
        /* PS1: directory frames 1-15, 0x51 starts a save */
        saves = 0;
        for (int i = 1; i < 16; i++) saves += b[i * 128] == 0x51;
    } else if (n >= 0x40 && !memcmp(b, "Sony PS2 Memory Card Format ", 28)) {
        /* PS2: the root directory's "." entry counts its entries, "." and ".." included */
        const uint32_t page = le16(b + 0x28), ppc = le16(b + 0x2a), clusters = le32(b + 0x30);
        const uint32_t alloc = le32(b + 0x34), root = le32(b + 0x3c);
        const int64_t raw = (int64_t)clusters * ppc * page, ecc = (int64_t)clusters * ppc * (page + page / 32);
        const int64_t stride = size == ecc ? page + page / 32 : size == raw ? page : 0;
        unsigned char e[8];
        if (page == 512 && ppc && stride && fseek(fp, (long)((int64_t)(alloc + root) * ppc * stride), SEEK_SET) == 0 &&
            fread(e, 1, sizeof e, fp) == sizeof e && (le16(e) & 0x8020) == 0x8020)
            saves = le32(e + 4) >= 2 ? (int)le32(e + 4) - 2 : 0;
    } else if (size == 128 * 1024) {
        /* Dreamcast VMU: the root block (255) says where the directory is;
         * each 32-byte entry's first byte is 0 when unused */
        unsigned char rb[512], dir[512];
        if (fseek(fp, 255 * 512, SEEK_SET) == 0 && fread(rb, 1, sizeof rb, fp) == sizeof rb) {
            const uint32_t at = le16(rb + 0x4a), blocks = le16(rb + 0x4c);
            if (at < 255 && blocks && blocks <= at + 1 && blocks <= 16) {
                saves = 0;
                for (uint32_t k = 0; k < blocks && saves >= 0; k++) {
                    if (fseek(fp, (long)(at - k) * 512, SEEK_SET) != 0 || fread(dir, 1, sizeof dir, fp) != sizeof dir) saves = -1;
                    for (int i = 0; i < 16 && saves >= 0; i++) saves += dir[i * 32] != 0;
                }
            }
        }
    }
    if (saves < 0 && unformatted(fp)) saves = 0;
    fclose(fp);
    return saves;
}

static void remember(const card *cd, int card_id, const char *hash, int version_id) {
    char ident[128];
    card_identity(cd, ident, sizeof ident);
    state_lock();
    cJSON *s = state_entry_ensure("memcards", cd->path);
    jset_num(s, "card_id", card_id);
    jset_str(s, "card", ident);
    jset_str(s, "hash", hash);
    jset_num(s, "version_id", version_id);
    jset_num(s, "synced_at", (double)time(NULL));
    state_save();
    state_unlock();
}

/* This console's card goes up as a new version. The version's id, or -1. */
static int upload_card(const card *cd, int card_id) {
    char zip[PATH_MAX_LEN], api[128], entry[PATH_MAX_LEN];
    snprintf(zip, sizeof zip, "%s/tmp/card-%d.zip", plat_data_dir(), card_id);
    mkdir_parent(zip);
    str_copy(entry, sizeof entry, cd->path);
    const char *files[] = {entry};
    if (zip_store_files(zip, files, 1) != 0) {
        LOGE("could not pack %s", cd->path);
        return -1;
    }
    snprintf(api, sizeof api, "/api/memory-cards/%d/versions", card_id);
    long st = 0;
    cJSON *v = romm_upload("POST", api, "cardFile", zip, path_basename(zip), &st);
    unlink(zip);
    int id = (st == 200 || st == 201) ? (int)jget_num(v, "id", 0) : -1;
    cJSON_Delete(v);
    return id;
}

static void backup_local(const char *path) {
    if (!file_exists(path)) return;
    char bak[PATH_MAX_LEN], ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    snprintf(bak, sizeof bak, "%s/backups/memcards/%s.%s", plat_data_dir(), path_basename(path), ts);
    mkdir_parent(bak);
    copy_file(path, bak);
}

/* RomM's version, fetched into tmp/card-v<id>/ (drop it with fetched_drop):
 * got is the card in it, written when the card was last written (0 if the
 * zip doesn't say). 0, or -1 with err set. */
static int fetch_card(int version_id, char *got, size_t gn, time_t *written, char *err, int en) {
    char api[128], zip[PATH_MAX_LEN], dir[PATH_MAX_LEN];
    snprintf(api, sizeof api, "/api/memory-cards/versions/%d/content", version_id);
    snprintf(zip, sizeof zip, "%s/tmp/card-v%d.zip", plat_data_dir(), version_id);
    snprintf(dir, sizeof dir, "%s/tmp/card-v%d", plat_data_dir(), version_id);
    got[0] = 0;
    *written = 0;
    mkdir_parent(zip);
    http_resp r;
    if (romm_download(api, zip, NULL, NULL, &r) != 0) {
        snprintf(err, (size_t)en, r.status == 404 ? "that card has no data on RomM" : "download failed");
        unlink(zip);
        return -1;
    }
    /* The one file in it, whatever another emulator named it. */
    remove_tree(dir);
    mkdir_p(dir);
    int rc = zip_extract_all(zip, dir); /* how many files it wrote */
    *written = zip_first_time(zip);
    unlink(zip);
    DIR *d = rc > 0 ? opendir(dir) : NULL;
    struct dirent *de;
    while (d && (de = readdir(d)))
        if (de->d_name[0] != '.' && !got[0]) path_join(got, gn, dir, de->d_name);
    if (d) closedir(d);
    /* A card is the same file wherever it's kept (its RomM name is the file's,
     * under its system): only an empty one is refused. */
    if (!got[0] || file_size(got) <= 0) {
        snprintf(err, (size_t)en, "RomM's copy of the card is empty");
        remove_tree(dir);
        return -1;
    }
    return 0;
}

static void fetched_drop(const char *got) {
    char dir[PATH_MAX_LEN];
    str_copy(dir, sizeof dir, got);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    remove_tree(dir);
}

/* A fetched card replaces this console's (kept in backups first). 0, or -1
 * with err set. */
static int install_card(const card *cd, const char *got, char *err, int en) {
    char tmp[PATH_MAX_LEN];
    backup_local(cd->path);
    snprintf(tmp, sizeof tmp, "%s.rommps", cd->path);
    mkdir_parent(cd->path);
    const int rc = copy_file(got, tmp) == 0 && rename(tmp, cd->path) == 0 ? 0 : -1;
    fetched_drop(got);
    if (rc) {
        unlink(tmp);
        snprintf(err, (size_t)en, "could not write %s", cd->path);
    }
    return rc;
}

static int download_card(const card *cd, int version_id, char *err, int en) {
    char got[PATH_MAX_LEN];
    time_t written;
    if (fetch_card(version_id, got, sizeof got, &written, err, en) != 0) return -1;
    return install_card(cd, got, err, en);
}

/* One card, both ways. 1 if one went up, 2 if one came down, 0 for nothing,
 * -1 on an error. upload_only: a game runs and may have the card open. */
static int sync_card(const card *cd, int upload_only) {
    char ident[128], hash[33] = "", err[128];
    card_identity(cd, ident, sizeof ident);
    const int here = file_exists(cd->path) && md5_file_hex(cd->path, hash) == 0;
    state_lock();
    cJSON *e = state_entry("memcards", cd->path);
    const int same_card = e && !strcmp(jget_str(e, "card", ""), ident);
    int card_id = same_card ? (int)jget_num(e, "card_id", 0) : 0;
    int synced_version = same_card ? (int)jget_num(e, "version_id", 0) : 0;
    char synced_hash[33] = "";
    if (same_card) str_copy(synced_hash, sizeof synced_hash, jget_str(e, "hash", ""));
    state_unlock();

    if (!card_id) card_id = find_card(cd, 0);
    time_t remote_at = 0;
    int remote = card_id ? latest_version(card_id, &remote_at) : 0;
    if (remote == -2) return -1;
    if (remote == -1) card_id = 0, remote = 0; /* deleted on RomM: made again below */

    /* Versions only count up: an older one (ours went missing on RomM) is no news. */
    const int local_changed = here && strcmp(hash, synced_hash) != 0;
    const int remote_changed = remote > synced_version;
    const int both = local_changed && remote_changed; /* also every card's first sync since 1.3 */
    /* A card wiped or made fresh here comes back from RomM instead of going up. */
    const int blank_here = here && card_saves(cd->path) == 0;
    int up = local_changed && !remote_changed && !(blank_here && remote > 0);
    int down = remote > 0 && !up && (remote_changed || (local_changed && blank_here));
    /* Not while any game runs, the sync at its start included: an emulator
     * that has the card open keeps writing to the file it opened, and one
     * swapped in under it would lose everything saved after. (Waking from
     * rest also looks like a game starting.) */
    if (down && (upload_only || sync_game_running())) return 0;
    if (down && !here) {
        /* A card this console doesn't have yet, for an emulator that's here. */
        char parent[PATH_MAX_LEN];
        str_copy(parent, sizeof parent, cd->path);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        if (!dir_exists(parent)) return 0;
    }
    char got[PATH_MAX_LEN] = "";
    if (down) {
        /* RomM's copy first, to see what it is. */
        time_t written = 0;
        if (fetch_card(remote, got, sizeof got, &written, err, sizeof err) != 0) {
            LOGE("memory card %s: %s", cd->path, err);
            return -1;
        }
        char theirs[33] = "";
        md5_file_hex(got, theirs);
        if (here && !strcmp(theirs, hash)) { /* the same card: nothing to move */
            fetched_drop(got);
            remember(cd, card_id, hash, remote);
            return 0;
        }
        if (here) {
            /* A blank card never beats one with saves; otherwise the one
             * written last wins (zips older than 1.3 only say when they went up). */
            const int mine = card_saves(cd->path), theirs_n = card_saves(got);
            if (mine == 0 && theirs_n > 0) down = 1;
            else if (theirs_n == 0 && mine > 0) down = 0;
            else if (both) down = (written ? written : remote_at) > file_mtime(cd->path);
            if (both) LOGI("memory card %s changed here and on RomM: keeping %s", cd->name, down ? "RomM's" : "this console's");
            else if (!down) LOGI("memory card %s: RomM's copy is blank, this console's goes up instead", cd->name);
            up = !down;
        }
        if (!down) fetched_drop(got);
    }
    if (up) {
        if (!card_id) card_id = find_card(cd, 1);
        int v = card_id ? upload_card(cd, card_id) : -1;
        if (v < 0) {
            LOGE("could not upload memory card %s", cd->path);
            return -1;
        }
        remember(cd, card_id, hash, v);
        LOGI("memory card %s up to RomM", cd->path);
        events_add("card", "uploaded", 0, cd->path, cd->label, NULL);
        return 1;
    }
    if (down) {
        if (install_card(cd, got, err, sizeof err) != 0) {
            LOGE("memory card %s: %s", cd->path, err);
            return -1;
        }
        md5_file_hex(cd->path, hash);
        remember(cd, card_id, hash, remote);
        LOGI("memory card %s down from RomM", cd->path);
        events_add("card", "downloaded", 0, cd->path, cd->label, NULL);
        return 2;
    }
    /* Nothing to move; a card from before 1.3 is now this one. */
    if (here && card_id && !same_card) remember(cd, card_id, hash, remote);
    return 0;
}

int memcard_sync(int upload_only, int *down) {
    *down = 0;
    if (!memcard_enabled()) return 0;
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS), up = 0;
    for (int i = 0; i < n; i++) {
        int r = sync_card(&cards[i], upload_only);
        if (r == 1) up++;
        if (r == 2) (*down)++;
    }
    return up;
}

int memcard_restore(int slot, char *err, int en) {
    card cards[MAX_CARDS];
    int n = memcard_enabled() ? cards_list(cards, MAX_CARDS) : 0;
    if (!n) {
        snprintf(err, (size_t)en, "no shared memory cards sync here");
        return -1;
    }
    if (slot < 1 || slot > n) {
        snprintf(err, (size_t)en, "no such slot");
        return -1;
    }
    if (sync_game_running()) {
        snprintf(err, (size_t)en, "close the emulator before restoring a memory card");
        return -1;
    }
    const card *cd = &cards[slot - 1];
    int card_id = find_card(cd, 0);
    time_t at = 0;
    int version = card_id ? latest_version(card_id, &at) : 0;
    if (version <= 0) {
        snprintf(err, (size_t)en, "no copy of it on RomM yet");
        return -1;
    }
    if (download_card(cd, version, err, en) != 0) return -1;
    char hash[33];
    md5_file_hex(cd->path, hash);
    remember(cd, card_id, hash, version);
    LOGI("restored memory card %s from RomM", path_basename(cd->path));
    events_add("card", "restored", 0, cd->path, cd->label, "Latest version");
    events_save();
    return 0;
}

cJSON *memcard_status(void) {
    char mode[16], version[32];
    config_lock();
    str_copy(mode, sizeof mode, g_cfg.ps2_cards);
    str_copy(version, sizeof version, g_cfg.server_version);
    profile_map *maps = NULL;
    int cnt = profiles_expand(g_cfg.profiles, &maps);
    config_unlock();
    /* PS2's method, and whether its emulators here let the user choose. */
    const char *method = "", *choice = NULL;
    for (int i = 0; i < cnt; i++) {
        if (!maps[i].cards_mode[0] || !profile_map_handles(&maps[i], "ps2", NULL)) continue;
        if (!method[0]) method = maps[i].cards_method;
        if (strchr(maps[i].cards_modes, ',')) choice = maps[i].profile_name;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", method[0] ? method : mode);
    cJSON_AddStringToObject(j, "chosen", mode);
    cJSON_AddStringToObject(j, "server_version", version);
    cJSON_AddBoolToObject(j, "supported", config_server_at_least(5, 3, 0));
    /* An emulator that keeps either: PS2's method can be chosen (lrps2: 1.2's name). */
    cJSON_AddBoolToObject(j, "choice", choice != NULL);
    cJSON_AddBoolToObject(j, "lrps2", choice != NULL);
    free(maps);
    card cards[MAX_CARDS];
    int n = cards_list(cards, MAX_CARDS);
    cJSON_AddBoolToObject(j, "enabled", n > 0 && config_server_at_least(5, 3, 0));
    cJSON *slots = cJSON_AddArrayToObject(j, "slots");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i + 1);
        cJSON_AddStringToObject(o, "emulator", cards[i].label);
        cJSON_AddStringToObject(o, "name", cards[i].name);
        cJSON_AddStringToObject(o, "path", cards[i].path);
        cJSON_AddBoolToObject(o, "exists", file_exists(cards[i].path));
        state_lock();
        cJSON *e = state_entry("memcards", cards[i].path);
        double at = e ? jget_num(e, "synced_at", jget_num(e, "backed_up_at", 0)) : 0;
        state_unlock();
        cJSON_AddNumberToObject(o, "synced_at", at);
        cJSON_AddNumberToObject(o, "backed_up_at", at);
        cJSON_AddItemToArray(slots, o);
    }
    return j;
}
