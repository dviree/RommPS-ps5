/* plat_mounts_set on the PS5: nullfs mounts for an emulator to play another
 * one's games without a copy (bfs, under /user and /data, has no hard links
 * or symlinks).
 *
 * An app reads its folders through its sandbox, /mnt/sandbox/<title>_<n>/
 * (n changes with every launch), and a mount on the real folder isn't seen
 * there, so ours go on the sandbox's view: "app0/games" is the app's
 * /app0/games. An app that leaves its sandbox to read its files (PSXS5
 * "unlocks storage") reads the real folder: it gets the mount there
 * ("/data/PSXS5/games"), for as long as it runs. They go on as soon as the
 * sandbox and the app's process appear, before its first look at its
 * folders, and come off the moment one of its processes exits: the system
 * takes app0 down 40-240 ms after the exit, and a mount of ours still on it
 * then crashes the console. Rest mode leaves them alone (unmounting there and
 * mounting again at wake lined up with the HEN's toolbox failing). Tested
 * with PSSwanStation and PSXS5, on OnionHEN and etaHEN with ShadowMountPlus.
 *
 * Mounts on a real folder are written down (mounts-held.txt in the data
 * folder): one a copy of the payload that stopped left on, for an app that
 * isn't running or no longer gets it, comes off, and its folder goes back to
 * its owner. */
#include "platform.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/event.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "console.h"
#include "events.h"
#include "ps5.h"
#include "util.h"

#define MAX_WANT   32
#define MAX_LAUNCH 4
#define MAX_PIDS   16
#define MAX_HELD   8
#define MAX_REAL   16
#define TICK_MS    100

typedef struct {
    char name[64]; /* the emulator's */
    char from[1024];
    char target[1300];
    uid_t owner; /* the folder's owner before we took it, or -1 */
    int on;
} held_t;

/* One run of an app with mounts. Over once one of its processes exits; the
 * slot is free again when the system has taken its sandbox down. */
typedef struct {
    int used, over;
    int polled; /* kqueue couldn't watch a process: checked every 2 ms */
    char title[16];
    char sandbox[256];
    pid_t pids[MAX_PIDS];
    int np;
    held_t held[MAX_HELD];
    int nh;
} launch_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static plat_mount g_want[MAX_WANT];
static int g_nwant, g_started;

static launch_t g_runs[MAX_LAUNCH];
static int g_kq = -1;

/* Our mounts on real folders, as written down: only the guard thread uses them. */
typedef struct {
    char title[16];
    char target[1300];
    uid_t owner;
} real_t;

static real_t g_real[MAX_REAL];
static int g_nreal;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int is_mount_point(const char *path) {
    struct statfs fs;
    return statfs(path, &fs) == 0 && !strcmp(fs.f_mntonname, path);
}

int plat_is_mount(const char *path) { return is_mount_point(path); }

static void real_path(char *out, size_t n) { path_join(out, n, plat_data_dir(), "mounts-held.txt"); }

static void real_save(void) {
    char path[PATH_MAX_LEN], *buf = malloc((size_t)MAX_REAL * 1400 + 1);
    size_t o = 0;
    if (!buf) return;
    buf[0] = 0;
    for (int i = 0; i < g_nreal; i++)
        o += (size_t)snprintf(buf + o, 1400, "%s\t%d\t%s\n", g_real[i].title, (int)g_real[i].owner, g_real[i].target);
    real_path(path, sizeof path);
    if (g_nreal) write_file_atomic(path, buf, o);
    else unlink(path);
    free(buf);
}

static void real_load(void) {
    char path[PATH_MAX_LEN];
    real_path(path, sizeof path);
    char *txt = read_file(path, NULL), *save = NULL;
    for (char *l = txt ? strtok_r(txt, "\n", &save) : NULL; l && g_nreal < MAX_REAL; l = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || t2[1] != '/') continue;
        *t1 = *t2 = 0;
        real_t *r = &g_real[g_nreal++];
        snprintf(r->title, sizeof r->title, "%s", l);
        r->owner = (uid_t)atoi(t1 + 1);
        snprintf(r->target, sizeof r->target, "%s", t2 + 1);
    }
    free(txt);
}

static real_t *real_find(const char *target) {
    for (int i = 0; i < g_nreal; i++)
        if (!strcmp(g_real[i].target, target)) return &g_real[i];
    return NULL;
}

static void real_add(const char *title, const char *target, uid_t owner) {
    real_t *r = real_find(target);
    if (!r && g_nreal == MAX_REAL) return;
    if (!r) r = &g_real[g_nreal++];
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->target, sizeof r->target, "%s", target);
    r->owner = owner;
    real_save();
}

static void real_drop(const char *target) {
    real_t *r = real_find(target);
    if (!r) return;
    *r = g_real[--g_nreal];
    real_save();
}

static int null_mount(const char *from, const char *on) {
    struct iovec iov[8];
    char errmsg[256] = "";
    const char *kv[] = {"fstype", "nullfs", "fspath", on, "target", from};
    int n = 0;
    for (int i = 0; i < 6; i++) {
        iov[n].iov_base = (void *)kv[i];
        iov[n++].iov_len = strlen(kv[i]) + 1;
    }
    iov[n].iov_base = "errmsg";
    iov[n++].iov_len = sizeof "errmsg";
    iov[n].iov_base = errmsg;
    iov[n++].iov_len = sizeof errmsg;
    if (nmount(iov, (unsigned)n, 0) == 0) return 0;
    LOGW("mounts: can't mount %s on %s: %s %s", from, on, strerror(errno), errmsg);
    return -1;
}

struct pids_ctx {
    const char *title;
    pid_t *pids;
    int n, max;
};

static int pids_cb(pid_t pid, const char *tdname, void *vctx) {
    struct pids_ctx *c = vctx;
    char title[16];
    (void)tdname;
    if (c->n < c->max && !ps5_proc_app(pid, title, NULL) && !strncmp(title, c->title, 9)) c->pids[c->n++] = pid;
    return 0;
}

static int find_pids(const char *title, pid_t *pids, int max) {
    struct pids_ctx c = {title, pids, 0, max};
    console_for_each_proc(pids_cb, &c);
    return c.n;
}

/* The kernel only lets us mount over a folder we own: even root can't over
 * one the app made (uid 1). Taken for the mount, given back after. */
static void give_back(held_t *h) {
    if (h->owner == (uid_t)-1) return;
    if (chown(h->target, h->owner, (gid_t)-1) != 0)
        LOGW("mounts: can't give %s back to uid %d: %s", h->target, (int)h->owner, strerror(errno));
    h->owner = (uid_t)-1;
}

static int put_on(held_t *h) {
    struct stat st;
    h->owner = (uid_t)-1;
    if (stat(h->target, &st) != 0 && mkdir(h->target, 0777) != 0) {
        LOGW("mounts: no %s: %s", h->target, strerror(errno));
        return -1;
    }
    if (stat(h->target, &st) == 0 && st.st_uid != 0) {
        if (chown(h->target, 0, (gid_t)-1) == 0) h->owner = st.st_uid;
        else LOGW("mounts: can't take %s from uid %d: %s", h->target, (int)st.st_uid, strerror(errno));
    }
    if (null_mount(h->from, h->target) != 0) {
        give_back(h);
        return -1;
    }
    h->on = 1;
    LOGI("mounts: %s on %s", h->from, h->target);
    return 0;
}

/* Without forcing: a mount the app has a file open on stays. 0 when off. */
static int take_off(held_t *h, int tries_max) {
    int tries = 0;
    while (unmount(h->target, 0) != 0 && errno != EINVAL && errno != ENOENT) {
        if (++tries >= tries_max) return -1;
        usleep(1000);
    }
    h->on = 0;
    give_back(h);
    real_drop(h->target);
    return 0;
}

/* At the exit there's no app left to pull files from, so a mount still busy
 * after 200 ms is forced off: the system is about to take app0 down. */
static void take_off_all(launch_t *r) {
    const double t = now_ms();
    int n = 0;
    for (int k = r->nh - 1; k >= 0; k--) {
        held_t *h = &r->held[k];
        if (!h->on) continue;
        n++;
        if (take_off(h, 200) != 0) {
            LOGW("mounts: %s still busy after 200 ms (%s), forcing", h->target, strerror(errno));
            if (unmount(h->target, MNT_FORCE) != 0) {
                LOGE("mounts: forced unmount of %s failed: %s", h->target, strerror(errno));
                events_add("emulator", "failed", 0, h->target, h->name, "Its games stayed mounted after it closed");
                events_save();
            }
            h->on = 0;
            give_back(h);
            real_drop(h->target);
        }
    }
    if (n) LOGI("mounts: %s closed, %d mount%s off in %.1f ms", r->title, n, n == 1 ? "" : "s", now_ms() - t);
}

static void exited(launch_t *r, pid_t pid) {
    int ours = 0;
    for (int i = 0; i < r->np; i++) ours |= r->pids[i] == pid;
    if (!r->used || r->over || !ours) return; /* a late one from the run this slot had before */
    r->over = 1;
    LOGI("mounts: %s's process %d exited", r->title, (int)pid);
    take_off_all(r);
}

static void watch_pids(launch_t *r) {
    for (int i = 0; i < r->np && !r->over; i++) {
        struct kevent ev;
        EV_SET(&ev, r->pids[i], EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, r);
        if (g_kq >= 0 && kevent(g_kq, &ev, 1, NULL, 0, NULL) == 0) continue;
        if (errno == ESRCH) exited(r, r->pids[i]); /* gone already */
        else r->polled = 1;
    }
}

static void poll_pids(void) {
    for (int i = 0; i < MAX_LAUNCH; i++)
        for (int k = 0; g_runs[i].used && g_runs[i].polled && !g_runs[i].over && k < g_runs[i].np; k++)
            if (kill(g_runs[i].pids[k], 0) != 0 && errno == ESRCH) exited(&g_runs[i], g_runs[i].pids[k]);
}

static launch_t *run_of_sandbox(const char *sandbox) {
    for (int i = 0; i < MAX_LAUNCH; i++)
        if (g_runs[i].used && !strcmp(g_runs[i].sandbox, sandbox)) return &g_runs[i];
    return NULL;
}

/* A nullfs mount below the sandbox's app0: only we put those there (the
 * HEN's /data and fonts go on the sandbox itself, the system's app0 isn't
 * below itself). */
static int ours_left(const struct statfs *m, const char *sandbox) {
    char app0[300];
    snprintf(app0, sizeof app0, "%s/app0/", sandbox);
    return !strncmp(m->f_mntonname, app0, strlen(app0)) && !strcmp(m->f_fstypename, "nullfs");
}

/* A new sandbox of an app with mounts, with its process running. */
static void start_run(const char *title, const char *sandbox, const pid_t *pids, int np, const struct statfs *mnt, int nmnt) {
    launch_t *r = NULL;
    for (int i = 0; i < MAX_LAUNCH && !r; i++)
        if (!g_runs[i].used) r = &g_runs[i];
    if (!r) {
        LOGW("mounts: too many apps at once, %s gets none", title);
        return;
    }
    memset(r, 0, sizeof *r);
    r->used = 1;
    snprintf(r->title, sizeof r->title, "%s", title);
    snprintf(r->sandbox, sizeof r->sandbox, "%s", sandbox);
    memcpy(r->pids, pids, sizeof *pids * (size_t)np);
    r->np = np;
    /* Ours left on by a copy of the payload that stopped (an update). Taken
     * over, so they come off at the exit. */
    for (int i = 0; i < nmnt && r->nh < MAX_HELD; i++) {
        if (!ours_left(&mnt[i], sandbox)) continue;
        held_t *h = &r->held[r->nh++];
        snprintf(h->target, sizeof h->target, "%s", mnt[i].f_mntonname);
        snprintf(h->from, sizeof h->from, "%s", mnt[i].f_mntfromname);
        h->owner = (uid_t)-1;
        h->on = 1;
        LOGI("mounts: took over %s", mnt[i].f_mntonname);
    }
    for (int i = 0; i < g_nreal && r->nh < MAX_HELD; i++) {
        if (strcmp(g_real[i].title, title) != 0 || !is_mount_point(g_real[i].target)) continue;
        held_t *h = &r->held[r->nh++];
        memset(h, 0, sizeof *h);
        snprintf(h->target, sizeof h->target, "%s", g_real[i].target);
        h->owner = g_real[i].owner;
        h->on = 1;
        LOGI("mounts: took over %s", h->target);
    }
    const double t = now_ms();
    int on = 0, failed = 0;
    pthread_mutex_lock(&g_lock);
    for (int k = 0; k < g_nwant && r->nh < MAX_HELD; k++) {
        const plat_mount *m = &g_want[k];
        if (strcmp(m->title, title) != 0) continue;
        char target[1300];
        if (m->into[0] == '/') snprintf(target, sizeof target, "%s", m->into); /* the real folder */
        else snprintf(target, sizeof target, "%s/%s", sandbox, m->into);
        int have = 0;
        for (int h = 0; h < r->nh; h++) have |= !strcmp(r->held[h].target, target);
        if (have) continue;
        held_t *h = &r->held[r->nh++];
        memset(h, 0, sizeof *h);
        snprintf(h->target, sizeof h->target, "%s", target);
        snprintf(h->from, sizeof h->from, "%s", m->from);
        snprintf(h->name, sizeof h->name, "%s", m->name);
        h->owner = (uid_t)-1;
        if (m->into[0] == '/' && is_mount_point(target)) {
            /* Left on by a copy of the payload that stopped, and not written
             * down: its owner is unknown. */
            h->on = 1;
            LOGI("mounts: took over %s", target);
        } else if (put_on(h) == 0) {
            if (m->into[0] == '/') real_add(title, target, h->owner);
            on++;
        } else {
            failed++;
            /* The app runs without the games: say so, as RommPS. */
            events_add("emulator", "failed", 0, h->target, m->name, "Its games couldn't be shown to it this time");
            events_save();
            plat_notify("RommPS: %s can't see its games this time. Close it and open it again.", m->name);
        }
    }
    pthread_mutex_unlock(&g_lock);
    LOGI("mounts: %s started (%s, %d process%s): %d on in %.1f ms%s", title, sandbox, np, np == 1 ? "" : "es", on,
         now_ms() - t, failed ? ", some failed" : "");
    watch_pids(r);
}

/* A real folder's mount written down, for an app that isn't running: left by
 * a copy of the payload that stopped. Nothing reads it, so it comes off. */
static void clear_left_real(void) {
    for (int i = g_nreal - 1; i >= 0; i--) {
        real_t *x = &g_real[i];
        int held = 0;
        for (int k = 0; k < MAX_LAUNCH && !held; k++)
            for (int j = 0; g_runs[k].used && j < g_runs[k].nh && !held; j++)
                held = g_runs[k].held[j].on && !strcmp(g_runs[k].held[j].target, x->target);
        if (held) continue;
        pid_t pids[MAX_PIDS];
        if (is_mount_point(x->target)) {
            if (find_pids(x->title, pids, MAX_PIDS) > 0) continue; /* its sandbox's run takes it over */
            if (unmount(x->target, 0) != 0) continue;              /* busy: next time */
            LOGI("mounts: took %s off, left on by an earlier copy", x->target);
        }
        /* Off now, or gone with a reboot: the folder goes back to its owner. */
        if (x->owner != (uid_t)-1 && chown(x->target, x->owner, (gid_t)-1) != 0)
            LOGW("mounts: can't give %s back to uid %d: %s", x->target, (int)x->owner, strerror(errno));
        real_drop(x->target);
    }
}

/* New sandboxes of the apps with mounts, and old ones the system took down. */
static void scan_sandboxes(void) {
    struct statfs *mnt;
    int nmnt = getmntinfo(&mnt, MNT_NOWAIT);
    if (nmnt <= 0) return;
    for (int i = 0; i < MAX_LAUNCH; i++) {
        launch_t *r = &g_runs[i];
        if (!r->used) continue;
        char app0[300];
        snprintf(app0, sizeof app0, "%s/app0", r->sandbox);
        int there = 0;
        for (int k = 0; k < nmnt && !there; k++) there = !strcmp(mnt[k].f_mntonname, app0);
        if (!there) {
            if (!r->over) { /* the exit went unseen: shouldn't happen, but don't leave ours behind */
                LOGW("mounts: %s's sandbox went before its exit was seen", r->title);
                take_off_all(r);
            }
            r->used = 0;
        }
    }
    for (int k = 0; k < nmnt; k++) {
        const char *on = mnt[k].f_mntonname;
        size_t len = strlen(on);
        if (strncmp(on, "/mnt/sandbox/", 13) != 0 || len < 5 + 13 || strcmp(on + len - 5, "/app0") != 0) continue;
        char sandbox[256], title[16];
        if (len - 5 >= sizeof sandbox) continue;
        memcpy(sandbox, on, len - 5);
        sandbox[len - 5] = 0;
        const char *base = sandbox + 13, *us = strrchr(base, '_');
        if (!us || us - base != 9) continue;
        memcpy(title, base, 9);
        title[9] = 0;
        int wanted = 0;
        pthread_mutex_lock(&g_lock);
        for (int w = 0; w < g_nwant && !wanted; w++) wanted = !strcmp(g_want[w].title, title);
        pthread_mutex_unlock(&g_lock);
        if (run_of_sandbox(sandbox)) continue;
        if (!wanted) {
            /* Not ours to mount, but ours may be on it from a copy that
             * stopped, under a list that has changed since. */
            int left = 0;
            for (int i = 0; i < nmnt && !left; i++) left = ours_left(&mnt[i], sandbox);
            for (int i = 0; i < g_nreal && !left; i++) left = !strcmp(g_real[i].title, title) && is_mount_point(g_real[i].target);
            if (!left) continue;
        }
        pid_t pids[MAX_PIDS];
        int np = find_pids(title, pids, MAX_PIDS);
        if (np > 0) start_run(title, sandbox, pids, np, mnt, nmnt);
    }
    clear_left_real();
}

static void *guard_thread(void *arg) {
    (void)arg;
    real_load();
    g_kq = kqueue();
    if (g_kq < 0) LOGW("mounts: no kqueue (%s), checking processes every 2 ms", strerror(errno));
    double next_scan = 0;
    for (;;) {
        int polled = 0;
        for (int i = 0; i < MAX_LAUNCH; i++) polled |= g_runs[i].used && g_runs[i].polled && !g_runs[i].over;
        const long wait_ms = polled ? 2 : TICK_MS;
        if (g_kq >= 0) {
            struct kevent ev[8];
            struct timespec ts = {0, wait_ms * 1000 * 1000};
            int got = kevent(g_kq, NULL, 0, ev, 8, &ts);
            for (int i = 0; i < got; i++)
                if (ev[i].filter == EVFILT_PROC) exited(ev[i].udata, (pid_t)ev[i].ident);
        } else {
            usleep((useconds_t)wait_ms * 1000);
        }
        if (polled) poll_pids();
        if (now_ms() >= next_scan) {
            scan_sandboxes();
            next_scan = now_ms() + TICK_MS;
        }
    }
    return NULL;
}

void plat_mounts_set(const plat_mount *mounts, int n) {
    if (n > MAX_WANT) n = MAX_WANT;
    pthread_mutex_lock(&g_lock);
    int changed = n != g_nwant || (n && memcmp(g_want, mounts, sizeof *mounts * (size_t)n) != 0);
    if (n) memcpy(g_want, mounts, sizeof *mounts * (size_t)n);
    g_nwant = n;
    /* Always running once started, and started at the first call even with
     * none: mounts an earlier copy left must come off at the app's exit. */
    int start = !g_started;
    g_started = 1;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; changed && i < n; i++) LOGI("mounts: %s gets %s on %s", mounts[i].title, mounts[i].from, mounts[i].into);
    if (start && thread_start(guard_thread, NULL) != 0) {
        LOGE("mounts: can't start the guard");
        pthread_mutex_lock(&g_lock);
        g_started = 0;
        pthread_mutex_unlock(&g_lock);
    }
}
