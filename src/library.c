#include "library.h"
#include "sync.h"
#include "covers.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "events.h"
#include "firmware.h"
#include "http.h"
#include "platform.h"
#include "profiles.h"
#include "romm.h"
#include "state.h"
#include "util.h"

#define MAX_WORKERS 8
#define MAX_JOBS    50

typedef enum { JOB_ROM, JOB_FIRMWARE } job_type;
typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED, JOB_SYNCING } job_status;

typedef struct job {
    int id;
    job_type type;
    int target_id; /* rom id or platform id */
    char name[256];
    char cover[160]; /* RomM's path_cover_small, for the queue row */
    char platform[64];
    job_status status;
    int cancel;
    char error[256];
    int64_t done, total;
    int64_t base; /* bytes from files already finished (multi-file games) */
    double speed; /* bytes/s, smoothed */
    char note[80]; /* Running job updates this after a download. */
    sync_game_result saves; /* what the sync after the download brought */
    double sample_t;
    int64_t sample_bytes;
    struct job *next;
} job;

static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cond = PTHREAD_COND_INITIALIZER;
static job *g_jobs; /* newest first */
static int g_job_seq, g_running;

static int finished(const job *j) { return j->status == JOB_DONE || j->status == JOB_ERROR || j->status == JOB_CANCELLED; }

static job *oldest_queued(void) {
    job *pick = NULL;
    for (job *j = g_jobs; j; j = j->next)
        if (j->status == JOB_QUEUED) pick = j;
    return pick;
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A game came or went through us: the installed fingerprint (library_installed_sig) is worked out again. */
static volatile int g_sig_stale = 1;
static void installed_changed(void) { g_sig_stale = 1; }

static int progress_cb(void *ud, int64_t done, int64_t total) {
    job *j = ud;
    double now = mono_now();
    pthread_mutex_lock(&g_q_lock);
    j->done = j->base + done;
    if (total > 0 && j->base + total > j->total) j->total = j->base + total;
    if (j->sample_t == 0) {
        j->sample_t = now;
        j->sample_bytes = j->done;
    } else if (now - j->sample_t >= 1.0) {
        double inst = (double)(j->done - j->sample_bytes) / (now - j->sample_t);
        j->speed = j->speed > 0 ? j->speed * 0.6 + inst * 0.4 : inst;
        j->sample_t = now;
        j->sample_bytes = j->done;
    }
    int stop = j->cancel;
    pthread_mutex_unlock(&g_q_lock);
    return stop;
}

static int cancelled(job *j) {
    pthread_mutex_lock(&g_q_lock);
    int c = j->cancel;
    pthread_mutex_unlock(&g_q_lock);
    return c;
}

static void job_end(job *j, job_status st, const char *fmt, const char *arg) {
    pthread_mutex_lock(&g_q_lock);
    j->status = j->cancel ? JOB_CANCELLED : st;
    if (fmt && j->status == JOB_ERROR) snprintf(j->error, sizeof j->error, fmt, arg ? arg : "");
    if (j->status == JOB_DONE) j->done = j->total;
    pthread_mutex_unlock(&g_q_lock);
    if (j->status == JOB_ERROR) LOGE("download of %s failed: %s", j->name, j->error);
    if (j->status == JOB_CANCELLED) LOGI("download of %s cancelled", j->name);
}

static int load_maps(profile_map **maps) {
    config_lock();
    int n = profiles_expand(g_cfg.profiles, maps);
    config_unlock();
    return n;
}

/* Files a RomM scan picks up beside the games (a frontend's metadata.txt or
 * gamelist.xml, artwork, readmes, checksums) aren't games: the library hides
 * them and they aren't downloaded. Not .md: that's a Mega Drive ROM. */
#define NOT_GAME_EXTS                                                                                                  \
    ".txt,.nfo,.diz,.pdf,.htm,.html,.xml,.json,.ini,.log,.url,.lnk,.jpg,.jpeg,.png,.gif,.bmp,.webp,.mp4,.mkv,"        \
    ".mp3,.flac,.ogg,.sfv,.md5,.sha1,.crc,.db"

/* Inside a game's folder fewer are left out, since a game can need its own
 * .xml, .ini or .db: only readmes, artwork and checksums. */
#define NOT_NEEDED_EXTS ".txt,.nfo,.diz,.pdf,.url,.lnk,.jpg,.jpeg,.png,.gif,.bmp,.webp,.sfv,.md5,.sha1"

int library_not_game(const char *fs_name) { return ext_in_list(fs_name, NOT_GAME_EXTS); }

/* A file in a game's folder that playing it doesn't need: RomM's manuals,
 * soundtracks, screenshots, walkthroughs and cheats, and the files above. */
static int extra_file(const cJSON *f) {
    const char *cat = jget_str(f, "category", "");
    static const char *skip[] = {"manual", "walkthrough", "soundtrack", "screenshot", "cheat"};
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++)
        if (!strcmp(cat, skip[i])) return 1;
    return ext_in_list(jget_str(f, "file_name", ""), NOT_NEEDED_EXTS);
}

/* Archives nothing on the console plays from: a .rar anywhere, and any
 * archive for a Switch game (RetroArch opens a .zip or .7z itself). The
 * game's archive's extension, or NULL. */
#define ARCHIVED_MSG "This download is archived (%s). Decompress it on your RomM server first."
static const char *rom_archive(const cJSON *rom, int nx) {
    const char *list = nx ? ".rar,.zip,.7z" : ".rar";
    const char *fs_name = jget_str(rom, "fs_name", "");
    if (ext_in_list(fs_name, list)) return path_ext(fs_name);
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files"))
        if (!extra_file(f) && ext_in_list(jget_str(f, "file_name", ""), list)) return path_ext(jget_str(f, "file_name", ""));
    return NULL;
}

/* Switch games come with their updates and DLC, which RomM keeps as games of
 * their own or as files in a game's folder. Each file names its title id
 * ("Game [0100000011D90800][v393216].nsp"): a game's ends in 000, its
 * update's is the same + 0x800 and its DLC's + 0x1001 on, so clearing the
 * low 13 bits gives the game's. The game goes in rom_dir, the rest in
 * update_dir and dlc_dir, and the library shows only the game. */
typedef enum { NX_NONE, NX_GAME, NX_UPDATE, NX_DLC } nx_kind;
#define NX_MAX_FILES 64

static int is_switch(const char *slug) { return !strcmp(slug, "switch"); }

static nx_kind nx_title(const char *name, unsigned long long *game, unsigned long *version) {
    nx_kind kind = NX_NONE;
    *game = 0;
    for (const char *p = strchr(name, '['); p && kind == NX_NONE; p = strchr(p + 1, '[')) {
        if (strncmp(p + 1, "01", 2) != 0 || strspn(p + 1, "0123456789abcdefABCDEF") != 16) continue;
        char hex[17];
        memcpy(hex, p + 1, 16);
        hex[16] = 0;
        const unsigned long long id = strtoull(hex, NULL, 16);
        const unsigned low = (unsigned)(id & 0x1FFF);
        kind = low == 0 ? NX_GAME : low == 0x800 ? NX_UPDATE : low > 0x1000 ? NX_DLC : NX_NONE;
        if (kind != NX_NONE) *game = id & ~0x1FFFULL;
    }
    if (version) {
        *version = 0;
        for (const char *v = strstr(name, "[v"); v; v = strstr(v + 1, "[v"))
            if (v[2] >= '0' && v[2] <= '9') {
                *version = strtoul(v + 2, NULL, 10);
                break;
            }
    }
    return kind;
}

/* A file of a game's folder: by its title id, else by RomM's category. */
static nx_kind nx_file_kind(const char *name, const char *category) {
    unsigned long long game;
    nx_kind k = nx_title(name, &game, NULL);
    if (k != NX_NONE) return k;
    return !strcmp(category, "update") ? NX_UPDATE : !strcmp(category, "dlc") ? NX_DLC : NX_GAME;
}

/* The game a RomM entry is: the title id in its name, else the game's own
 * file's in its folder, else any file's. */
static nx_kind nx_rom(const cJSON *rom, unsigned long long *game) {
    nx_kind k = nx_title(jget_str(rom, "fs_name", ""), game, NULL);
    unsigned long long id;
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
        if (k == NX_GAME) break;
        const nx_kind fk = nx_title(jget_str(f, "file_name", ""), &id, NULL);
        if (fk != NX_NONE && (k == NX_NONE || fk == NX_GAME)) {
            k = fk;
            *game = id;
        }
    }
    return k;
}

static const char *nx_dir(const profile_map *m, nx_kind k) {
    return k == NX_UPDATE ? m->update_dir : k == NX_DLC ? m->dlc_dir : m->rom_dir;
}

/* The update and DLC RomM keeps as games of their own for a game, found by
 * the start of its title id: [{"id", "kind": "update" | "dlc"}], the newest
 * update only. */
typedef struct {
    cJSON *out;
    unsigned long long game;
    int platform_id;
    unsigned long newest;
} nx_find;

static int nx_add_addon(const cJSON *it, const cJSON *page, int first, void *ctx) {
    (void)page, (void)first;
    nx_find *fd = ctx;
    unsigned long long game;
    unsigned long version;
    if (!it || (int)jget_num(it, "platform_id", 0) != fd->platform_id) return 0;
    const nx_kind k = nx_title(jget_str(it, "fs_name", ""), &game, &version);
    if (game != fd->game || (k != NX_UPDATE && k != NX_DLC)) return 0;
    if (k == NX_UPDATE) {
        cJSON *have = NULL, *x;
        cJSON_ArrayForEach(x, fd->out) if (!strcmp(jget_str(x, "kind", ""), "update")) have = x;
        if (have && version <= fd->newest) return 0;
        if (have) cJSON_Delete(cJSON_DetachItemViaPointer(fd->out, have));
        fd->newest = version;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", jget_num(it, "id", 0));
    cJSON_AddStringToObject(o, "kind", k == NX_UPDATE ? "update" : "dlc");
    cJSON_AddStringToObject(o, "name", jget_str(it, "fs_name", ""));
    cJSON_AddNumberToObject(o, "size", jget_num(it, "fs_size_bytes", 0));
    cJSON_AddItemToArray(fd->out, o);
    return 0;
}

static cJSON *nx_addons(int platform_id, unsigned long long game) {
    char path[256];
    snprintf(path, sizeof path, "/api/roms?platform_ids=%d&search_term=%012llX&order_by=name&order_dir=asc" ROMS_LEAN,
             platform_id, game >> 16);
    nx_find fd = {cJSON_CreateArray(), game, platform_id, 0};
    long st = 0;
    if (romm_list(path, 0, 500, nx_add_addon, &fd, &st) < 0) LOGW("could not look for the update and DLC of %016llX (HTTP %ld)", game, st);
    return fd.out;
}

/* GoodTools codes ("!", "M3", "hI", "t1", "b2", "a1", "f1", "o1", "p1", "T+Eng")
 * say nothing a player needs. */
static int readable_tag(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || !strcmp(t, "!")) return 0;
    if (n <= 3 && strchr("Mhtbafop", t[0])) {
        int code = 1;
        for (size_t i = 1; i < n; i++) code &= (t[i] >= '0' && t[i] <= '9') || (t[i] >= 'A' && t[i] <= 'Z') || t[i] == '+';
        if (code) return 0;
    }
    return !(t[0] == 'T' && (t[1] == '+' || t[1] == '-'));
}

static void add_list(char *buf, size_t size, const cJSON *arr, int max) {
    const cJSON *x;
    int n = 0;
    cJSON_ArrayForEach(x, arr) {
        if (!cJSON_IsString(x) || !readable_tag(x->valuestring) || n++ == max) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, size - l, "%s%s", l ? " · " : "", x->valuestring);
    }
}

/* What sets one version of a game apart from the others RomM has for it
 * (its sibling_roms): "USA · Rev 1 · Disc 2". The extension when the file
 * names say nothing more. */
static void version_label(const cJSON *rom, char *buf, size_t size) {
    buf[0] = 0;
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "regions"), 2);
    const char *rev = jget_str(rom, "revision", "");
    if (rev[0]) {
        size_t l = strlen(buf);
        snprintf(buf + l, size - l, "%sRev %s", l ? " · " : "", rev);
    }
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "languages"), 3);
    add_list(buf, size, cJSON_GetObjectItemCaseSensitive(rom, "tags"), 2);
    if (!buf[0]) {
        const char *ext = jget_str(rom, "fs_extension", "");
        for (size_t i = 0; ext[i] && i + 1 < size; i++) buf[i] = (char)(ext[i] >= 'a' && ext[i] <= 'z' ? ext[i] - 32 : ext[i]), buf[i + 1] = 0;
    }
}

static void add_version(cJSON *o, const cJSON *rom) {
    int others = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(rom, "sibling_roms"));
    if (others <= 0) return;
    char label[96];
    version_label(rom, label, sizeof label);
    cJSON_AddStringToObject(o, "version", label);
    cJSON_AddNumberToObject(o, "versions", others + 1);
}

static void discard_partial(const char *dest, int multi) {
    char part[PATH_MAX_LEN];
    if (multi) {
        remove_tree(dest);
        return;
    }
    snprintf(part, sizeof part, "%s.part", dest);
    unlink(part);
}

static void record_rom(job *j, const cJSON *rom, const profile_map *m, const char *dest, int multi) {
    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    char stem[256];
    cJSON *stems = cJSON_CreateArray();
    if (multi) {
        cJSON_AddItemToArray(stems, cJSON_CreateString(fs_name));
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = jget_str(f, "file_name", "");
            if (!ext_in_list(fn, ".cue,.m3u,.gdi,.chd,.ccd,.iso,.pbp")) continue;
            path_stem(stem, sizeof stem, fn);
            cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
        }
    } else {
        path_stem(stem, sizeof stem, fs_name);
        cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
    }
    state_lock();
    cJSON *e = state_entry_ensure("roms", dest);
    jset_num(e, "rom_id", j->target_id);
    jset_str(e, "profile", m->profile_id);
    jset_str(e, "platform_dir", m->platform_dir);
    jset_str(e, "name", jget_str(rom, "name", ""));
    jset_str(e, "fs_name", fs_name);
    jset_str(e, "md5", jget_str(rom, "md5_hash", ""));
    jset_num(e, "platform_id", jget_num(rom, "platform_id", 0));
    jset_str(e, "cover", jget_str(rom, "path_cover_small", ""));
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
    cJSON_DeleteItemFromObjectCaseSensitive(e, "stems");
    cJSON_AddItemToObject(e, "stems", stems);
    state_save();
    state_unlock();
    installed_changed();
}

/* One file of a Switch game, recorded as RomM's game it is. An update or DLC
 * also says which game it came with ("with"): removing that game removes it,
 * and it isn't a game of its own on the console. */
static void record_file(int rom_id, const cJSON *rom, const profile_map *m, const char *dest, int with) {
    char stem[256];
    path_stem(stem, sizeof stem, path_basename(dest));
    state_lock();
    cJSON *e = state_entry_ensure("roms", dest);
    jset_num(e, "rom_id", rom_id);
    jset_str(e, "profile", m->profile_id);
    jset_str(e, "platform_dir", m->platform_dir);
    jset_str(e, "name", jget_str(rom, "name", ""));
    jset_str(e, "fs_name", path_basename(dest));
    jset_num(e, "platform_id", jget_num(rom, "platform_id", 0));
    jset_str(e, "cover", jget_str(rom, "path_cover_small", ""));
    if (with > 0) jset_num(e, "with", with);
    cJSON_DeleteItemFromObjectCaseSensitive(e, "miss_at");
    cJSON_DeleteItemFromObjectCaseSensitive(e, "stems");
    cJSON *stems = cJSON_AddArrayToObject(e, "stems");
    cJSON_AddItemToArray(stems, cJSON_CreateString(stem));
    state_save();
    state_unlock();
    installed_changed();
}

/* After a game is down: the platform's BIOS files, then its saves, before it
 * shows as ready so it isn't started without them. */
static void finish_download(job *j, const profile_map *maps, int n, const cJSON *rom) {
    firmware_sync(maps, n, NULL, (int)jget_num(rom, "platform_id", 0), NULL);
    pthread_mutex_lock(&g_q_lock);
    j->status = JOB_SYNCING;
    pthread_mutex_unlock(&g_q_lock);
    char name[256], system[128], head[400];
    str_copy(name, sizeof name, j->name);
    str_copy(system, sizeof system, j->platform);
    sync_game_result saves;
    if (sync_game(j->target_id, name, system, &saves) != 0) { /* save sync off: just the game */
        config_lock();
        const int notify = g_cfg.notify;
        config_unlock();
        download_notice(head, sizeof head, name, system);
        if (notify) plat_notify("RommPS: %s", head);
    }
    pthread_mutex_lock(&g_q_lock);
    j->saves = saves;
    pthread_mutex_unlock(&g_q_lock);
    job_end(j, JOB_DONE, NULL, NULL);
}

/* A file a Switch download brings: one of a game's, or of its update's or DLC's. */
typedef struct {
    int rom_id, file_id; /* file_id 0: the game's one file, by its name */
    int with;            /* the game an update or DLC came with, else 0 */
    nx_kind kind;
    int had;             /* there, whole, before this download */
    int got;             /* brought by this download */
    int64_t size;
    char name[256];
    char dest[PATH_MAX_LEN];
    const cJSON *rom;
} nx_file;

static int nx_plan(const cJSON *rom, const profile_map *m, int with, nx_file *out, int max) {
    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files"), *f;
    int multi = cJSON_GetArraySize(files) > 0, n = 0;
    cJSON_ArrayForEach(f, files)
        if (!strcmp(jget_str(f, "file_name", ""), fs_name)) multi = 0;
    if (!multi) {
        if (max < 1) return 0;
        nx_file *x = &out[n++];
        memset(x, 0, sizeof *x);
        x->kind = nx_file_kind(fs_name, "");
        x->size = (int64_t)jget_num(rom, "fs_size_bytes", -1);
        str_copy(x->name, sizeof x->name, fs_name);
    } else {
        cJSON_ArrayForEach(f, files) {
            if (extra_file(f) || n == max) continue;
            nx_file *x = &out[n++];
            memset(x, 0, sizeof *x);
            x->file_id = (int)jget_num(f, "id", 0);
            x->kind = nx_file_kind(jget_str(f, "file_name", ""), jget_str(f, "category", ""));
            x->size = (int64_t)jget_num(f, "file_size_bytes", -1);
            str_copy(x->name, sizeof x->name, jget_str(f, "file_name", ""));
        }
    }
    for (int i = 0; i < n; i++) {
        out[i].rom_id = (int)jget_num(rom, "id", 0);
        out[i].with = with;
        out[i].rom = rom;
        path_join(out[i].dest, sizeof out[i].dest, nx_dir(m, out[i].kind), out[i].name);
        out[i].had = out[i].size >= 0 && file_size(out[i].dest) == out[i].size;
    }
    return n;
}

static void run_switch_job(job *j, const cJSON *rom, const profile_map *maps, int n, const profile_map *m) {
    nx_file *files = calloc(NX_MAX_FILES, sizeof *files);
    cJSON *addons = cJSON_CreateArray(), *a;
    if (!files) {
        job_end(j, JOB_ERROR, "out of memory%s", "");
        cJSON_Delete(addons);
        return;
    }
    int nf = nx_plan(rom, m, 0, files, NX_MAX_FILES), skipped = 0;
    unsigned long long game;
    if (nx_rom(rom, &game) == NX_GAME) {
        cJSON *found = nx_addons((int)jget_num(rom, "platform_id", 0), game);
        cJSON_ArrayForEach(a, found) {
            char path[64];
            snprintf(path, sizeof path, "/api/roms/%d", (int)jget_num(a, "id", 0));
            long st = 0;
            cJSON *full = romm_call("GET", path, NULL, &st);
            if (st != 200 || !full) {
                LOGW("%s: could not load its %s (RomM game %d)", j->name, jget_str(a, "kind", ""), (int)jget_num(a, "id", 0));
                cJSON_Delete(full);
                continue;
            }
            if (rom_archive(full, 1)) {
                LOGW("%s: its %s %s is archived, left out", j->name, jget_str(a, "kind", ""), jget_str(full, "fs_name", ""));
                cJSON_Delete(full);
                skipped++;
                continue;
            }
            nf += nx_plan(full, m, j->target_id, files + nf, NX_MAX_FILES - nf);
            cJSON_AddItemToArray(addons, full); /* the plan points into it */
        }
        cJSON_Delete(found);
    }
    int64_t total = 0;
    for (int i = 0; i < nf; i++) total += files[i].size > 0 ? files[i].size : 0;
    pthread_mutex_lock(&g_q_lock);
    j->total = total;
    pthread_mutex_unlock(&g_q_lock);

    int ok = 1, ups = 0, dlcs = 0;
    int64_t base = 0;
    http_resp r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < nf && ok; i++) {
        nx_file *x = &files[i];
        ups += x->kind == NX_UPDATE;
        dlcs += x->kind == NX_DLC;
        if (x->had) {
            base += x->size;
            continue;
        }
        char path[2048];
        char *q = http_escape(x->name);
        if (x->file_id) snprintf(path, sizeof path, "/api/roms/%d/files/content/%s", x->file_id, q);
        else snprintf(path, sizeof path, "/api/roms/%d/content/%s", x->rom_id, q);
        free(q);
        mkdir_p(nx_dir(m, x->kind));
        pthread_mutex_lock(&g_q_lock);
        j->base = base;
        snprintf(j->note, sizeof j->note, "%s", x->kind == NX_UPDATE ? "Update" : x->kind == NX_DLC ? "DLC" : "");
        pthread_mutex_unlock(&g_q_lock);
        x->got = romm_download(path, x->dest, progress_cb, j, &r) == 0;
        ok = x->got && !cancelled(j);
        base += file_size(x->dest) > 0 ? file_size(x->dest) : 0;
    }
    if (ok) {
        for (int i = 0; i < nf; i++) record_file(files[i].rom_id, files[i].rom, m, files[i].dest, files[i].with);
        LOGI("downloaded %s: %d file%s, %d update, %d DLC%s", j->name, nf, nf == 1 ? "" : "s", ups, dlcs,
             skipped ? " (archived ones left out)" : "");
        /* Another Switch emulator with folders of its own gets the same files. */
        const char *slug = jget_str(rom, "platform_slug", "");
        for (int k = 0; k < n; k++) {
            const profile_map *x = &maps[k];
            if (x == m || !x->extra || !profile_map_handles(x, slug, jget_str(rom, "platform_fs_slug", ""))) continue;
            for (int i = 0; i < nf; i++) {
                char other[PATH_MAX_LEN];
                path_join(other, sizeof other, nx_dir(x, files[i].kind), files[i].name);
                if (!strcmp(other, files[i].dest)) continue;
                mkdir_p(nx_dir(x, files[i].kind));
                if (link_or_copy(files[i].dest, other) == 0) record_file(files[i].rom_id, files[i].rom, x, other, files[i].with);
                else LOGW("could not put %s in %s", files[i].name, nx_dir(x, files[i].kind));
            }
        }
        finish_download(j, maps, n, rom);
    } else {
        /* Cancelled: what this download brought goes; what was there stays. */
        for (int i = 0; i < nf; i++) {
            char part[PATH_MAX_LEN + 8];
            snprintf(part, sizeof part, "%s.part", files[i].dest);
            unlink(part);
            if (files[i].got && cancelled(j)) unlink(files[i].dest);
        }
        job_end(j, JOB_ERROR, "%s", r.error);
    }
    cJSON_Delete(addons);
    free(files);
}

static void run_rom_job(job *j) {
    char path[2048], dest[PATH_MAX_LEN];
    snprintf(path, sizeof path, "/api/roms/%d", j->target_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    if (st != 200 || !rom) {
        job_end(j, JOB_ERROR, "could not load the game from RomM%s", "");
        cJSON_Delete(rom);
        return;
    }
    pthread_mutex_lock(&g_q_lock);
    str_copy(j->name, sizeof j->name, jget_str(rom, "name", jget_str(rom, "fs_name", "?")));
    str_copy(j->cover, sizeof j->cover, jget_str(rom, "path_cover_small", ""));
    str_copy(j->platform, sizeof j->platform, jget_str(rom, "platform_display_name", jget_str(rom, "platform_slug", "")));
    j->total = (int64_t)jget_num(rom, "fs_size_bytes", 0);
    pthread_mutex_unlock(&g_q_lock);

    profile_map *maps = NULL;
    int n = load_maps(&maps);
    const char *slug = jget_str(rom, "platform_slug", "");
    const profile_map *m = profile_find_for_platform(maps, n, slug, jget_str(rom, "platform_fs_slug", ""));
    if (!m) {
        job_end(j, JOB_ERROR, "no emulator is set up for %s", slug);
        free(maps);
        cJSON_Delete(rom);
        return;
    }

    const char *fs_name = jget_str(rom, "fs_name", "");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(rom, "files");
    /* fs_name is one of files[] only for a plain single-file rom; otherwise
     * it's the folder's name (no extension) and the rom is fetched per file. */
    int multi = cJSON_GetArraySize(files) > 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, files)
        if (strcmp(jget_str(f, "file_name", ""), fs_name) == 0) { multi = 0; break; }
    if (library_not_game(fs_name)) {
        job_end(j, JOB_ERROR, "%s isn't a game", fs_name);
        free(maps);
        cJSON_Delete(rom);
        return;
    }
    const char *archive = rom_archive(rom, is_switch(slug));
    if (archive) {
        job_end(j, JOB_ERROR, ARCHIVED_MSG, archive);
        free(maps);
        cJSON_Delete(rom);
        return;
    }
    if (is_switch(slug)) {
        run_switch_job(j, rom, maps, n, m);
        free(maps);
        cJSON_Delete(rom);
        return;
    }
    if (multi) {
        const cJSON *f;
        int64_t skipped = 0;
        cJSON_ArrayForEach(f, files) if (extra_file(f)) skipped += (int64_t)jget_num(f, "file_size_bytes", 0);
        pthread_mutex_lock(&g_q_lock);
        if (skipped < j->total) j->total -= skipped;
        pthread_mutex_unlock(&g_q_lock);
    }
    path_join(dest, sizeof dest, m->rom_dir, fs_name);
    mkdir_p(m->rom_dir);
    /* A game downloaded again: cancelling keeps what was already there. */
    const int had = file_exists(dest) || dir_exists(dest);
    int ok = 1;
    http_resp r;
    if (!multi) {
        char *q = http_escape(fs_name);
        snprintf(path, sizeof path, "/api/roms/%d/content/%s", j->target_id, q);
        free(q);
        ok = romm_download(path, dest, progress_cb, j, &r) == 0;
    } else {
        /* Fetched file by file into <rom_dir>/<fs_name>/ so nothing needs unzipping. */
        int64_t base = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, files) {
            const char *fn = jget_str(f, "file_name", "");
            if (extra_file(f)) continue;
            char fdest[PATH_MAX_LEN];
            path_join(fdest, sizeof fdest, dest, fn);
            int64_t size = (int64_t)jget_num(f, "file_size_bytes", -1);
            if (file_size(fdest) == size) {
                base += size;
                continue;
            }
            char *q = http_escape(fn);
            snprintf(path, sizeof path, "/api/roms/%d/files/content/%s", (int)jget_num(f, "id", 0), q);
            free(q);
            pthread_mutex_lock(&g_q_lock);
            j->base = base;
            pthread_mutex_unlock(&g_q_lock);
            if (romm_download(path, fdest, progress_cb, j, &r) != 0) {
                ok = 0;
                break;
            }
            base += file_size(fdest);
        }
    }

    if (ok && !cancelled(j)) {
        record_rom(j, rom, m, dest, multi);
        LOGI("downloaded %s to %s", j->name, dest);
        /* The extra emulators for this system get the same game: a hard link
         * where they share a drive, a copy where they don't. */
        for (int i = 0; i < n; i++) {
            const profile_map *x = &maps[i];
            if (x == m || !x->extra || !profile_map_handles(x, slug, jget_str(rom, "platform_fs_slug", ""))) continue;
            int covered = plat_is_mount(x->rom_dir);
            for (int k = 0; k < n && !covered; k++) covered = maps[k].mount_title[0] && !strcmp(maps[k].own_rom_dir, x->rom_dir);
            if (covered) continue; /* an extra's mount shows it the game */
            char other[PATH_MAX_LEN];
            path_join(other, sizeof other, x->rom_dir, fs_name);
            if (!strcmp(other, dest)) continue;
            mkdir_p(x->rom_dir);
            pthread_mutex_lock(&g_q_lock);
            snprintf(j->note, sizeof j->note, "Copying to %s", x->profile_name);
            j->base = j->done = j->total = 0;
            j->speed = 0;
            j->sample_t = 0;
            pthread_mutex_unlock(&g_q_lock);
            if (link_or_copy_progress(dest, other, progress_cb, j) == 0) {
                record_rom(j, rom, x, other, multi);
                LOGI("also for %s: %s", x->profile_name, other);
            } else {
                LOGW("could not put %s in %s", j->name, x->rom_dir);
            }
        }
        finish_download(j, maps, n, rom);
    } else {
        if (cancelled(j) && !(multi && had)) discard_partial(dest, multi);
        job_end(j, JOB_ERROR, "%s", r.error);
    }
    free(maps);
    cJSON_Delete(rom);
}

static void run_firmware_job(job *j) {
    char path[64];
    profile_map *maps = NULL;
    int n = load_maps(&maps), failed = 0;
    long st = 0;
    snprintf(path, sizeof path, "/api/platforms/%d", j->target_id);
    cJSON *plat = romm_call("GET", path, NULL, &st);
    pthread_mutex_lock(&g_q_lock);
    str_copy(j->platform, sizeof j->platform, jget_str(plat, "display_name", jget_str(plat, "name", "")));
    pthread_mutex_unlock(&g_q_lock);
    if (st != 200 || !profile_find_for_platform(maps, n, jget_str(plat, "slug", ""), jget_str(plat, "fs_slug", ""))) {
        job_end(j, JOB_ERROR, "no emulator is set up for %s", plat ? jget_str(plat, "slug", "") : "this platform");
        cJSON_Delete(plat);
        free(maps);
        return;
    }
    /* Every emulator that plays it gets them, as a sync does. */
    cJSON *one = cJSON_CreateArray();
    cJSON_AddItemToArray(one, plat);
    int count = firmware_sync(maps, n, one, j->target_id, &failed);
    cJSON_Delete(one);
    pthread_mutex_lock(&g_q_lock);
    snprintf(j->name, sizeof j->name, "BIOS files (%d new)", count > 0 ? count : 0);
    pthread_mutex_unlock(&g_q_lock);
    char msg[32];
    snprintf(msg, sizeof msg, "%d file(s) failed", failed);
    job_end(j, failed || count < 0 ? JOB_ERROR : JOB_DONE, "%s", msg);
    free(maps);
}

static int concurrency(void) {
    config_lock();
    int n = g_cfg.download_concurrency;
    config_unlock();
    return n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n;
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_q_lock);
        job *j;
        while (g_running >= concurrency() || !(j = oldest_queued())) pthread_cond_wait(&g_q_cond, &g_q_lock);
        j->status = JOB_RUNNING;
        g_running++;
        pthread_mutex_unlock(&g_q_lock);

        if (j->type == JOB_ROM)
            run_rom_job(j);
        else
            run_firmware_job(j);

        pthread_mutex_lock(&g_q_lock);
        g_running--;
        pthread_cond_broadcast(&g_q_cond);
        pthread_mutex_unlock(&g_q_lock);
    }
    return NULL;
}

void library_init(void) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        thread_start(worker, NULL);
    }
}

int library_busy(void) {
    int busy = 0;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j && !busy; j = j->next)
        busy = j->status == JOB_QUEUED || j->status == JOB_RUNNING || j->status == JOB_SYNCING;
    pthread_mutex_unlock(&g_q_lock);
    return busy;
}

void library_wake(void) {
    pthread_mutex_lock(&g_q_lock);
    pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
}

/* Drops the oldest finished jobs once the list is long. Caller holds the lock. */
static void trim_jobs(void) {
    int n = 0;
    for (job *p = g_jobs; p; p = p->next) n++;
    while (n > MAX_JOBS) {
        job **pp = &g_jobs, **oldest = NULL;
        for (; *pp; pp = &(*pp)->next)
            if (finished(*pp)) oldest = pp;
        if (!oldest) break;
        job *dead = *oldest;
        *oldest = dead->next;
        free(dead);
        n--;
    }
}

static int enqueue(job_type type, int id, const char *name, const char *cover, char *err, int en) {
    pthread_mutex_lock(&g_q_lock);
    for (job *p = g_jobs; p; p = p->next) {
        if (p->type == type && p->target_id == id && !finished(p)) {
            pthread_mutex_unlock(&g_q_lock);
            snprintf(err, (size_t)en, "already downloading");
            return -1;
        }
    }
    job *j = calloc(1, sizeof *j);
    if (!j) {
        pthread_mutex_unlock(&g_q_lock);
        snprintf(err, (size_t)en, "out of memory");
        return -1;
    }
    j->type = type;
    j->target_id = id;
    str_copy(j->name, sizeof j->name, name);
    str_copy(j->cover, sizeof j->cover, cover ? cover : "");
    j->id = ++g_job_seq;
    j->next = g_jobs;
    g_jobs = j;
    trim_jobs();
    pthread_cond_broadcast(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
    return 0;
}

int library_queue_rom(int rom_id, const char *name, const char *cover, char *err, int en) {
    /* The name the client shows, so the download is named from the start;
     * RomM's own replaces it once the job runs. */
    char fallback[32];
    snprintf(fallback, sizeof fallback, "Game #%d", rom_id);
    /* An archive is turned down at once, so the client can say why; RomM
     * out of reach leaves that to the download. */
    char path[64];
    snprintf(path, sizeof path, "/api/roms/%d", rom_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    const char *archive = st == 200 ? rom_archive(rom, is_switch(jget_str(rom, "platform_slug", ""))) : NULL;
    const int refused = archive != NULL;
    if (refused) snprintf(err, (size_t)en, ARCHIVED_MSG, archive);
    cJSON_Delete(rom);
    if (refused) return -1;
    return enqueue(JOB_ROM, rom_id, name && name[0] ? name : fallback, cover, err, en);
}

int library_queue_firmware(int platform_id, char *err, int en) {
    return enqueue(JOB_FIRMWARE, platform_id, "BIOS files", NULL, err, en);
}

int library_cancel(int id, char *err, int en) {
    int rc = -1;
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        if (j->id != id) continue;
        if (j->status == JOB_QUEUED) {
            j->status = JOB_CANCELLED;
            rc = 0;
        } else if (j->status == JOB_RUNNING) {
            j->cancel = 1; /* the progress callback aborts the transfer */
            rc = 0;
        } else if (j->status == JOB_SYNCING) {
            snprintf(err, (size_t)en, "the game is downloaded, its saves are syncing");
        } else {
            snprintf(err, (size_t)en, "download already finished");
        }
        break;
    }
    pthread_mutex_unlock(&g_q_lock);
    if (rc && !err[0]) snprintf(err, (size_t)en, "no such download");
    return rc;
}

int library_clear_finished(void) {
    int n = 0;
    pthread_mutex_lock(&g_q_lock);
    job **pp = &g_jobs;
    while (*pp) {
        if (finished(*pp)) {
            job *dead = *pp;
            *pp = dead->next;
            free(dead);
            n++;
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_q_lock);
    return n;
}

/* A game that's already gone (deleted by hand, or its drive unplugged) counts
 * as removed: there's nothing left to fail on. */
int library_delete_rom(int rom_id, char *err, int en) {
    /* 0 is what files RomM didn't match are recorded as: never theirs to delete. */
    if (rom_id <= 0) {
        snprintf(err, (size_t)en, "no game given");
        return -1;
    }
    int removed = 0;
    state_lock();
    cJSON *roms = state_section("roms"), *e = roms ? roms->child : NULL;
    while (e) {
        cJSON *next = e->next;
        /* A Switch game's update and DLC go with it. */
        if ((int)jget_num(e, "rom_id", -1) == rom_id || (int)jget_num(e, "with", 0) == rom_id) {
            LOGI("removing %s (saves are kept)", e->string);
            remove_tree(e->string);
            cJSON_DeleteItemFromObjectCaseSensitive(roms, e->string);
            removed++;
        }
        e = next;
    }
    state_save();
    state_unlock();
    installed_changed();
    if (!removed) LOGI("rom %d: nothing of it on the console to remove", rom_id);
    /* Can't leave a finished download in the downloads page if we removed that title. */
    pthread_mutex_lock(&g_q_lock);
    for (job **pp = &g_jobs; *pp;) {
        job *dead = *pp;
        if (dead->type == JOB_ROM && dead->target_id == rom_id && finished(dead)) {
            *pp = dead->next;
            free(dead);
        } else {
            pp = &dead->next;
        }
    }
    pthread_mutex_unlock(&g_q_lock);
    return 0;
}

cJSON *library_downloads(void) {
    static const char *names[] = {"queued", "downloading", "done", "error", "cancelled", "syncing"};
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_q_lock);
    for (job *j = g_jobs; j; j = j->next) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", j->id);
        cJSON_AddStringToObject(o, "type", j->type == JOB_ROM ? "rom" : "firmware");
        cJSON_AddNumberToObject(o, j->type == JOB_ROM ? "rom_id" : "platform_id", j->target_id);
        cJSON_AddStringToObject(o, "name", j->name);
        cJSON_AddStringToObject(o, "platform", j->platform);
        if (j->type == JOB_ROM) cJSON_AddStringToObject(o, "cover", j->cover);
        cJSON_AddStringToObject(o, "status", j->status == JOB_RUNNING && j->cancel ? "cancelling" : names[j->status]);
        cJSON_AddStringToObject(o, "error", j->error);
        cJSON_AddNumberToObject(o, "done", (double)j->done);
        cJSON_AddNumberToObject(o, "total", (double)j->total);
        cJSON_AddNumberToObject(o, "speed", j->status == JOB_RUNNING ? j->speed : 0);
        cJSON_AddStringToObject(o, "note", j->status == JOB_RUNNING ? j->note : "");
        if (j->status == JOB_DONE && j->type == JOB_ROM) {
            cJSON_AddNumberToObject(o, "saves", j->saves.saves);
            cJSON_AddNumberToObject(o, "states", j->saves.states);
            cJSON_AddNumberToObject(o, "save_conflicts", j->saves.conflicts);
            cJSON_AddBoolToObject(o, "saves_failed", j->saves.failed);
        }
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_q_lock);
    return arr;
}

cJSON *library_platforms(char *err, int en) {
    long st = 0;
    cJSON *plats = romm_call("GET", "/api/platforms", NULL, &st);
    if (st != 200 || !cJSON_IsArray(plats)) {
        if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load platforms (HTTP %ld)", st);
        cJSON_Delete(plats);
        return NULL;
    }
    profile_map *maps = NULL;
    int n = load_maps(&maps);
    cJSON *out = cJSON_CreateArray();
    const cJSON *p;
    cJSON_ArrayForEach(p, plats) {
        if (jget_num(p, "rom_count", 0) <= 0) continue;
        const profile_map *m = profile_find_for_platform(maps, n, jget_str(p, "slug", ""), jget_str(p, "fs_slug", ""));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", jget_num(p, "id", 0));
        cJSON_AddStringToObject(o, "slug", jget_str(p, "slug", ""));
        cJSON_AddStringToObject(o, "fs_slug", jget_str(p, "fs_slug", ""));
        cJSON_AddStringToObject(o, "name", jget_str(p, "display_name", jget_str(p, "name", "")));
        cJSON_AddNumberToObject(o, "rom_count", jget_num(p, "rom_count", 0));
        cJSON_AddNumberToObject(o, "firmware_count", jget_num(p, "firmware_count", 0));
        if (m) {
            cJSON_AddStringToObject(o, "profile", m->profile_name);
            cJSON_AddStringToObject(o, "rom_dir", m->rom_dir);
        }
        cJSON_AddItemToArray(out, o);
    }
    free(maps);
    cJSON_Delete(plats);
    return out;
}

/* An extra emulator turned on for a system gets the games already there, not
 * only the ones downloaded from now on: each game in the system's main
 * emulator's folder is linked into the extra's (a copy across drives). */
/* A game folder a download is still filling (its files arrive as .part). */
static int still_downloading(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int part = 0;
    while (d && !part && (de = readdir(d))) part = ext_in_list(de->d_name, ".part");
    if (d) closedir(d);
    return part;
}

/* The copy in an extra emulator's folder is recorded like the game it came
 * from, so the game's saves sync there too and removing the game removes it. */
static void record_link(const char *from, const char *to, const profile_map *x) {
    state_lock();
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
    if (src && jget_num(src, "rom_id", 0) > 0) {
        cJSON *e = state_entry_ensure("roms", to);
        const cJSON *f;
        cJSON_ArrayForEach(f, src) if (!cJSON_GetObjectItemCaseSensitive(e, f->string))
            cJSON_AddItemToObject(e, f->string, cJSON_Duplicate(f, 1));
        jset_str(e, "profile", x->profile_id);
        jset_str(e, "platform_dir", x->platform_dir);
        state_save();
    }
    state_unlock();
}

/* The extras that play the main emulator's games through a mount (see
 * profile_map.mount_games), for the console to put on when they start. */
static void update_mounts(const profile_map *maps, int n) {
    plat_mount *want = calloc((size_t)(n > 0 ? n : 1), sizeof *want);
    int nw = 0;
    for (int i = 0; want && i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->mount_title[0]) continue;
        int dup = 0;
        for (int k = 0; k < nw && !dup; k++) dup = !strcmp(want[k].title, x->mount_title) && !strcmp(want[k].into, x->mount_into);
        if (dup) continue;
        mkdir_p(x->own_rom_dir); /* to mount on */
        str_copy(want[nw].title, sizeof want[nw].title, x->mount_title);
        str_copy(want[nw].name, sizeof want[nw].name, x->profile_name);
        str_copy(want[nw].from, sizeof want[nw].from, x->rom_dir);
        str_copy(want[nw].into, sizeof want[nw].into, x->mount_into);
        nw++;
    }
    plat_mounts_set(want, nw);
    free(want);
}

/* An extra that could play the main emulator's games through a mount but
 * has games of its own in its folder (it was the main one before): the ones
 * RommPS downloaded move to the main one's folder, a rename on the same
 * drive, so the mount can go on. Files it didn't download stay, and so does
 * the copy. Returns how many moved. */
static int move_to_main(const profile_map *maps, int n) {
    int moved = 0;
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->mount_blocked || plat_is_mount(x->rom_dir)) continue;
        char sys[64];
        str_copy(sys, sizeof sys, x->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        const profile_map *m = profile_find_for_platform(maps, n, sys, NULL);
        if (!m || m == x || m->extra) continue;
        DIR *d = opendir(x->rom_dir);
        struct dirent *de;
        int here = 0;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || ext_in_list(de->d_name, ".part,.tmp")) continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, x->rom_dir, de->d_name);
            path_join(to, sizeof to, m->rom_dir, de->d_name);
            if (file_exists(to) || dir_exists(to)) continue; /* a copy: cleared once the mount is on */
            state_lock();
            const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
            const int ours = rec && jget_num(rec, "rom_id", 0) > 0;
            state_unlock();
            if (!ours || mkdir_p(m->rom_dir) != 0 || rename(from, to) != 0) continue;
            state_lock();
            cJSON *e = cJSON_DetachItemFromObjectCaseSensitive(state_section("roms"), from);
            const int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
            if (e) {
                jset_str(e, "profile", m->profile_id);
                jset_str(e, "platform_dir", m->platform_dir);
                cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
                cJSON_AddItemToObject(state_section("roms"), to, e);
            }
            state_save();
            state_unlock();
            LOGI("moved %s to %s: %s plays it from there now", from, to, m->profile_name);
            char why[160];
            snprintf(why, sizeof why, "From %s's folder to %s's", x->profile_name, m->profile_name);
            events_add("game", "moved", rom_id, to, x->profile_name, why);
            here++;
        }
        if (d) closedir(d);
        if (here) {
            events_save();
            plat_notify("RommPS: %d %s game%s moved to %s", here, x->profile_name, here == 1 ? "" : "s", m->profile_name);
        }
        moved += here;
    }
    return moved;
}

int library_own_games(const char *root, const char *system, const char *action, char *err, int en) {
    const int move = !strcmp(action, "move");
    if (!move && strcmp(action, "delete") != 0) {
        snprintf(err, (size_t)en, "action is move or delete");
        return -1;
    }
    profile_map *maps = NULL;
    int n = load_maps(&maps), done = 0;
    const profile_map *x = NULL, *m = NULL;
    for (int i = 0; i < n && !x; i++) {
        char sys[64];
        str_copy(sys, sizeof sys, maps[i].romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        if (maps[i].extra && maps[i].mount_blocked && !strcmp(maps[i].root, root) && !strcmp(sys, system)) x = &maps[i];
    }
    if (x) {
        char sys[64];
        str_copy(sys, sizeof sys, x->romm_slugs);
        sys[strcspn(sys, ",")] = 0;
        m = profile_find_for_platform(maps, n, sys, NULL);
    }
    if (!x || !m || m == x || m->extra) {
        snprintf(err, (size_t)en, "no extra emulator at %s for %s with games in the way of its mount", root, system);
        free(maps);
        return -1;
    }
    if (plat_is_mount(x->rom_dir)) {
        snprintf(err, (size_t)en, "close %s first", x->profile_name);
        free(maps);
        return -1;
    }
    DIR *d = opendir(x->rom_dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.' || ext_in_list(de->d_name, ".part,.tmp")) continue;
        char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
        path_join(from, sizeof from, x->rom_dir, de->d_name);
        path_join(to, sizeof to, m->rom_dir, de->d_name);
        if (file_exists(to) || dir_exists(to)) continue; /* a copy */
        if (move && (mkdir_p(m->rom_dir) != 0 || rename(from, to) != 0)) {
            LOGW("could not move %s to %s", from, to);
            continue;
        }
        if (!move) remove_tree(from);
        state_lock();
        cJSON *e = cJSON_DetachItemFromObjectCaseSensitive(state_section("roms"), from);
        const int rom_id = e ? (int)jget_num(e, "rom_id", 0) : 0;
        if (e && move) {
            jset_str(e, "profile", m->profile_id);
            jset_str(e, "platform_dir", m->platform_dir);
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
            cJSON_AddItemToObject(state_section("roms"), to, e);
        } else {
            cJSON_Delete(e);
        }
        state_save();
        state_unlock();
        char why[160];
        snprintf(why, sizeof why, move ? "From %s's folder to %s's" : "Deleted from %s's folder", x->profile_name,
                 m->profile_name);
        events_add("game", move ? "moved" : "removed", rom_id, move ? to : from, x->profile_name, why);
        LOGI("%s %s%s%s", move ? "moved" : "deleted", from, move ? " to " : "", move ? to : "");
        done++;
    }
    if (d) closedir(d);
    events_save();
    free(maps);
    library_link_extras(); /* the mount can go on now */
    return done;
}

/* Whether b holds the same game as a: the same size, file by file. */
static int same_game(const char *a, const char *b) {
    if (!dir_exists(a)) return file_exists(b) && file_size(a) == file_size(b);
    if (!dir_exists(b)) return 0;
    DIR *d = opendir(a);
    struct dirent *de;
    int same = d != NULL;
    while (same && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char x[PATH_MAX_LEN], y[PATH_MAX_LEN];
        path_join(x, sizeof x, a, de->d_name);
        path_join(y, sizeof y, b, de->d_name);
        same = same_game(x, y);
    }
    if (d) closedir(d);
    return same;
}

static int64_t game_size(const char *path) {
    if (!dir_exists(path)) return file_size(path) > 0 ? file_size(path) : 0;
    DIR *d = opendir(path);
    struct dirent *de;
    int64_t total = 0;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[PATH_MAX_LEN];
        path_join(sub, sizeof sub, path, de->d_name);
        total += game_size(sub);
    }
    if (d) closedir(d);
    return total;
}

/* Copies made for an extra before it had a mount: the main emulator has the
 * same game, so they go, and so do their records. */
static void clear_old_copies(const profile_map *maps, int n) {
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->mount_title[0]) continue;
        int dup = 0;
        for (int k = 0; k < i && !dup; k++) dup = !strcmp(maps[k].own_rom_dir, x->own_rom_dir);
        /* Our mount on it while the app runs: those are the main one's
         * games. A folder another emulator plays from keeps its games. */
        for (int k = 0; k < n && !dup; k++) dup = !strcmp(maps[k].rom_dir, x->own_rom_dir);
        if (dup || plat_is_mount(x->own_rom_dir)) continue;
        DIR *d = opendir(x->own_rom_dir);
        struct dirent *de;
        int gone = 0;
        int64_t freed = 0;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char copy[PATH_MAX_LEN], game[PATH_MAX_LEN];
            path_join(copy, sizeof copy, x->own_rom_dir, de->d_name);
            path_join(game, sizeof game, x->rom_dir, de->d_name);
            if (!same_game(copy, game)) {
                LOGW("%s: %s isn't the same as %s, kept", x->profile_name, copy, game);
                continue;
            }
            const int64_t size = game_size(copy);
            remove_tree(copy);
            if (file_exists(copy) || dir_exists(copy)) {
                LOGW("%s: could not remove the copy %s", x->profile_name, copy);
                continue;
            }
            state_lock();
            const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), copy);
            const int rom_id = rec ? (int)jget_num(rec, "rom_id", 0) : 0;
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), copy);
            state_save();
            state_unlock();
            events_add("game", "removed", rom_id, copy, x->profile_name, "A copy: it plays the main emulator's now");
            gone++;
            freed += size;
        }
        if (d) closedir(d);
        if (gone) {
            LOGI("%s plays %s's games now: removed %d copies (%lld bytes)", x->profile_name, x->rom_dir, gone, (long long)freed);
            events_save();
            plat_notify("RommPS: %s now plays the same game files. %.1f GB freed.", x->profile_name, (double)freed / 1e9);
        }
    }
}

/* Switch games downloaded before 1.3 as a folder each, which the emulator
 * doesn't look in: the files move out, the game's to rom_dir and its update's
 * and DLC's to theirs, each recorded as the game. An empty folder (a download
 * that never finished) goes; one holding an archive stays as it is. */
static void nx_unfold(const profile_map *maps, int n) {
    typedef struct { char path[PATH_MAX_LEN]; int map; } folder;
    folder *todo = NULL;
    int nt = 0, cap = 0, logged = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        if (jget_num(e, "rom_id", 0) <= 0) continue;
        char parent[PATH_MAX_LEN];
        str_copy(parent, sizeof parent, e->string);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        for (int i = 0; i < n; i++) {
            const profile_map *m = &maps[i];
            if (strncmp(m->romm_slugs, "switch", 6) != 0 || (m->romm_slugs[6] && m->romm_slugs[6] != ',') ||
                strcmp(jget_str(e, "profile", ""), m->profile_id) != 0 || strcmp(parent, m->rom_dir) != 0)
                continue;
            if (nt == cap) {
                cap = cap ? cap * 2 : 8;
                folder *grown = realloc(todo, sizeof *todo * (size_t)cap);
                if (!grown) break;
                todo = grown;
            }
            str_copy(todo[nt].path, sizeof todo[nt].path, e->string);
            todo[nt++].map = i;
            break;
        }
    }
    state_unlock();
    for (int t = 0; t < nt; t++) {
        const char *dir = todo[t].path;
        const profile_map *m = &maps[todo[t].map];
        if (!dir_exists(dir) || still_downloading(dir)) continue;
        state_lock();
        cJSON *rec = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(state_section("roms"), dir), 1);
        state_unlock();
        if (!rec) continue;
        const int rom_id = (int)jget_num(rec, "rom_id", 0);
        int moved = 0, left = 0;
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, dir, de->d_name);
            const nx_kind k = nx_file_kind(de->d_name, "");
            path_join(to, sizeof to, nx_dir(m, k), de->d_name);
            if (dir_exists(from) || ext_in_list(de->d_name, ".rar,.zip,.7z") || file_exists(to) ||
                mkdir_p(nx_dir(m, k)) != 0 || move_file(from, to) != 0) {
                left++;
                continue;
            }
            state_lock();
            cJSON *ne = cJSON_Duplicate(rec, 1);
            jset_str(ne, "fs_name", de->d_name);
            cJSON_DeleteItemFromObjectCaseSensitive(ne, "md5");
            cJSON_DeleteItemFromObjectCaseSensitive(ne, "stems");
            char stem[256];
            path_stem(stem, sizeof stem, de->d_name);
            cJSON_AddItemToArray(cJSON_AddArrayToObject(ne, "stems"), cJSON_CreateString(stem));
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), to);
            cJSON_AddItemToObject(state_section("roms"), to, ne);
            state_save();
            state_unlock();
            moved++;
        }
        if (d) closedir(d);
        if (left) {
            if (moved) LOGI("%s: %d file%s moved out, %d left (archives stay)", dir, moved, moved == 1 ? "" : "s", left);
        } else {
            rmdir(dir);
            state_lock();
            cJSON_DeleteItemFromObjectCaseSensitive(state_section("roms"), dir);
            state_save();
            state_unlock();
            LOGI("%s: %s", dir, moved ? "unfolded, the emulator sees it now" : "empty, removed");
            events_add("game", moved ? "moved" : "removed", rom_id, dir, m->profile_name,
                       moved ? "Out of its folder, where the emulator sees it" : "An empty folder: the download never finished");
            logged++;
        }
        cJSON_Delete(rec);
        if (moved || !left) installed_changed();
    }
    if (logged) events_save();
    free(todo);
}

static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_link_running, g_link_again;

static void *link_extras_thread(void *arg) {
    (void)arg;
again:;
    profile_map *maps = NULL;
    int n = load_maps(&maps), linked = 0;
    if (move_to_main(maps, n)) { /* now it can have a mount */
        free(maps);
        maps = NULL;
        n = load_maps(&maps);
    }
    update_mounts(maps, n);
    clear_old_copies(maps, n);
    nx_unfold(maps, n);
    for (int i = 0; i < n; i++) {
        const profile_map *x = &maps[i];
        if (!x->extra) continue;
        char first[64];
        str_copy(first, sizeof first, x->romm_slugs);
        char *comma = strchr(first, ',');
        if (comma) *comma = 0;
        const profile_map *m = profile_find_for_platform(maps, n, first, NULL);
        if (!m || m == x || m->extra || !strcmp(m->rom_dir, x->rom_dir)) continue;
        /* A folder an extra's mount covers gets no copies. */
        int covered = plat_is_mount(x->rom_dir);
        for (int k = 0; k < n && !covered; k++) covered = maps[k].mount_title[0] && !strcmp(maps[k].own_rom_dir, x->rom_dir);
        if (covered) continue;
        /* A folder several of the main emulator's systems share (Porpoise's
         * games: GameCube and Wii): only the games matched for this system. */
        int shared = 0;
        for (int k = 0; k < n; k++) shared |= &maps[k] != m && !strcmp(maps[k].rom_dir, m->rom_dir);
        DIR *d = opendir(m->rom_dir);
        struct dirent *de;
        int here = 0;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.' || ext_in_list(de->d_name, m->save_exts) || ext_in_list(de->d_name, m->state_exts) ||
                ext_in_list(de->d_name, ".part,.tmp"))
                continue;
            char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
            path_join(from, sizeof from, m->rom_dir, de->d_name);
            if (shared) {
                state_lock();
                const cJSON *rec = cJSON_GetObjectItemCaseSensitive(state_section("roms"), from);
                const int ours = rec && jget_num(rec, "rom_id", 0) > 0 && !strcmp(jget_str(rec, "platform_dir", ""), m->platform_dir);
                state_unlock();
                if (!ours) continue;
            }
            path_join(to, sizeof to, x->rom_dir, de->d_name);
            if (file_exists(to) || dir_exists(to) || (dir_exists(from) && still_downloading(from))) continue;
            mkdir_p(x->rom_dir);
            if (link_or_copy(from, to) == 0) {
                record_link(from, to, x);
                here++;
            } else {
                LOGW("could not put %s in %s", de->d_name, x->rom_dir);
            }
        }
        if (d) closedir(d);
        if (here) {
            char sys[64];
            str_copy(sys, sizeof sys, x->platform_dir);
            for (char *c = sys; *c; c++)
                if (*c >= 'a' && *c <= 'z') *c -= 32;
            plat_notify("RommPS: %d %s game%s now also sync with %s", here, sys, here == 1 ? "" : "s",
                        x->profile_name);
            linked += here;
        }
    }
    if (config_is_paired()) firmware_sync(maps, n, NULL, 0, NULL); /* emulators just turned on */
    free(maps);
    if (linked) {
        LOGI("extra emulators: %d games linked in", linked);
        sync_request("extra emulators");
    }
    /* One at a time; settings saved meanwhile get one more round. */
    pthread_mutex_lock(&g_link_lock);
    int again = g_link_again;
    g_link_again = 0;
    g_link_running = again;
    pthread_mutex_unlock(&g_link_lock);
    if (again) goto again;
    return NULL;
}

void library_link_extras(void) {
    pthread_mutex_lock(&g_link_lock);
    int start = !g_link_running;
    if (start) g_link_running = 1;
    else g_link_again = 1;
    pthread_mutex_unlock(&g_link_lock);
    if (start && thread_start(link_extras_thread, NULL) != 0) {
        pthread_mutex_lock(&g_link_lock);
        g_link_running = 0;
        pthread_mutex_unlock(&g_link_lock);
    }
}

/* What RommPS shows about a game: RomM's metadata (release year, publishers,
 * genres, players, rating, summary, How Long to Beat), the user's own data
 * (last played, status, completion, rating) and the play time RomM has
 * recorded. Kept a few minutes, so moving through a shelf stays quick. */
#define DETAILS_SLOTS 24
#define DETAILS_SEC   300
static struct {
    int id;
    double at;
    cJSON *details;
} g_details[DETAILS_SLOTS];
static pthread_mutex_t g_details_lock = PTHREAD_MUTEX_INITIALIZER;

static void join_strings(cJSON *out, const char *key, const cJSON *arr, int max) {
    char buf[256] = "";
    int n = 0;
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        if (!cJSON_IsString(x) || !x->valuestring[0] || n == max) continue;
        size_t l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s%s", n ? ", " : "", x->valuestring);
        n++;
    }
    if (buf[0]) cJSON_AddStringToObject(out, key, buf);
}

cJSON *library_details(int rom_id, char *err, int en) {
    double now = mono_now();
    pthread_mutex_lock(&g_details_lock);
    for (int i = 0; i < DETAILS_SLOTS; i++)
        if (g_details[i].details && g_details[i].id == rom_id && now - g_details[i].at < DETAILS_SEC) {
            cJSON *hit = cJSON_Duplicate(g_details[i].details, 1);
            pthread_mutex_unlock(&g_details_lock);
            return hit;
        }
    pthread_mutex_unlock(&g_details_lock);

    char path[160];
    snprintf(path, sizeof path, "/api/roms/%d", rom_id);
    long st = 0;
    cJSON *rom = romm_call("GET", path, NULL, &st);
    if (st != 200 || !rom) {
        if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load the game from RomM (HTTP %ld)", st);
        cJSON_Delete(rom);
        return NULL;
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", rom_id);
    add_version(out, rom);
    /* What a download would bring, or why it can't. */
    const int nx = is_switch(jget_str(rom, "platform_slug", ""));
    const char *archive = rom_archive(rom, nx);
    if (archive) {
        char msg[200];
        snprintf(msg, sizeof msg, ARCHIVED_MSG, archive);
        cJSON_AddStringToObject(out, "archived", msg);
    }
    unsigned long long game;
    /* A Switch game's update and DLC, in its folder or as games of their own:
     * [{"name", "kind", "size"}], updates first. */
    if (nx && !archive && nx_rom(rom, &game) == NX_GAME) {
        cJSON *list = cJSON_CreateArray(), *found = nx_addons((int)jget_num(rom, "platform_id", 0), game), *a;
        const cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(rom, "files")) {
            if (extra_file(f)) continue;
            const nx_kind k = nx_file_kind(jget_str(f, "file_name", ""), jget_str(f, "category", ""));
            if (k != NX_UPDATE && k != NX_DLC) continue;
            a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "kind", k == NX_UPDATE ? "update" : "dlc");
            cJSON_AddStringToObject(a, "name", jget_str(f, "file_name", ""));
            cJSON_AddNumberToObject(a, "size", jget_num(f, "file_size_bytes", 0));
            cJSON_AddItemToArray(found, a);
        }
        int updates = 0, dlc = 0;
        for (int pass = 0; pass < 2; pass++)
            cJSON_ArrayForEach(a, found) {
                const int up = !strcmp(jget_str(a, "kind", ""), "update");
                if (up != !pass) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "kind", up ? "update" : "dlc");
                cJSON_AddStringToObject(o, "name", jget_str(a, "name", ""));
                cJSON_AddNumberToObject(o, "size", jget_num(a, "size", 0));
                cJSON_AddItemToArray(list, o);
                updates += up;
                dlc += !up;
            }
        cJSON_Delete(found);
        if (updates) cJSON_AddNumberToObject(out, "updates", updates);
        if (dlc) cJSON_AddNumberToObject(out, "dlc", dlc);
        if (cJSON_GetArraySize(list)) cJSON_AddItemToObject(out, "addons", list);
        else cJSON_Delete(list);
    }
    const char *summary = jget_str(rom, "summary", "");
    if (summary[0]) cJSON_AddStringToObject(out, "summary", summary);
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(rom, "metadatum");
    /* A release date in seconds or milliseconds since 1970, as a year. */
    double date = jget_num(meta, "first_release_date", 0);
    if (date > 1e11) date /= 1000.0;
    int year = 0;
    if (date > 0) {
        time_t t = (time_t)date;
        struct tm tm;
        gmtime_r(&t, &tm);
        year = tm.tm_year + 1900;
    }
    const cJSON *hltb = cJSON_GetObjectItemCaseSensitive(rom, "hltb_metadata");
    if (!year) year = (int)jget_num(hltb, "release_year", 0);
    if (year) cJSON_AddNumberToObject(out, "year", year);
    join_strings(out, "publishers", cJSON_GetObjectItemCaseSensitive(meta, "publishers"), 2);
    join_strings(out, "developers", cJSON_GetObjectItemCaseSensitive(meta, "developers"), 2);
    join_strings(out, "genres", cJSON_GetObjectItemCaseSensitive(meta, "genres"), 3);
    const char *players = jget_str(meta, "player_count", "");
    if (players[0]) cJSON_AddStringToObject(out, "players", players);
    double rating = jget_num(meta, "average_rating", 0);
    if (rating > 0) cJSON_AddNumberToObject(out, "rating", rating);
    double main_story = jget_num(hltb, "main_story", 0);
    if (main_story > 0) cJSON_AddNumberToObject(out, "main_story_s", main_story);
    const cJSON *mine = cJSON_GetObjectItemCaseSensitive(rom, "rom_user");
    const char *last = jget_str(mine, "last_played", "");
    if (last[0]) cJSON_AddStringToObject(out, "last_played", last);
    const char *status = jget_str(mine, "status", "");
    if (status[0]) cJSON_AddStringToObject(out, "status", status);
    if (jget_num(mine, "completion", 0) > 0) cJSON_AddNumberToObject(out, "completion", jget_num(mine, "completion", 0));
    if (jget_num(mine, "rating", 0) > 0) cJSON_AddNumberToObject(out, "my_rating", jget_num(mine, "rating", 0));
    cJSON_Delete(rom);

    /* Play time: the sessions RomM has for the game, from every device. */
    double played_ms = 0;
    int sessions = 0;
    for (int offset = 0; offset < 2000; offset += 200) {
        char sp[160];
        snprintf(sp, sizeof sp, "/api/play-sessions?rom_id=%d&limit=200&offset=%d", rom_id, offset);
        cJSON *list = romm_call("GET", sp, NULL, &st);
        const cJSON *items = cJSON_IsArray(list) ? list : cJSON_GetObjectItemCaseSensitive(list, "items"), *x;
        int got = 0;
        cJSON_ArrayForEach(x, items) {
            played_ms += jget_num(x, "duration_ms", 0);
            got++;
        }
        sessions += got;
        cJSON_Delete(list);
        if (st != 200 || got < 200) break;
    }
    if (sessions) {
        cJSON_AddNumberToObject(out, "play_ms", played_ms);
        cJSON_AddNumberToObject(out, "sessions", sessions);
    }

    pthread_mutex_lock(&g_details_lock);
    int slot = 0;
    for (int i = 0; i < DETAILS_SLOTS; i++) {
        if (!g_details[i].details || g_details[i].id == rom_id) {
            slot = i;
            break;
        }
        if (g_details[i].at < g_details[slot].at) slot = i;
    }
    cJSON_Delete(g_details[slot].details);
    g_details[slot].id = rom_id;
    g_details[slot].at = now;
    g_details[slot].details = cJSON_Duplicate(out, 1);
    pthread_mutex_unlock(&g_details_lock);
    return out;
}

/* The games on this console, most recently played first: when one of its saves
 * last changed, or else when its file did. [{id, name, fs_name, platform_id,
 * cover, profile, played}], at most `limit`. */
/* Games recorded before the state kept a game's cover and platform get them
 * from RomM, a few per call so a first look stays quick. */
#define INSTALLED_BACKFILL 6
static void backfill_installed(void) {
    int ids[INSTALLED_BACKFILL], n = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0 || cJSON_GetObjectItemCaseSensitive(e, "platform_id") || n == INSTALLED_BACKFILL) continue;
        int seen = 0;
        for (int i = 0; i < n; i++) seen |= ids[i] == id;
        if (!seen) ids[n++] = id;
    }
    state_unlock();
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/api/roms/%d", ids[i]);
        long st = 0;
        cJSON *rom = romm_call("GET", path, NULL, &st);
        if (!rom) continue;
        state_lock();
        cJSON *x;
        cJSON_ArrayForEach(x, state_section("roms")) {
            if ((int)jget_num(x, "rom_id", 0) != ids[i]) continue;
            jset_num(x, "platform_id", jget_num(rom, "platform_id", 0));
            jset_str(x, "cover", jget_str(rom, "path_cover_small", ""));
        }
        state_save();
        state_unlock();
        cJSON_Delete(rom);
    }
}

/* A game's size here: its file, or everything in its folder. */
static double local_size(const char *path, int depth) {
    if (!dir_exists(path)) return (double)(file_size(path) > 0 ? file_size(path) : 0);
    double total = 0;
    DIR *d = depth < 3 ? opendir(path) : NULL;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PATH_MAX_LEN];
        path_join(p, sizeof p, path, de->d_name);
        total += local_size(p, depth + 1);
    }
    if (d) closedir(d);
    return total;
}

static int newest_first(const void *a, const void *b) {
    double x = jget_num(*(cJSON *const *)a, "played", 0), y = jget_num(*(cJSON *const *)b, "played", 0);
    return x < y ? 1 : x > y ? -1 : 0;
}

cJSON *library_installed(int limit) {
    backfill_installed();
    cJSON *out = cJSON_CreateArray();
    const cJSON *e, *sv;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0 || jget_num(e, "with", 0) > 0 || !(file_exists(e->string) || dir_exists(e->string))) continue;
        double played = (double)file_mtime(e->string);
        cJSON_ArrayForEach(sv, state_section("saves"))
            if ((int)jget_num(sv, "rom_id", 0) == id && jget_num(sv, "mtime", 0) > played) played = jget_num(sv, "mtime", 0);
        cJSON *dup = NULL, *o;
        cJSON_ArrayForEach(o, out) if ((int)jget_num(o, "id", 0) == id) dup = o;
        if (dup) {
            if (played > jget_num(dup, "played", 0)) jset_num(dup, "played", played);
            continue;
        }
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", id);
        cJSON_AddStringToObject(o, "name", jget_str(e, "name", ""));
        cJSON_AddStringToObject(o, "fs_name", jget_str(e, "fs_name", ""));
        cJSON_AddNumberToObject(o, "platform_id", jget_num(e, "platform_id", 0));
        cJSON_AddStringToObject(o, "cover", jget_str(e, "cover", ""));
        cJSON_AddStringToObject(o, "profile", jget_str(e, "profile", ""));
        cJSON_AddNumberToObject(o, "played", played);
        cJSON_AddStringToObject(o, "path", e->string); /* sized below, off the lock */
        cJSON_AddItemToArray(out, o);
    }
    state_unlock();
    /* Newest first. */
    int n = cJSON_GetArraySize(out), k = 0;
    cJSON **all = malloc(sizeof *all * (size_t)(n ? n : 1)), *o;
    cJSON *sorted = cJSON_CreateArray();
    if (!all) {
        cJSON_Delete(out);
        return sorted;
    }
    while ((o = cJSON_DetachItemFromArray(out, 0))) all[k++] = o;
    cJSON_Delete(out);
    qsort(all, (size_t)n, sizeof *all, newest_first);
    for (k = 0; k < n; k++) {
        if (k < limit) {
            cJSON_AddNumberToObject(all[k], "size", local_size(jget_str(all[k], "path", ""), 0));
            cJSON_DeleteItemFromObjectCaseSensitive(all[k], "path");
            cJSON_AddItemToArray(sorted, all[k]);
        } else {
            cJSON_Delete(all[k]);
        }
    }
    free(all);
    return sorted;
}

/* Whether a game is downloaded: a local file or folder the state maps to it.
 * Looked at after the state lock, like the fingerprint below. */
#define ROM_PLACES 8 /* one per emulator that has it, a few at most */
int library_on_console(int id) {
    char paths[ROM_PLACES][PATH_MAX_LEN];
    int n = 0;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms"))
        if ((int)jget_num(e, "rom_id", -1) == id && n < ROM_PLACES) str_copy(paths[n++], PATH_MAX_LEN, e->string);
    state_unlock();
    for (int i = 0; i < n; i++)
        if (file_exists(paths[i]) || dir_exists(paths[i])) return 1;
    return 0;
}

/* The installed fingerprint, worked out lazily and kept: the status that
 * carries it is asked for every few seconds, and that never touches the
 * disk. It's looked at again at most every INSTALLED_SIG_SEC, never while a
 * game is running, and at once after our own downloads and deletes. The
 * paths are copied out under the state lock and looked at after it, so a
 * slow or sleeping drive never holds anyone else up. */
#define INSTALLED_SIG_SEC 15
static pthread_mutex_t g_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_sig;
static double g_sig_at;

unsigned library_installed_sig(void) {
    /* Someone else is working it out: theirs will do, this one says the last. */
    if (pthread_mutex_trylock(&g_sig_lock) != 0) return g_sig;
    const double now = mono_now();
    const char *env = getenv("ROMM_SYNC_INSTALLED_SIG_SEC"); /* shorter in tests */
    const double every = env && atof(env) > 0 ? atof(env) : INSTALLED_SIG_SEC;
    if (!g_sig_stale && (now - g_sig_at < every || sync_game_running())) {
        pthread_mutex_unlock(&g_sig_lock);
        return g_sig;
    }
    g_sig_stale = 0;
    int n = 0, cap = 0;
    struct { int id; char *path; } *all = NULL;
    const cJSON *e;
    state_lock();
    cJSON_ArrayForEach(e, state_section("roms")) {
        int id = (int)jget_num(e, "rom_id", 0);
        if (id <= 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            void *grown = realloc(all, sizeof *all * (size_t)cap);
            if (!grown) break;
            all = grown;
        }
        all[n].id = id;
        all[n].path = strdup(e->string);
        if (all[n].path) n++;
    }
    state_unlock();
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) {
        if (file_exists(all[i].path) || dir_exists(all[i].path)) h = (h ^ (unsigned)all[i].id) * 16777619u;
        free(all[i].path);
    }
    free(all);
    g_sig = h;
    g_sig_at = now;
    pthread_mutex_unlock(&g_sig_lock);
    return h;
}

/* Recent library pages, so going back, changing letters or reopening the UI
 * doesn't fetch the same games again over a slow link. Installed flags are
 * worked out again on every hit. */
#define PAGE_CACHE_SLOTS 32
#define PAGE_CACHE_SEC   300
static struct {
    char key[1100]; /* the request path (up to 1024) and its offset and limit */
    double at;
    cJSON *page;
} g_pages[PAGE_CACHE_SLOTS];
static pthread_mutex_t g_pages_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *page_cache_get(const char *key) {
    cJSON *hit = NULL;
    double now = mono_now();
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++)
        if (g_pages[i].page && !strcmp(g_pages[i].key, key) && now - g_pages[i].at < PAGE_CACHE_SEC)
            hit = cJSON_Duplicate(g_pages[i].page, 1);
    pthread_mutex_unlock(&g_pages_lock);
    return hit;
}

static void page_cache_put(const char *key, const cJSON *page) {
    int slot = 0;
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS; i++) {
        if (!g_pages[i].page || !strcmp(g_pages[i].key, key)) {
            slot = i;
            break;
        }
        if (g_pages[i].at < g_pages[slot].at) slot = i;
    }
    cJSON_Delete(g_pages[slot].page);
    str_copy(g_pages[slot].key, sizeof g_pages[slot].key, key);
    g_pages[slot].at = mono_now();
    g_pages[slot].page = cJSON_Duplicate(page, 1);
    pthread_mutex_unlock(&g_pages_lock);
}

/* Keeps only what the library page shows of each game. */
static int add_library_rom(const cJSON *it, const cJSON *page, int first, void *ctx) {
    cJSON *out = ctx;
    if (first && !cJSON_GetObjectItemCaseSensitive(out, "total")) {
        cJSON_AddNumberToObject(out, "total", jget_num(page, "total", 0));
        /* First letter -> offset of its first game, for the A-Z ribbon. */
        const cJSON *ci = cJSON_GetObjectItemCaseSensitive(page, "char_index");
        if (cJSON_IsObject(ci)) cJSON_AddItemToObject(out, "char_index", cJSON_Duplicate(ci, 1));
    }
    if (!it) return 0;
    int id = (int)jget_num(it, "id", 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", jget_str(it, "name", jget_str(it, "fs_name", "")));
    cJSON_AddStringToObject(o, "fs_name", jget_str(it, "fs_name", ""));
    cJSON_AddNumberToObject(o, "size", jget_num(it, "fs_size_bytes", 0));
    cJSON_AddStringToObject(o, "cover", jget_str(it, "path_cover_small", ""));
    if (!library_not_game(jget_str(it, "fs_name", "")))
        covers_warm(jget_str(it, "path_cover_small", "")); /* fetched ahead, in the background */
    cJSON_AddNumberToObject(o, "platform_id", jget_num(it, "platform_id", 0));
    cJSON_AddBoolToObject(o, "multi", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "has_multiple_files")));
    cJSON_AddBoolToObject(o, "installed", library_on_console(id));
    /* A Switch game's update or DLC comes with the game, and isn't listed. */
    const int nx = is_switch(jget_str(it, "platform_slug", ""));
    unsigned long long game;
    const nx_kind k = nx ? nx_title(jget_str(it, "fs_name", ""), &game, NULL) : NX_NONE;
    if (library_not_game(jget_str(it, "fs_name", "")) || k == NX_UPDATE || k == NX_DLC) cJSON_AddBoolToObject(o, "not_game", 1);
    if (k == NX_UPDATE || k == NX_DLC) cJSON_AddStringToObject(o, "addon", k == NX_UPDATE ? "update" : "dlc");
    if (rom_archive(it, nx)) cJSON_AddBoolToObject(o, "archived", 1);
    if (nx) {
        /* What nx_collapse needs (and takes out again): the game's title id
         * and the entries RomM counts as versions of the same game. */
        char id[20];
        if (k == NX_GAME) {
            snprintf(id, sizeof id, "%016llX", game);
            cJSON_AddStringToObject(o, "_game", id);
        }
        cJSON *sib = cJSON_AddArrayToObject(o, "_sib");
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(it, "sibling_roms"))
            cJSON_AddItemToArray(sib, cJSON_CreateNumber(jget_num(s, "id", 0)));
    } else {
        add_version(o, it); /* a Switch game's "versions" are its files: one card */
    }
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(out, "items"), o);
    return 0;
}

/* A Switch game RomM keeps as several entries (the same game as an .nsp and an
 * .nsz, a .rar beside its files, two dumps) shows as one card: the one on the
 * console, else not an archive, else named by its title id, else the biggest.
 * The others are hidden like updates and DLC. Entries go together when RomM
 * counts them as versions of the same game, or they carry the same title id;
 * only within a page, where the A-Z order puts them side by side. */
static int nx_same(const cJSON *a, const cJSON *b) {
    const char *ga = jget_str(a, "_game", ""), *gb = jget_str(b, "_game", "");
    if (ga[0] && !strcmp(ga, gb)) return 1;
    const int ia = (int)jget_num(a, "id", 0), ib = (int)jget_num(b, "id", 0);
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(a, "_sib")) if ((int)s->valuedouble == ib) return 1;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(b, "_sib")) if ((int)s->valuedouble == ia) return 1;
    return 0;
}

static double nx_rank(const cJSON *o) {
    return (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "installed")) ? 8e15 : 0) +
           (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "archived")) ? 0 : 4e15) +
           (jget_str(o, "_game", "")[0] ? 2e15 : 0) + jget_num(o, "size", 0);
}

static void nx_collapse(cJSON *page) {
    cJSON *items = cJSON_GetObjectItemCaseSensitive(page, "items");
    const int n = cJSON_GetArraySize(items);
    int *group = malloc(sizeof *group * (size_t)(n ? n : 1));
    cJSON **at = malloc(sizeof *at * (size_t)(n ? n : 1));
    if (!group || !at) {
        free(group);
        free(at);
        return;
    }
    for (int i = 0; i < n; i++) {
        at[i] = cJSON_GetArrayItem(items, i);
        group[i] = i;
    }
    /* Each shown Switch entry joins the group of the first one before it that
     * it goes with. */
    for (int i = 0; i < n; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(at[i], "_sib") || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(at[i], "not_game")))
            continue;
        for (int j = 0; j < i; j++)
            if (cJSON_GetObjectItemCaseSensitive(at[j], "_sib") &&
                !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(at[j], "not_game")) && nx_same(at[i], at[j])) {
                group[i] = group[j];
                break;
            }
    }
    for (int g = 0; g < n; g++) {
        int best = -1, members = 0;
        for (int i = 0; i < n; i++)
            if (group[i] == g) {
                members++;
                if (best < 0 || nx_rank(at[i]) > nx_rank(at[best])) best = i;
            }
        if (members < 2) continue;
        for (int i = 0; i < n; i++)
            if (group[i] == g && i != best) {
                cJSON_DeleteItemFromObjectCaseSensitive(at[i], "not_game");
                cJSON_AddBoolToObject(at[i], "not_game", 1);
                cJSON_AddNumberToObject(at[i], "same_as", jget_num(at[best], "id", 0));
            }
    }
    for (int i = 0; i < n; i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(at[i], "_game");
        cJSON_DeleteItemFromObjectCaseSensitive(at[i], "_sib");
    }
    free(group);
    free(at);
}

/* The pages either side of the one just shown, fetched in the background so
 * scrolling on (or back up, after a jump to a letter) finds them here. RomM
 * takes a second or two for a page of a big platform. One thread, working
 * from the latest page shown: what was asked for meanwhile is dropped. */
static struct {
    int busy, pending, platform_id, offset, limit, total;
    char path[sizeof g_pages[0].key], search[256];
    pthread_t thread;
} g_near;

static int page_cached(const char *path, int offset, int limit) {
    char key[sizeof g_pages[0].key];
    snprintf(key, sizeof key, "%s&offset=%d&limit=%d", path, offset, limit);
    int hit = 0;
    double now = mono_now();
    pthread_mutex_lock(&g_pages_lock);
    for (int i = 0; i < PAGE_CACHE_SLOTS && !hit; i++)
        hit = g_pages[i].page && !strcmp(g_pages[i].key, key) && now - g_pages[i].at < PAGE_CACHE_SEC / 2;
    pthread_mutex_unlock(&g_pages_lock);
    return hit;
}

static void *near_pages_thread(void *arg) {
    (void)arg;
    char err[160], path[sizeof g_near.path], search[sizeof g_near.search];
    pthread_mutex_lock(&g_pages_lock);
    g_near.thread = pthread_self();
    while (g_near.pending) {
        g_near.pending = 0;
        int platform_id = g_near.platform_id, offset = g_near.offset, limit = g_near.limit, total = g_near.total;
        str_copy(path, sizeof path, g_near.path);
        str_copy(search, sizeof search, g_near.search);
        pthread_mutex_unlock(&g_pages_lock);
        /* Down first, two pages: that's the way most scrolling goes. */
        const int near[3] = {offset + limit, offset + 2 * limit, offset - limit};
        for (int k = 0; k < 3; k++) {
            if (near[k] < 0 || near[k] >= total || page_cached(path, near[k], limit)) continue;
            cJSON_Delete(library_roms(platform_id, search, near[k], limit, err, sizeof err));
            pthread_mutex_lock(&g_pages_lock);
            int moved = g_near.pending;
            pthread_mutex_unlock(&g_pages_lock);
            if (moved) break;
        }
        pthread_mutex_lock(&g_pages_lock);
    }
    g_near.busy = 0;
    memset(&g_near.thread, 0, sizeof g_near.thread);
    pthread_mutex_unlock(&g_pages_lock);
    return NULL;
}

static void fetch_near_pages(const char *path, int platform_id, const char *search, int offset, int limit, int total) {
    if (!(offset + limit < total && !page_cached(path, offset + limit, limit)) &&
        !(offset - limit >= 0 && !page_cached(path, offset - limit, limit)))
        return;
    pthread_mutex_lock(&g_pages_lock);
    if (g_near.busy && pthread_equal(g_near.thread, pthread_self())) {
        pthread_mutex_unlock(&g_pages_lock); /* its own fetches don't ask for more */
        return;
    }
    g_near.pending = 1;
    g_near.platform_id = platform_id;
    g_near.offset = offset;
    g_near.limit = limit;
    g_near.total = total;
    str_copy(g_near.path, sizeof g_near.path, path);
    str_copy(g_near.search, sizeof g_near.search, search ? search : "");
    int start = !g_near.busy;
    g_near.busy = 1;
    pthread_mutex_unlock(&g_pages_lock);
    if (start && thread_start(near_pages_thread, NULL) != 0) {
        pthread_mutex_lock(&g_pages_lock);
        g_near.busy = 0;
        pthread_mutex_unlock(&g_pages_lock);
    }
}

cJSON *library_roms(int platform_id, const char *search, int offset, int limit, char *err, int en) {
    char path[1024];
    char *q = http_escape(search ? search : "");
    /* Platform 0: every platform (a search across the library). */
    if (platform_id > 0)
        snprintf(path, sizeof path, "/api/roms?platform_ids=%d&search_term=%s&order_by=name&order_dir=asc" ROMS_LEAN, platform_id, q);
    else
        snprintf(path, sizeof path, "/api/roms?search_term=%s&order_by=name&order_dir=asc" ROMS_LEAN, q);
    free(q);
    char key[sizeof g_pages[0].key];
    snprintf(key, sizeof key, "%s&offset=%d&limit=%d", path, offset, limit);
    cJSON *out = page_cache_get(key), *it;
    if (out) {
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(out, "items"))
            cJSON_ReplaceItemInObjectCaseSensitive(it, "installed", cJSON_CreateBool(library_on_console((int)jget_num(it, "id", 0))));
        fetch_near_pages(path, platform_id, search, offset, limit, (int)jget_num(out, "total", 0));
        return out;
    }
    out = cJSON_CreateObject();
    cJSON_AddArrayToObject(out, "items");
    long st = 0;
    if (romm_list(path, offset, limit, add_library_rom, out, &st) < 0) {
        if (st == 200) snprintf(err, (size_t)en, "could not read RomM's list of games (see the log)");
        else if (st || !romm_no_answer(err, en)) snprintf(err, (size_t)en, "could not load games (HTTP %ld)", st);
        cJSON_Delete(out);
        return NULL;
    }
    if (!cJSON_GetObjectItemCaseSensitive(out, "total")) cJSON_AddNumberToObject(out, "total", 0);
    nx_collapse(out);
    page_cache_put(key, out);
    fetch_near_pages(path, platform_id, search, offset, limit, (int)jget_num(out, "total", 0));
    return out;
}
