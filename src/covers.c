/* Covers and platform icons on the console. See covers.h. */
#include "covers.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "config.h"
#include "http.h"
#include "library.h"
#include "platform.h"
#include "romm.h"
#include "state.h"
#include "sync.h"
#include "util.h"

/* An asset RomM doesn't have is remembered for a day (a .missing file), so it
 * isn't asked for again on every page. The folder is pruned, oldest first,
 * past its limit: on the consoles, room for some 30,000 covers (66 KB each on
 * average). The full pass stops short of it. */
#define MISSING_SEC (24 * 60 * 60)
#define FULL_EVERY_SEC (24 * 60 * 60)
#define WARM_SLOTS 256

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_prune; /* the cache went past its limit */
static long long g_cached_bytes = -1; /* -1 until counted */

/* Covers to fetch soon (the pages being looked at), newest first. */
static char g_warm[WARM_SLOTS][512];
static int g_nwarm;

/* The full pass: every game's cover path, worked through by the workers. It
 * stays on disk, a line each in cache/cover-pass (twenty thousand paths are
 * more than the PS4 can spare), with its size and counts in cover-pass.json
 * and how far it got in cover-pass.pos, so a restart carries on from there. */
static FILE *g_pass;
static int g_nall, g_next, g_done, g_total; /* g_total: the pass's size, after it's gone */
/* The library was being looked at: the pass waits, its pages' covers don't. */
static time_t g_browse_at;
#define BROWSE_PAUSE_SEC 15
static int g_refresh; /* a full pass was asked for */
static char g_state[16] = "idle";

static int is_ps5(void) { return strcmp(plat_info()->name, "ps5") == 0; }

static long long cache_limit(void) { return strcmp(plat_info()->name, "host") ? 2048LL * 1024 * 1024 : 300LL * 1024 * 1024; }

static int is_ps4(void) { return strcmp(plat_info()->name, "ps4") == 0; }

/* The PS4 payload shares a small memory pool with GoldHEN: one cover at a time. */
static int workers(void) { return is_ps5() ? 4 : is_ps4() ? 1 : 2; }

static void cache_path(const char *p, const char *suffix, char *out, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a of the asset path and query */
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) h = (h ^ *c) * 0x100000001b3ULL;
    snprintf(out, n, "%s/cache/assets/%016llx%s", plat_data_dir(), (unsigned long long)h, suffix);
}

static const char *image_type(const char *p) {
    char path[1024];
    str_copy(path, sizeof path, p);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    if (str_ends_with_ci(path, ".png")) return "image/png";
    if (str_ends_with_ci(path, ".webp")) return "image/webp";
    if (str_ends_with_ci(path, ".svg")) return "image/svg+xml";
    if (str_ends_with_ci(path, ".ico")) return "image/x-icon";
    return "image/jpeg";
}

/* RomM's platform icons only carry a viewBox, and the console browser renders an
 * SVG without a size at 0x0: give it the viewBox's size. */
static char *svg_with_size(const char *svg, size_t len, size_t *out_len) {
    const char *tag = strstr(svg, "<svg");
    if (!tag) return NULL;
    const char *end = strchr(tag, '>');
    if (!end) return NULL;
    for (const char *q = tag; q < end; q++)
        if (!strncmp(q, " width=", 7)) return NULL; /* already sized */
    const char *vb = strstr(tag, "viewBox=\"");
    double x, y, w = 0, h = 0;
    if (!vb || vb > end || sscanf(vb + 9, "%lf %lf %lf %lf", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0) return NULL;
    char attrs[96];
    int al = snprintf(attrs, sizeof attrs, " width=\"%g\" height=\"%g\"", w, h);
    size_t pos = (size_t)(tag - svg) + 4; /* after "<svg" */
    char *outp = malloc(len + (size_t)al + 1);
    if (!outp) return NULL;
    memcpy(outp, svg, pos);
    memcpy(outp + pos, attrs, (size_t)al);
    memcpy(outp + pos + (size_t)al, svg + pos, len - pos);
    *out_len = len + (size_t)al;
    outp[*out_len] = 0;
    return outp;
}

/* Pruning goes by age without a list of the files, so it takes the same
 * little memory for a hundred covers as for twenty thousand: one pass sorts
 * their sizes into AGE_BINS slices of the folder's age span, the next deletes
 * the oldest slices, whole, until the rest fit. */
#define AGE_BINS 256

/* Calls fn on each cached file (the folder's own, not dotfiles). */
static void each_cached(const char *dir, void (*fn)(const char *path, const struct stat *st, void *ud), void *ud) {
    char path[PATH_MAX_LEN];
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        struct stat st;
        if (de->d_name[0] == '.') continue;
        path_join(path, sizeof path, dir, de->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) fn(path, &st, ud);
    }
    closedir(d);
}

struct age_scan {
    long long total, bins[AGE_BINS];
    time_t oldest, newest;
    int pass, cutoff; /* cutoff: the newest slice deleted */
};

static int age_bin(const struct age_scan *a, time_t t) {
    const long long span = (long long)(a->newest - a->oldest) + 1;
    const long long b = (long long)(t - a->oldest) * AGE_BINS / span;
    return b < 0 ? 0 : b >= AGE_BINS ? AGE_BINS - 1 : (int)b;
}

static void age_visit(const char *path, const struct stat *st, void *ud) {
    struct age_scan *a = ud;
    if (a->pass == 0) {
        a->total += st->st_size;
        if (!a->oldest || st->st_mtime < a->oldest) a->oldest = st->st_mtime;
        if (st->st_mtime > a->newest) a->newest = st->st_mtime;
    } else if (a->pass == 1) {
        a->bins[age_bin(a, st->st_mtime)] += st->st_size;
    } else if (age_bin(a, st->st_mtime) <= a->cutoff && unlink(path) == 0) {
        a->total -= st->st_size;
    }
}

/* The folder's size; and, past the limit, the oldest files deleted until it's
 * back under 95% of it. On the planner's thread. */
static long long cache_scan(int prune) {
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s/cache/assets", plat_data_dir());
    struct age_scan a;
    memset(&a, 0, sizeof a);
    each_cached(dir, age_visit, &a);
    if (prune && a.total > cache_limit()) {
        a.pass = 1;
        each_cached(dir, age_visit, &a);
        long long over = a.total - cache_limit() / 20 * 19, freed = 0;
        int last = 0;
        while (last < AGE_BINS - 1 && (freed += a.bins[last]) < over) last++;
        a.cutoff = last;
        a.pass = 2;
        each_cached(dir, age_visit, &a);
    }
    pthread_mutex_lock(&g_lock);
    g_cached_bytes = a.total;
    pthread_mutex_unlock(&g_lock);
    return a.total;
}

static void cache_store(const char *path, const void *body, size_t len) {
    char tmp[PATH_MAX_LEN + 32];
    snprintf(tmp, sizeof tmp, "%s.%lx.tmp", path, (unsigned long)pthread_self());
    mkdir_parent(path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(body, 1, len, f) == len;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_cached_bytes >= 0) g_cached_bytes += (long long)len;
    if (g_cached_bytes > cache_limit()) g_prune = 1; /* the planner prunes, not a request's thread */
    pthread_mutex_unlock(&g_lock);
}

static int valid_path(const char *p) { return p && !strncmp(p, "/assets/", 8) && !strstr(p, ".."); }

/* Whether an asset is on the console (or known to be missing on RomM). */
static int have(const char *p) {
    char cached[PATH_MAX_LEN], missing[PATH_MAX_LEN];
    cache_path(p, "", cached, sizeof cached);
    if (file_exists(cached)) return 1;
    cache_path(p, ".missing", missing, sizeof missing);
    time_t miss = file_mtime(missing);
    return miss && time(NULL) - miss < MISSING_SEC;
}

int covers_fetch(const char *p, char **out, size_t *out_len, const char **type) {
    char base[512], auth[300], url[3200], cached[PATH_MAX_LEN], missing[PATH_MAX_LEN];
    if (!valid_path(p)) return -2;
    *type = image_type(p);
    cache_path(p, "", cached, sizeof cached);
    cache_path(p, ".missing", missing, sizeof missing);
    size_t clen = 0;
    char *cbody = read_file(cached, &clen);
    if (cbody) {
        *out = cbody;
        *out_len = clen;
        return 0;
    }
    time_t miss = file_mtime(missing);
    if (miss && time(NULL) - miss < MISSING_SEC) return 1;
    config_lock();
    str_copy(base, sizeof base, g_cfg.server_url);
    snprintf(auth, sizeof auth, "Bearer %s", g_cfg.token);
    config_unlock();
    /* Cover paths come with literal spaces in them ("?ts=2026-08-03 07:20:30"),
     * which libcurl refuses: they get encoded. */
    size_t o = (size_t)snprintf(url, sizeof url, "%s", base);
    for (const unsigned char *c = (const unsigned char *)p; *c && o + 4 < sizeof url; c++) {
        if (*c <= 0x20 || *c >= 0x7f || *c == '"' || *c == '<' || *c == '>' || *c == '\\' || *c == '^' ||
            *c == '`' || *c == '{' || *c == '|' || *c == '}')
            o += (size_t)snprintf(url + o, sizeof url - o, "%%%02X", *c);
        else
            url[o++] = (char)*c;
    }
    url[o] = 0;
    http_resp resp;
    if (http_request("GET", url, auth, NULL, NULL, 0, &resp) != 0 || resp.status != 200) {
        int gone = resp.status == 404;
        if (gone) cache_store(missing, "", 0);
        http_resp_free(&resp);
        return gone ? 1 : -1;
    }
    unlink(missing);
    size_t len = resp.len;
    char *fixed = !strcmp(*type, "image/svg+xml") ? svg_with_size(resp.body, len, &len) : NULL;
    const char *body = fixed ? fixed : resp.body;
    cache_store(cached, body, len);
    *out = malloc(len + 1);
    if (*out) {
        memcpy(*out, body, len);
        (*out)[len] = 0;
        *out_len = len;
    }
    free(fixed);
    http_resp_free(&resp);
    return *out ? 0 : -1;
}

void covers_warm(const char *p) {
    pthread_mutex_lock(&g_lock);
    g_browse_at = time(NULL);
    pthread_mutex_unlock(&g_lock);
    if (!valid_path(p) || have(p)) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nwarm; i++)
        if (!strcmp(g_warm[i], p)) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    /* The newest first; the oldest drop off a full queue. */
    if (g_nwarm == WARM_SLOTS) g_nwarm--;
    memmove(g_warm[1], g_warm[0], sizeof g_warm[0] * (size_t)g_nwarm);
    str_copy(g_warm[0], sizeof g_warm[0], p);
    g_nwarm++;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

void covers_refresh(void) {
    pthread_mutex_lock(&g_lock);
    g_refresh = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

static int enabled(void) {
    config_lock();
    int on = g_cfg.cover_cache && config_is_paired() && g_cfg.setup_complete;
    config_unlock();
    return on;
}

/* The console is busy with something that matters more. */
static int busy(void) { return sync_game_running() || sync_is_running() || library_busy(); }

static void pass_path(char *out, size_t n, const char *suffix) {
    snprintf(out, n, "%s/cache/cover-pass%s", plat_data_dir(), suffix);
}

/* Where the pass got to: every so many covers, not each one. Call with g_lock held. */
static void save_pos(void) {
    char path[PATH_MAX_LEN], pos[64];
    long at = g_pass ? ftell(g_pass) : -1;
    if (at < 0) return;
    pass_path(path, sizeof path, ".pos");
    int len = snprintf(pos, sizeof pos, "%d %ld\n", g_next, at);
    write_file_atomic(path, pos, (size_t)len);
}

/* Each platform's game count when a pass last listed it: the daily pass
 * lists only the platforms whose count changed since (a cover replaced on
 * RomM comes down when its game is next looked at). The counts of a pass are
 * kept once its covers are done, so an unfinished one is listed again. */
static cJSON *g_pass_counts;

static void close_pass(void) {
    if (g_pass) fclose(g_pass);
    g_pass = NULL;
    g_nall = g_next = 0;
}

/* A pass that's over (done, or stopped by a full cache) goes, files and all.
 * Call with g_lock held. */
static void drop_pass(void) {
    char path[PATH_MAX_LEN];
    close_pass();
    static const char *const files[] = {"", ".json", ".pos"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        pass_path(path, sizeof path, files[i]);
        unlink(path);
    }
}

/* Opens the pass on disk where it was left (the top, for a new one). 0 if
 * there's one to carry on with. Call with g_lock held. */
static int open_pass(void) {
    char path[PATH_MAX_LEN];
    close_pass();
    pass_path(path, sizeof path, ".json");
    char *js = read_file(path, NULL);
    cJSON *head = js ? cJSON_Parse(js) : NULL;
    free(js);
    pass_path(path, sizeof path, "");
    g_pass = head ? fopen(path, "r") : NULL;
    if (!g_pass) {
        cJSON_Delete(head);
        return -1;
    }
    setvbuf(g_pass, NULL, _IOFBF, 4096);
    int next = 0;
    long at = 0;
    pass_path(path, sizeof path, ".pos");
    char *pos = read_file(path, NULL);
    if (!pos || sscanf(pos, "%d %ld", &next, &at) != 2 || next < 0 || at < 0 || fseek(g_pass, at, SEEK_SET) != 0) {
        next = 0;
        fseek(g_pass, 0, SEEK_SET);
    }
    free(pos);
    g_nall = g_total = (int)jget_num(head, "total", 0);
    g_next = g_done = next;
    cJSON_Delete(g_pass_counts);
    g_pass_counts = cJSON_DetachItemFromObjectCaseSensitive(head, "counts");
    cJSON_Delete(head);
    return 0;
}

/* While RomM lists the games: each platform's first covers (its tile) in one
 * file, the rest in another. */
struct pass_build { FILE *heads, *rest; int n, in_plat; };

static int collect_cover(const cJSON *it, const cJSON *page, int first, void *ctx) {
    (void)page, (void)first;
    if (!it) return 0;
    const char *p = jget_str(it, "path_cover_small", "");
    if (!valid_path(p) || strlen(p) >= 500 || strchr(p, '\n')) return 0;
    struct pass_build *b = ctx;
    if (fprintf(b->in_plat++ < 3 ? b->heads : b->rest, "%s\n", p) < 0) return -1;
    b->n++;
    return 0;
}

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Goes through one file's lines, leaving out those in mine (sorted): with
 * out, copies the rest onto it, and says how many; without, marks in seen
 * the ones of mine it came across. */
static int merge_lines(FILE *out, const char *path, char **mine, int nmine, char *seen) {
    char line[512];
    FILE *in = fopen(path, "r");
    int n = 0;
    while (in && fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *key = line;
        char **hit = nmine ? bsearch(&key, mine, (size_t)nmine, sizeof *mine, by_str) : NULL;
        if (hit && seen) seen[hit - mine] = 1;
        if (hit || !out) continue;
        fprintf(out, "%s\n", line);
        n++;
    }
    if (in) fclose(in);
    return n;
}

/* The covers of the games on this console, sorted, without repeats. */
static char **my_covers(int *n) {
    char **mine = NULL;
    int cap = 0;
    *n = 0;
    state_lock();
    const cJSON *e;
    cJSON_ArrayForEach(e, state_section("roms")) {
        const char *cover = jget_str(e, "cover", "");
        if (!valid_path(cover) || !(file_exists(e->string) || dir_exists(e->string))) continue;
        if (*n == cap) {
            char **grown = realloc(mine, sizeof *mine * (size_t)(cap = cap ? cap * 2 : 64));
            if (!grown) break;
            mine = grown;
        }
        if ((mine[*n] = strdup(cover))) (*n)++;
    }
    state_unlock();
    if (*n) qsort(mine, (size_t)*n, sizeof *mine, by_str);
    int kept = 0;
    for (int i = 0; i < *n; i++) {
        if (kept && !strcmp(mine[kept - 1], mine[i])) free(mine[i]);
        else mine[kept++] = mine[i];
    }
    *n = kept;
    return mine;
}

/* Every game's cover path, platform by platform, into a new pass on disk;
 * all: even the platforms whose game count didn't change. The covers seen
 * first go first: the games on this console, then each platform's tile, then
 * the rest in RomM's order. 0 when RomM gave the whole list; -1 if it couldn't
 * be reached part way (what came is still used). */
static int list_all(int all) {
    char heads[PATH_MAX_LEN], rest[PATH_MAX_LEN], fresh[PATH_MAX_LEN], path[PATH_MAX_LEN];
    pass_path(heads, sizeof heads, ".heads");
    pass_path(rest, sizeof rest, ".rest");
    pass_path(fresh, sizeof fresh, ".new");
    mkdir_parent(heads);
    struct pass_build b = {fopen(heads, "w"), fopen(rest, "w"), 0, 0};
    if (!b.heads || !b.rest) {
        if (b.heads) fclose(b.heads);
        if (b.rest) fclose(b.rest);
        LOGW("covers: can't write %s", heads);
        return -1;
    }
    long st = 0;
    cJSON *plats = romm_call("GET", "/api/platforms", NULL, &st), *p;
    int ok = st == 200 && cJSON_IsArray(plats), same = 0;
    state_lock();
    cJSON *had = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(state_section("covers"), "counts"), 1);
    state_unlock();
    cJSON *counts = cJSON_CreateObject();
    cJSON_ArrayForEach(p, plats) {
        const double n = jget_num(p, "rom_count", 0);
        if (n <= 0) continue;
        char id[24], list[160];
        snprintf(id, sizeof id, "%d", (int)jget_num(p, "id", 0));
        if (!all && jget_num(had, id, -1) == n) {
            cJSON_AddNumberToObject(counts, id, n);
            same++;
            continue;
        }
        snprintf(list, sizeof list, "/api/roms?platform_ids=%s&order_by=name&order_dir=asc&with_char_index=false" ROMS_LEAN, id);
        b.in_plat = 0;
        if (romm_list(list, 0, 100000, collect_cover, &b, &st) < 0) ok = 0;
        else cJSON_AddNumberToObject(counts, id, n);
    }
    cJSON_Delete(had);
    cJSON_Delete(plats);
    const int written = fclose(b.heads) == 0 && fclose(b.rest) == 0;

    FILE *out = written ? fopen(fresh, "w") : NULL;
    int total = 0, nmine = 0;
    if (out) {
        /* This console's games first, those RomM listed (a cover there since
         * replaced has a new path), then the tiles and the rest without them. */
        char **mine = my_covers(&nmine);
        char *seen = calloc((size_t)nmine + 1, 1);
        merge_lines(NULL, heads, mine, seen ? nmine : 0, seen);
        merge_lines(NULL, rest, mine, seen ? nmine : 0, seen);
        for (int i = 0; seen && i < nmine; i++)
            if (seen[i]) {
                fprintf(out, "%s\n", mine[i]);
                total++;
            }
        total += merge_lines(out, heads, mine, seen ? nmine : 0, NULL);
        total += merge_lines(out, rest, mine, seen ? nmine : 0, NULL);
        for (int i = 0; i < nmine; i++) free(mine[i]);
        free(mine);
        free(seen);
        if (fclose(out) != 0) total = -1;
    }
    unlink(heads);
    unlink(rest);
    cJSON *head = cJSON_CreateObject();
    cJSON_AddNumberToObject(head, "total", total);
    cJSON_AddItemToObject(head, "counts", counts);
    char *js = cJSON_PrintUnformatted(head);
    cJSON_Delete(head);

    pthread_mutex_lock(&g_lock);
    drop_pass();
    int opened = -1;
    pass_path(path, sizeof path, "");
    if (total >= 0 && js && rename(fresh, path) == 0) {
        char meta[PATH_MAX_LEN];
        pass_path(meta, sizeof meta, ".json");
        if (write_file_atomic(meta, js, strlen(js)) == 0) opened = open_pass();
    }
    if (opened != 0) drop_pass();
    pthread_mutex_unlock(&g_lock);
    unlink(fresh);
    free(js);
    if (opened != 0) {
        LOGW("covers: couldn't keep the list of %d games to check", b.n);
        return -1;
    }
    LOGI("covers: %d games to check%s%s", total, ok ? "" : " (RomM couldn't be reached for all of them)",
         same ? ", the platforms with as many games as last time skipped" : "");
    return ok ? 0 : -1;
}

/* The next cover to fetch: the pages being looked at first, then the full
 * pass, which waits while the library is being looked at. "" when there's
 * nothing to do. */
static void next_job(char *out, size_t n, int *from_pass) {
    out[0] = 0;
    *from_pass = 0;
    pthread_mutex_lock(&g_lock);
    if (g_nwarm) {
        str_copy(out, n, g_warm[0]);
        g_nwarm--;
        memmove(g_warm[0], g_warm[1], sizeof g_warm[0] * (size_t)g_nwarm);
    } else if (g_pass && g_next < g_nall && time(NULL) - g_browse_at >= BROWSE_PAUSE_SEC) {
        char line[512];
        if (fgets(line, sizeof line, g_pass)) {
            line[strcspn(line, "\r\n")] = 0;
            str_copy(out, n, line);
            *from_pass = 1;
            if (++g_next % 25 == 0) save_pos();
        } else {
            g_next = g_nall; /* shorter than it said: it's done anyway */
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        if (!enabled() || busy()) {
            pthread_mutex_lock(&g_lock);
            str_copy(g_state, sizeof g_state, enabled() ? "paused" : "idle");
            pthread_mutex_unlock(&g_lock);
            sleep(2);
            continue;
        }
        char p[512];
        int from_pass;
        next_job(p, sizeof p, &from_pass);
        if (!p[0]) {
            pthread_mutex_lock(&g_lock);
            if (g_next >= g_nall && g_nall) {
                str_copy(g_state, sizeof g_state, "done");
                drop_pass();
            }
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 30;
            pthread_cond_timedwait(&g_wake, &g_lock, &until);
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        if (from_pass) {
            pthread_mutex_lock(&g_lock);
            str_copy(g_state, sizeof g_state, "running");
            long long used = g_cached_bytes;
            pthread_mutex_unlock(&g_lock);
            /* Stop short of the limit, so pruning never undoes the pass. */
            if (used >= 0 && used > cache_limit() * 9 / 10) {
                pthread_mutex_lock(&g_lock);
                drop_pass();
                str_copy(g_state, sizeof g_state, "full");
                pthread_mutex_unlock(&g_lock);
                LOGW("covers: the cache is nearly full, the rest wait");
                continue;
            }
        }
        if (have(p)) {
            /* Still a current cover: keep it among the newest, so pruning takes
             * replaced and deleted covers first. */
            char cached[PATH_MAX_LEN];
            cache_path(p, "", cached, sizeof cached);
            if (from_pass) utime(cached, NULL);
        } else {
            char *body = NULL;
            size_t len = 0;
            const char *type;
            char why[160];
            if (covers_fetch(p, &body, &len, &type) == 0) {
                free(body);
            } else if (http_out_of_memory()) {
                /* No room for it now: it goes first again once some is free,
                 * rather than every cover after it failing the same way. */
                covers_warm(p);
                pthread_mutex_lock(&g_lock);
                str_copy(g_state, sizeof g_state, "waiting");
                pthread_mutex_unlock(&g_lock);
                sleep(10);
            } else if (romm_no_answer(why, sizeof why)) {
                /* Not skipped: it goes first again, once RomM answers (this
                 * worker's next try finds out). */
                covers_warm(p);
                pthread_mutex_lock(&g_lock);
                str_copy(g_state, sizeof g_state, "waiting");
                pthread_mutex_unlock(&g_lock);
                sleep(5);
            }
        }
        if (from_pass) {
            pthread_mutex_lock(&g_lock);
            g_done++;
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

/* Starts a full pass after setup, then once a day (or when asked). */
static void *planner(void *arg) {
    (void)arg;
    cache_scan(0);
    pthread_mutex_lock(&g_lock);
    if (open_pass() == 0 && g_next < g_nall) LOGI("covers: carrying on, %d of %d games left to check", g_nall - g_next, g_nall);
    pthread_mutex_unlock(&g_lock);
    /* How often it looks for work; ROMM_SYNC_COVERS_TICK shortens it in tests. */
    const char *env = getenv("ROMM_SYNC_COVERS_TICK");
    unsigned tick = env && atoi(env) > 0 ? (unsigned)atoi(env) : 20;
    time_t retry_at = 0; /* after a pass RomM couldn't list in full */
    for (;;) {
        sleep(tick);
        pthread_mutex_lock(&g_lock);
        int prune = g_prune;
        g_prune = 0;
        pthread_mutex_unlock(&g_lock);
        if (prune) cache_scan(1);
        if (!enabled() || busy()) continue;
        pthread_mutex_lock(&g_lock);
        int running = g_next < g_nall, asked = g_refresh;
        g_refresh = 0;
        cJSON *counts = running ? NULL : g_pass_counts;
        if (counts) g_pass_counts = NULL;
        pthread_mutex_unlock(&g_lock);
        if (counts) { /* a pass's covers are done */
            state_lock();
            cJSON *cs = state_section("covers");
            cJSON_DeleteItemFromObjectCaseSensitive(cs, "counts");
            cJSON_AddItemToObject(cs, "counts", counts);
            state_save();
            state_unlock();
        }
        if (running) continue;
        state_lock();
        double last = jget_num(state_section("covers"), "last_full", 0);
        state_unlock();
        if (!asked && last > 0 && time(NULL) - (time_t)last < FULL_EVERY_SEC) continue;
        if (!asked && time(NULL) < retry_at) continue;
        /* A pass RomM couldn't list in full isn't counted: it's tried again in
         * a few minutes, not a day. */
        if (list_all(asked) == 0) {
            state_lock();
            jset_num(state_section("covers"), "last_full", (double)time(NULL));
            state_save();
            state_unlock();
        } else {
            retry_at = time(NULL) + 300;
        }
        pthread_mutex_lock(&g_lock);
        pthread_cond_broadcast(&g_wake);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void covers_init(void) {
    thread_start(planner, NULL);
    for (int i = 0; i < workers(); i++) thread_start(worker, NULL);
}

cJSON *covers_status(void) {
    cJSON *j = cJSON_CreateObject();
    config_lock();
    cJSON_AddBoolToObject(j, "enabled", g_cfg.cover_cache);
    config_unlock();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(j, "state", g_state);
    cJSON_AddNumberToObject(j, "done", g_done);
    cJSON_AddNumberToObject(j, "total", g_total);
    cJSON_AddNumberToObject(j, "cached_mb", g_cached_bytes > 0 ? (double)(g_cached_bytes / (1024 * 1024)) : 0);
    pthread_mutex_unlock(&g_lock);
    cJSON_AddNumberToObject(j, "limit_mb", (double)(cache_limit() / (1024 * 1024)));
    return j;
}
