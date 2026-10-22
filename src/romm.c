#include "romm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "platform.h"
#include "state.h"
#include "util.h"

static void current_auth(char *base, size_t bn, char *auth, size_t an) {
    config_lock();
    str_copy(base, bn, g_cfg.server_url);
    if (g_cfg.token[0])
        snprintf(auth, an, "Bearer %s", g_cfg.token);
    else
        auth[0] = 0;
    config_unlock();
}

/* What a game list carries that nothing here reads, and that makes it heavy:
 * RetroAchievements' lists alone are most of a page (1.1 of 1.3 MB for 16 PS1
 * games), and the PS4 payload can't hold that. Dropped as the page arrives. */
static const char *const LIST_DROP[] = {
    "merged_ra_metadata", "ra_metadata",        "ss_metadata",     "igdb_metadata",
    "hasheous_metadata",  "launchbox_metadata", "moby_metadata",   "flashpoint_metadata",
    "gamelist_metadata",  "merged_screenshots", "url_screenshots", "url_manual",
    NULL};

static cJSON *call(const char *base, const char *auth, const char *method, const char *path, const cJSON *body,
                   const char *const *drop, long *status) {
    char url[2048];
    snprintf(url, sizeof url, "%s%s", base, path);
    char *payload = body ? cJSON_PrintUnformatted(body) : NULL;
    http_resp r;
    int rc = drop ? http_get_lean(url, auth, drop, &r)
                  : http_request(method, url, auth, payload ? "application/json" : NULL, payload,
                                 payload ? strlen(payload) : 0, &r);
    free(payload);
    if (status) *status = rc == 0 ? r.status : 0;
    cJSON *j = (rc == 0 && r.body) ? cJSON_Parse(r.body) : NULL;
    /* A big reply can't always be parsed where memory is tight (the PS4 payload
     * shares GoldHEN's process); romm_list pages lists to stay small. */
    if (rc == 0 && r.status < 300 && r.len && !j)
        LOGW("%s %s -> %ld, but the %zu-byte reply couldn't be read (out of memory? largest free block %d MB)", method,
             path, r.status, r.len, mem_largest_mb());
    if (rc == 0 && r.status >= 400) {
        char d[256];
        romm_error_detail(j, d, sizeof d);
        /* Normal until the user approves the pairing. */
        if (strcmp(d, "authorization_pending") != 0) LOGW("%s %s -> %ld %s", method, path, r.status, d);
    }
    http_resp_free(&r);
    return j;
}

cJSON *romm_call_raw(const char *base, const char *auth, const char *method, const char *path,
                     const cJSON *body, long *status) {
    return call(base, auth, method, path, body, NULL, status);
}

int romm_no_answer(char *msg, int n) {
    char base[512];
    config_lock();
    str_copy(base, sizeof base, g_cfg.server_url);
    config_unlock();
    if (!base[0] || !http_down(base)) return 0;
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    snprintf(msg, (size_t)n, "RomM at %.*s isn't answering", (int)strcspn(host, "/"), host);
    return 1;
}

static pthread_mutex_t g_check_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_checking;

static void *check_back(void *arg) {
    (void)arg;
    long st = 0;
    cJSON_Delete(romm_call("GET", "/api/heartbeat", NULL, &st));
    pthread_mutex_lock(&g_check_lock);
    g_checking = 0;
    pthread_mutex_unlock(&g_check_lock);
    return NULL;
}

void romm_check_back(void) {
    pthread_mutex_lock(&g_check_lock);
    const int start = !g_checking;
    g_checking = 1;
    pthread_mutex_unlock(&g_check_lock);
    if (start && thread_start(check_back, NULL) != 0) {
        pthread_mutex_lock(&g_check_lock);
        g_checking = 0;
        pthread_mutex_unlock(&g_check_lock);
    }
}

int romm_list(const char *path, int offset, int limit, romm_list_cb cb, void *ctx, long *status) {
    int seen = 0, page_size = strcmp(plat_info()->name, "ps4") ? 60 : 16;
    char why[160];
    if (status) *status = 0;
    while (seen < limit) {
        char url[2048];
        int want = limit - seen < page_size ? limit - seen : page_size;
        snprintf(url, sizeof url, "%s%soffset=%d&limit=%d", path, strchr(path, '?') ? "&" : "?", offset + seen, want);
        long st = 0;
        char base[512], auth[300];
        current_auth(base, sizeof base, auth, sizeof auth);
        cJSON *page = base[0] ? call(base, auth, "GET", url, NULL, LIST_DROP, &st) : NULL;
        if (status) *status = st;
        /* A page too big to take in (games with a lot of metadata, where memory
         * is tight): the same games again, fewer at a time. Not when RomM
         * didn't answer at all: fewer games won't change that. */
        if ((st == 0 || (st == 200 && !page)) && want > 1 && !(st == 0 && romm_no_answer(why, sizeof why))) {
            cJSON_Delete(page);
            page_size = want / 2;
            LOGI("%s: trying %d games at a time", path, page_size);
            continue;
        }
        if (st != 200 || !page) {
            cJSON_Delete(page);
            return -1;
        }
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(page, "items"), *it;
        int got = cJSON_GetArraySize(items), stop = 0;
        cJSON_ArrayForEach(it, items) {
            if ((stop = cb(it, page, seen == 0, ctx)) != 0) break;
        }
        if (got == 0 && seen == 0) cb(NULL, page, 1, ctx); /* an empty list still has a total */
        cJSON_Delete(page);
        seen += got;
        if (stop || got < want) break;
    }
    return seen;
}

cJSON *romm_call(const char *method, const char *path, const cJSON *body, long *status) {
    char base[512], auth[300];
    current_auth(base, sizeof base, auth, sizeof auth);
    if (!base[0]) {
        if (status) *status = 0;
        return NULL;
    }
    return romm_call_raw(base, auth, method, path, body, status);
}

void romm_error_detail(const cJSON *j, char *out, int n) {
    out[0] = 0;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "detail");
    if (cJSON_IsString(d)) {
        str_copy(out, (size_t)n, d->valuestring);
    } else if (d) {
        char *s = cJSON_PrintUnformatted(d);
        str_copy(out, (size_t)n, s);
        free(s);
    }
}

int romm_heartbeat(const char *base, char *version, int vn) {
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "GET", "/api/heartbeat", NULL, &st);
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(j, "SYSTEM");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(sys, "VERSION");
    int ok = st == 200 && cJSON_IsString(v);
    if (ok && version) str_copy(version, (size_t)vn, v->valuestring);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}

static cJSON *scopes_array(void) {
    static const char *scopes[] = {ROMM_SCOPES};
    return cJSON_CreateStringArray(scopes, (int)(sizeof scopes / sizeof scopes[0]));
}

int romm_device_init(const char *base, romm_device_flow *flow) {
    cJSON *body = cJSON_CreateObject();
    config_lock();
    cJSON_AddStringToObject(body, "client_device_identifier", g_cfg.client_device_identifier);
    cJSON_AddStringToObject(body, "name", g_cfg.device_name);
    config_unlock();
    cJSON_AddStringToObject(body, "client", plat_info()->client);
    cJSON_AddStringToObject(body, "platform", plat_info()->console);
    cJSON_AddStringToObject(body, "client_version", APP_VERSION);
    cJSON_AddItemToObject(body, "requested_scopes", scopes_array());
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/auth/device/init", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    if ((st == 200 || st == 201) && j) {
        memset(flow, 0, sizeof *flow);
        str_copy(flow->device_code, sizeof flow->device_code, jget_str(j, "device_code", ""));
        str_copy(flow->user_code, sizeof flow->user_code, jget_str(j, "user_code", ""));
        snprintf(flow->verification_url, sizeof flow->verification_url, "%s%s", base,
                 jget_str(j, "verification_path_complete", ""));
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(j, "interval");
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
        flow->interval = cJSON_IsNumber(iv) && iv->valueint > 0 ? iv->valueint : 5;
        flow->expires_in = cJSON_IsNumber(ex) ? ex->valueint : 600;
        rc = flow->device_code[0] ? 0 : -1;
    }
    cJSON_Delete(j);
    return rc;
}

romm_poll romm_device_poll(const char *base, const char *device_code, char *token, int tn,
                           char *device_id, int dn) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_code", device_code);
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/auth/device/token", body, &st);
    cJSON_Delete(body);
    romm_poll res = POLL_ERROR;
    if (st == 200 && j) {
        str_copy(token, (size_t)tn, jget_str(j, "access_token", ""));
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "device_id");
        if (cJSON_IsString(d))
            str_copy(device_id, (size_t)dn, d->valuestring);
        else if (cJSON_IsNumber(d))
            snprintf(device_id, (size_t)dn, "%d", d->valueint);
        res = token[0] ? POLL_OK : POLL_ERROR;
    } else if (st >= 400 && st < 500) {
        char d[128];
        romm_error_detail(j, d, sizeof d);
        if (strstr(d, "authorization_pending")) res = POLL_PENDING;
        else if (strstr(d, "slow_down") || st == 429) res = POLL_SLOW_DOWN;
        else if (strstr(d, "access_denied")) res = POLL_DENIED;
        else if (strstr(d, "expired")) res = POLL_EXPIRED;
    }
    cJSON_Delete(j);
    return res;
}

int romm_pair_exchange(const char *base, const char *code, char *token, int tn) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "code", code);
    long st = 0;
    cJSON *j = romm_call_raw(base, NULL, "POST", "/api/client-tokens/exchange", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    if (st == 200 && j) {
        str_copy(token, (size_t)tn, jget_str(j, "raw_token", ""));
        rc = token[0] ? 0 : -1;
    }
    cJSON_Delete(j);
    return rc;
}

void romm_update_device_name(void) {
    char name[64], id[64], path[128];
    config_lock();
    int same = !strcmp(g_cfg.device_name, g_cfg.romm_device_name);
    str_copy(name, sizeof name, g_cfg.device_name);
    str_copy(id, sizeof id, g_cfg.device_id);
    config_unlock();
    if (same || !name[0] || !id[0]) return;
    char *q = http_escape(id);
    snprintf(path, sizeof path, "/api/devices/%s", q);
    free(q);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", name);
    long st = 0;
    cJSON_Delete(romm_call("PUT", path, body, &st));
    cJSON_Delete(body);
    /* No such device or no renaming on this RomM: nothing to retry. */
    if (st != 200 && st != 404 && st != 405) {
        if (st) LOGW("could not rename the device on RomM (HTTP %ld)", st);
        return;
    }
    if (st == 200) LOGI("device is called \"%s\" on RomM now", name);
    config_lock();
    str_copy(g_cfg.romm_device_name, sizeof g_cfg.romm_device_name, name);
    config_save();
    config_unlock();
}

int romm_register_device(const char *base, const char *token, char *device_id, int dn) {
    char auth[300], ip[64] = "";
    snprintf(auth, sizeof auth, "Bearer %s", token);
    plat_local_ip(ip, sizeof ip);
    cJSON *body = cJSON_CreateObject();
    config_lock();
    cJSON_AddStringToObject(body, "name", g_cfg.device_name);
    config_unlock();
    cJSON_AddStringToObject(body, "platform", plat_info()->console);
    cJSON_AddStringToObject(body, "client", plat_info()->client);
    cJSON_AddStringToObject(body, "client_version", APP_VERSION);
    if (ip[0]) cJSON_AddStringToObject(body, "ip_address", ip);
    cJSON_AddStringToObject(body, "sync_mode", "api");
    cJSON_AddBoolToObject(body, "allow_existing", 1);
    long st = 0;
    cJSON *j = romm_call_raw(base, auth, "POST", "/api/devices", body, &st);
    cJSON_Delete(body);
    int rc = -1;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "device_id");
    if (!d) d = cJSON_GetObjectItemCaseSensitive(j, "id");
    if ((st == 200 || st == 201) && d) {
        if (cJSON_IsString(d))
            str_copy(device_id, (size_t)dn, d->valuestring);
        else
            snprintf(device_id, (size_t)dn, "%d", d->valueint);
        rc = 0;
    }
    cJSON_Delete(j);
    return rc;
}

/* A transfer cut off halfway (RomM restarted, paused, the Wi-Fi dropped)
 * picks up where it stopped, for as long as each go gets further. One that
 * gets nothing more ends it: RomM is away, and the download waits for it. */
#define DOWNLOAD_TRIES 5

int romm_download(const char *path, const char *dest, http_progress_fn cb, void *ud,
                  http_resp *resp) {
    char base[512], auth[300], url[2048], part[PATH_MAX_LEN];
    current_auth(base, sizeof base, auth, sizeof auth);
    snprintf(url, sizeof url, "%s%s", base, path);
    snprintf(part, sizeof part, "%s.part", dest);
    int rc = -1;
    for (int go = 1; go <= DOWNLOAD_TRIES; go++) {
        const int64_t before = file_size(part);
        rc = http_download(url, auth, dest, 1, cb, ud, resp);
        if (rc != 0 && resp->status == 404)
            /* RomM still lists it, its disk doesn't have it: renamed or moved since its last scan. */
            snprintf(resp->error, sizeof resp->error,
                     "RomM lists this game, but %.120s is missing from its server. If it was moved or renamed, "
                     "rescan the platform in RomM",
                     path_basename(dest));
        if (rc == 0 || resp->status || resp->no_answer || resp->aborted) break;
        const int64_t after = file_size(part);
        if (after <= (before > 0 ? before : 0)) break;
        LOGI("download of %s cut off at %lld bytes (%s): carrying on", path_basename(dest), (long long)after,
             resp->error);
        sleep(2);
    }
    return rc;
}

cJSON *romm_upload(const char *method, const char *path, const char *field, const char *file,
                   const char *upload_name, long *status) {
    char base[512], auth[300], url[2048];
    current_auth(base, sizeof base, auth, sizeof auth);
    snprintf(url, sizeof url, "%s%s", base, path);
    http_resp r;
    int rc = http_upload_file(method, url, auth, field, file, upload_name, &r);
    if (status) *status = rc == 0 ? r.status : 0;
    cJSON *j = (rc == 0 && r.body) ? cJSON_Parse(r.body) : NULL;
    if (rc == 0 && r.status >= 400) {
        char d[256];
        romm_error_detail(j, d, sizeof d);
        LOGW("%s %s -> %ld %s", method, path, r.status, d);
    }
    http_resp_free(&r);
    return j;
}
