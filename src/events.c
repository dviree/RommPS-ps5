#include "events.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform.h"
#include "state.h"
#include "util.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_events; /* oldest first */
static char g_sync[64] = "";
static int g_dirty;

static void path_of(char *out, size_t n) { path_join(out, n, plat_data_dir(), "events.json"); }

void events_load(void) {
    char path[PATH_MAX_LEN];
    path_of(path, sizeof path);
    char *txt = read_file(path, NULL);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    pthread_mutex_lock(&g_lock);
    cJSON_Delete(g_events);
    g_events = cJSON_IsArray(j) ? j : cJSON_CreateArray();
    if (!cJSON_IsArray(j)) cJSON_Delete(j);
    pthread_mutex_unlock(&g_lock);
}

void events_save(void) {
    pthread_mutex_lock(&g_lock);
    char *txt = g_dirty && g_events ? cJSON_PrintUnformatted(g_events) : NULL;
    g_dirty = 0;
    pthread_mutex_unlock(&g_lock);
    if (!txt) return;
    char path[PATH_MAX_LEN];
    path_of(path, sizeof path);
    if (write_file_atomic(path, txt, strlen(txt)) != 0) LOGW("could not write %s", path);
    free(txt);
}

void events_set_sync(const char *reason) {
    pthread_mutex_lock(&g_lock);
    str_copy(g_sync, sizeof g_sync, reason ? reason : "");
    pthread_mutex_unlock(&g_lock);
}

void events_add(const char *kind, const char *action, int rom_id, const char *path, const char *emulator,
                const char *detail) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "t", (double)time(NULL));
    cJSON_AddNumberToObject(e, "rom_id", rom_id);
    cJSON_AddStringToObject(e, "kind", kind);
    cJSON_AddStringToObject(e, "action", action);
    cJSON_AddStringToObject(e, "path", path ? path : "");
    cJSON_AddStringToObject(e, "emulator", emulator ? emulator : "");
    if (detail && detail[0]) cJSON_AddStringToObject(e, "detail", detail);
    pthread_mutex_lock(&g_lock);
    if (!g_events) g_events = cJSON_CreateArray();
    cJSON_AddStringToObject(e, "sync", g_sync);
    cJSON_AddItemToArray(g_events, e);
    while (cJSON_GetArraySize(g_events) > EVENTS_MAX) cJSON_DeleteItemFromArray(g_events, 0);
    g_dirty = 1;
    pthread_mutex_unlock(&g_lock);
}

cJSON *events_json(int rom_id, int limit) {
    cJSON *out = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    cJSON *copy = g_events ? cJSON_Duplicate(g_events, 1) : cJSON_CreateArray();
    pthread_mutex_unlock(&g_lock);
    state_lock();
    const cJSON *roms = state_section("roms");
    for (int i = cJSON_GetArraySize(copy) - 1; i >= 0 && (limit <= 0 || cJSON_GetArraySize(out) < limit); i--) {
        cJSON *e = cJSON_GetArrayItem(copy, i);
        int id = (int)jget_num(e, "rom_id", 0);
        if (rom_id && id != rom_id) continue;
        const cJSON *r, *game = NULL;
        cJSON_ArrayForEach(r, roms) if ((int)jget_num(r, "rom_id", -1) == id && id) game = r;
        cJSON *o = cJSON_Duplicate(e, 1);
        cJSON_AddStringToObject(o, "game", game ? jget_str(game, "name", "") : "");
        cJSON_AddStringToObject(o, "platform", game ? jget_str(game, "platform_dir", "") : "");
        cJSON_AddItemToArray(out, o);
    }
    state_unlock();
    cJSON_Delete(copy);
    return out;
}
