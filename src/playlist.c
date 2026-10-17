#include "playlist.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "state.h"
#include "util.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* The PS5's RetroArch runs from its title folder, which it sees as /app0:
 * its playlists name files that way. */
static int app_folder(const char *root) {
    char p[PATH_MAX_LEN];
    path_join(p, sizeof p, root, "sce_sys/param.json");
    return file_exists(p);
}

static void to_app_path(const char *root, const char *real, char *out, size_t n) {
    size_t rl = strlen(root);
    if (app_folder(root) && !strncmp(real, root, rl) && real[rl] == '/') snprintf(out, n, "/app0%s", real + rl);
    else str_copy(out, n, real);
}

static void to_real_path(const char *root, const char *p, char *out, size_t n) {
    if (!strncmp(p, "/app0/", 6) && app_folder(root)) snprintf(out, n, "%s%s", root, p + 5);
    else str_copy(out, n, p);
}

static int save(const char *path, const cJSON *j) {
    char *txt = cJSON_Print(j);
    int rc = txt ? write_file_atomic(path, txt, strlen(txt)) : -1;
    free(txt);
    if (rc) LOGW("could not write %s", path);
    return rc;
}

static void move_in(const char *root, const char *dir, const char *from, const char *to) {
    size_t fl = strlen(from);
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!str_ends_with_ci(de->d_name, ".lpl")) continue;
        char file[PATH_MAX_LEN];
        path_join(file, sizeof file, dir, de->d_name);
        char *txt = read_file(file, NULL);
        cJSON *pl = txt ? cJSON_Parse(txt) : NULL, *it;
        free(txt);
        int changed = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(pl, "items")) {
            const char *p = jget_str(it, "path", "");
            char real[PATH_MAX_LEN], moved[PATH_MAX_LEN + 256], back[PATH_MAX_LEN + 256];
            to_real_path(root, p, real, sizeof real);
            if (strncmp(real, from, fl) != 0 || real[fl] != '/') continue;
            snprintf(moved, sizeof moved, "%s%s", to, real + fl);
            if (!strncmp(p, "/app0/", 6)) to_app_path(root, moved, back, sizeof back);
            else str_copy(back, sizeof back, moved);
            jset_str(it, "path", back);
            changed++;
        }
        if (changed && !save(file, pl)) LOGI("RetroArch playlist %s: %d game%s moved", de->d_name, changed, changed == 1 ? "" : "s");
        cJSON_Delete(pl);
    }
    if (d) closedir(d);
}

void playlists_move(const char *root, const char *playlist_dir, const char *from, const char *to) {
    char dir[PATH_MAX_LEN], builtin[PATH_MAX_LEN];
    if (playlist_dir && playlist_dir[0]) str_copy(dir, sizeof dir, playlist_dir);
    else path_join(dir, sizeof dir, root, "playlists");
    path_join(builtin, sizeof builtin, dir, "builtin");
    pthread_mutex_lock(&g_lock);
    move_in(root, dir, from, to);
    move_in(root, builtin, from, to);
    pthread_mutex_unlock(&g_lock);
}
