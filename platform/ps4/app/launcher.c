/* RommPS for PS4: the home screen app. The sync itself is the payload,
 * which GoldHEN runs from /data/payloads (platform/ps4/platform.c); this app
 * only installs that payload and shows its web UI, so it never needs to stay
 * running. Built with OpenOrbis (platform/ps4/ps4.mk, "make ps4-pkg").
 *
 * When opened it:
 *   1. copies the payload bundled in the package to /data/payloads when it's
 *      missing or older than the bundled one. Versions come from the
 *      ROMM_SYNC_VERSION= tag in each payload (src/main.c), so an older app
 *      never undoes a newer payload from an update or a manual copy. This app
 *      runs as a regular user, so it only relies on /data/payloads being
 *      writable. Files are read in full: stat() is unreliable in the sandbox;
 *   2. checks that RomM Sync answers on its web port;
 *   3. shows the web UI in the system web browser dialog, or the Browser app
 *      if the dialog won't open; if RomM Sync isn't running, says why, from
 *      what it left in its logs, and how to start it.
 *
 * Each step goes to /data/romm-sync/launcher.log, for bug reports.
 *
 * The web browser dialog has no OpenOrbis types; its layout is Sony's
 * SceWebBrowserDialogParam as used by Nuvio-PS5 and EVO Player. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define BUNDLED_PAYLOAD "/app0/romm-sync-ps4.elf"
#define PAYLOAD_DIR     "/data/payloads"
#define PAYLOAD_PATH    PAYLOAD_DIR "/romm-sync-ps4.elf"
#define DATA_DIR        "/data/romm-sync"
#define LOG_PATH        DATA_DIR "/launcher.log"
#define BOOT_LOG        DATA_DIR "/boot.log"     /* the payload's start steps, from its last start */
#define PAYLOAD_LOG     DATA_DIR "/romm-sync.log"
#define AUTORUN_INI     "/data/GoldHEN/payloads.ini" /* "[AutoRun]" then "<payload path> = 1" */
#define CONFIG_PATH     DATA_DIR "/config.json"
#define VERSION_TAG     "ROMM_SYNC_VERSION="
#define DEFAULT_PORT    8780

/* libSceWebBrowserDialog */
typedef struct {
    OrbisCommonDialogBaseParam base;
    uint64_t size;
    int32_t mode; /* 1: the browser's own layout, 2: the rectangle below */
    int32_t user_id;
    const char *url;
    void *callback_init;
    uint16_t width, height, pos_x, pos_y;
    uint32_t parts;
    uint16_t header_width, header_x, header_y, pad0;
    uint32_t control;
    void *ime_param;
    void *webview_param;
    uint32_t animation;
    uint8_t reserved[202];
    uint16_t tail_pad;
} web_dialog_param;
_Static_assert(sizeof(web_dialog_param) == 328, "SceWebBrowserDialogParam is 328 bytes");

int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(web_dialog_param *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);
/* OpenOrbis declares it without parameters; this is the real signature. */
int launch_web_browser(const char *uri, void *param) __asm__("sceSystemServiceLaunchWebBrowser");

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    char ts[32];
    time_t now = time(NULL);
    va_list ap;
    if (!f) return;
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static int user_id(void) {
    int id = -1;
    if (sceUserServiceGetForegroundUser(&id) == 0 && id != -1 && id != 0xff) return id;
    if (sceUserServiceGetInitialUser(&id) == 0) return id;
    return -1;
}

/* Whole file, by reading: stat() sizes can't be trusted in the sandbox. */
static unsigned char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t cap = 0, n = 0, r;
    if (!f) return NULL;
    do {
        if (n == cap) {
            unsigned char *nb = realloc(buf, cap = cap ? cap * 2 : 64 * 1024);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
        }
        r = fread(buf + n, 1, cap - n, f);
        n += r;
    } while (r > 0);
    fclose(f);
    *len = n;
    return buf;
}

/* The version in a payload's ROMM_SYNC_VERSION= tag, or "" if it has none
 * (payloads older than the tag). Read in chunks: a payload is megabytes, and
 * this app's heap doesn't take a buffer that big well. 0 if it can't be read. */
static int payload_tag(const char *path, char *out, size_t n) {
    static unsigned char buf[64 * 1024 + 128];
    size_t tl = strlen(VERSION_TAG), keep = 0, got;
    FILE *f = fopen(path, "rb");
    out[0] = 0;
    if (!f) return 0;
    while ((got = fread(buf + keep, 1, sizeof buf - keep, f)) > 0) {
        size_t len = keep + got;
        for (size_t i = 0; i + tl < len; i++) {
            if (buf[i] != 'R' || memcmp(buf + i, VERSION_TAG, tl) != 0) continue;
            size_t j = 0;
            for (const unsigned char *v = buf + i + tl; v < buf + len && *v != '\n' && *v && j + 1 < n; v++)
                out[j++] = (char)*v;
            out[j] = 0;
            fclose(f);
            return 1;
        }
        /* The tail again, in case the tag straddles two reads. */
        keep = len < 128 ? len : 128;
        memmove(buf, buf + len - keep, keep);
    }
    fclose(f);
    return 1;
}

/* The two files hold the same bytes. */
static int same_file(const char *a, const char *b) {
    static unsigned char x[64 * 1024], y[64 * 1024];
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    size_t na, nb;
    while (same) {
        na = fread(x, 1, sizeof x, fa);
        nb = fread(y, 1, sizeof y, fb);
        if (na != nb || memcmp(x, y, na) != 0) same = 0;
        else if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* Copies src to dst; 0 when every byte got there. */
static int copy_to(const char *src, const char *dst) {
    static unsigned char buf[64 * 1024];
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    size_t got;
    int ok = in && out;
    while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, got, out) == got;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* "1.2.3" or "1.2.3-ps4-prototype.6": the three numbers, and the last number
 * of the pre-release part (-1 for a release). 0 if it isn't a version. */
static int parse_version(const char *s, int v[4]) {
    v[3] = -1;
    if (sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) != 3) return 0;
    const char *pre = strchr(s, '-');
    if (pre) {
        v[3] = 0;
        for (const char *c = pre; *c; c++)
            if (*c >= '0' && *c <= '9' && (c == pre || c[-1] < '0' || c[-1] > '9')) v[3] = atoi(c);
    }
    return 1;
}

/* >0 if a is newer than b. A release is newer than its pre-releases. */
static int compare_versions(const int a[4], const int b[4]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] - b[i];
    if ((a[3] < 0) != (b[3] < 0)) return a[3] < 0 ? 1 : -1;
    return a[3] - b[3];
}

/* 1 if installed or replaced, 0 if left as it is, -1 on failure. */
static int install_payload(void) {
    char want_ver[64], have_ver[64];
    int wv[4], hv[4], rc = -1;
    if (!payload_tag(BUNDLED_PAYLOAD, want_ver, sizeof want_ver)) {
        logf_("can't read the bundled payload %s", BUNDLED_PAYLOAD);
        return -1;
    }
    const int have = payload_tag(PAYLOAD_PATH, have_ver, sizeof have_ver);
    int replace;
    if (!have) replace = 1;
    else if (same_file(PAYLOAD_PATH, BUNDLED_PAYLOAD)) replace = 0;
    else if (!parse_version(want_ver, wv)) replace = 0; /* an untagged test build: never overwrite */
    else if (!parse_version(have_ver, hv)) replace = 1; /* older than version tags */
    else replace = compare_versions(wv, hv) > 0;
    if (!replace) {
        logf_("payload %s left as it is (installed %s, this app has %s)", PAYLOAD_PATH, have_ver[0] ? have_ver : "?",
              want_ver[0] ? want_ver : "?");
        return 0;
    }
    mkdir(PAYLOAD_DIR, 0777);
    if (copy_to(BUNDLED_PAYLOAD, PAYLOAD_PATH ".tmp") == 0) rc = rename(PAYLOAD_PATH ".tmp", PAYLOAD_PATH) == 0 ? 1 : -1;
    logf_("%s %s with %s (was %s): %s", have ? "replaced" : "installed", PAYLOAD_PATH, want_ver[0] ? want_ver : "?",
          have ? (have_ver[0] ? have_ver : "untagged") : "missing", rc == 1 ? "ok" : "failed");
    return rc;
}

/* The web_port from RomM Sync's config.json, if it was changed. */
static int web_port(void) {
    size_t len = 0;
    char *cfg = (char *)slurp(CONFIG_PATH, &len), *p;
    int port = DEFAULT_PORT;
    if (cfg) {
        cfg = realloc(cfg, len + 1);
        cfg[len] = 0;
        if ((p = strstr(cfg, "\"web_port\"")) && (p = strchr(p, ':'))) port = atoi(p + 1);
        free(cfg);
    }
    return port > 0 && port < 65536 ? port : DEFAULT_PORT;
}

/* The running payload's version from /api/status, or 0 if nothing answers. */
static int payload_version(int port, char *version, size_t n) {
    struct sockaddr_in sa;
    char buf[2048], req[128], *v;
    int fd, got = 0, r;
    version[0] = 0;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return 0;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return 0;
    }
    snprintf(req, sizeof req, "GET /api/status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
    write(fd, req, strlen(req));
    while (got < (int)sizeof buf - 1 && (r = (int)read(fd, buf + got, sizeof buf - 1 - got)) > 0) got += r;
    close(fd);
    buf[got] = 0;
    if ((v = strstr(buf, "\"version\":\"")) || (v = strstr(buf, "\"version\": \""))) {
        v = strchr(v + 9, '"') + 1;
        snprintf(version, n, "%.*s", (int)strcspn(v, "\""), v);
    }
    return 1;
}

/* The end of a text file, at most TEXT_TAIL bytes of it (RomM Sync's log
 * only grows, and its last start is near the end). Read straight through:
 * sizes can't be trusted in the sandbox. */
#define TEXT_TAIL (256 * 1024)
static char *slurp_text(const char *path) {
    FILE *f = fopen(path, "rb");
    char *t = f ? malloc(TEXT_TAIL + 64 * 1024 + 1) : NULL;
    size_t len = 0, got;
    if (!t) {
        if (f) fclose(f);
        return NULL;
    }
    while ((got = fread(t + len, 1, 64 * 1024, f)) > 0) {
        len += got;
        if (len > TEXT_TAIL) { /* keep the newest TEXT_TAIL bytes */
            memmove(t, t + len - TEXT_TAIL, TEXT_TAIL);
            len = TEXT_TAIL;
        }
    }
    fclose(f);
    t[len] = 0;
    return t;
}

/* GoldHEN runs the payload on every jailbreak: it's in the AutoRun queue. */
static int in_autorun(void) {
    char *ini = slurp_text(AUTORUN_INI), *line;
    int on = 0;
    for (line = ini ? strstr(ini, "romm-sync-ps4.elf") : NULL; line; line = strstr(line + 1, "romm-sync-ps4.elf")) {
        char *eq = strchr(line, '='), *eol = strchr(line, '\n');
        if (eq && (!eol || eq < eol) && atoi(eq + 1) == 1) on = 1;
    }
    free(ini);
    return on;
}

/* The part of RomM Sync's log since it last started, or NULL. */
static char *last_start(char *log) {
    char *since = NULL;
    for (char *p = log; p && (p = strstr(p, " starting (")); p++) since = p;
    if (since)
        while (since > log && since[-1] != '\n') since--;
    return since;
}

/* RomM Sync started and hasn't opened its web page yet, nor failed: it's
 * still starting (or stuck doing so). */
static int starting(void) {
    char *boot = slurp_text(BOOT_LOG), *log = slurp_text(PAYLOAD_LOG), *since = last_start(log);
    int yes = boot && strstr(boot, "platform ready") && !strstr(boot, "already running") && since &&
              !strstr(since, "web UI on") && !strstr(since, "crashed with signal") && !strstr(since, "cannot listen on port");
    free(boot);
    free(log);
    return yes;
}

/* Why RomM Sync isn't answering, from what it left behind: its start steps
 * (boot.log, rewritten at each start) and its log since it last started. */
static void diagnose(int installed, int port, char *out, size_t n) {
    char *boot = slurp_text(BOOT_LOG), *log = slurp_text(PAYLOAD_LOG), *since = last_start(log), when[20] = "";
    const int autorun = in_autorun();
    const char *how = autorun ? "In GoldHEN Settings > Payloader, run romm-sync-ps4.elf, then open RommPS again."
                              : "In GoldHEN Settings > Payloader, run romm-sync-ps4.elf and add it to the AutoRun queue. "
                                "Then open RommPS again.";
    logf_("in GoldHEN's AutoRun queue: %s", autorun ? "yes" : "no");
    if (since) snprintf(when, sizeof when, "%.19s", since); /* the line's "2026-10-16 09:30:00" */
    if (boot && strstr(boot, "already running")) {
        logf_("diagnosis: a copy holds the lock but doesn't answer");
        snprintf(out, n, "RommPS's sync service is stuck. Restart the console, jailbreak, and open RommPS again.");
    } else if (since && strstr(since, "crashed with signal")) {
        logf_("diagnosis: crashed after starting at %s", when);
        snprintf(out, n,
                 "RommPS's sync service stopped with an error (started %s).\n\nRestart the console and jailbreak again. "
                 "If it keeps happening, send /data/romm-sync/romm-sync.log with a bug report.",
                 when);
    } else if (since && strstr(since, "cannot listen on port")) {
        logf_("diagnosis: port %d is taken", port);
        snprintf(out, n,
                 "RommPS's sync service is running, but another app uses port %d.\n\nClose other homebrew servers, "
                 "or change web_port in /data/romm-sync/config.json.",
                 port);
    } else if (since && !strstr(since, "web UI on")) {
        const char *last = since + strlen(since);
        while (last > since && last[-1] == '\n') last--;
        const char *bol = last;
        while (bol > since && bol[-1] != '\n') bol--;
        logf_("diagnosis: started at %s, no web page yet; it last logged: %.*s", when, (int)(last - bol), bol);
        snprintf(out, n, "RommPS's sync service started (%s) but hasn't opened its web page yet.\n\nGive it a moment "
                         "and open RommPS again. If this keeps happening, send /data/romm-sync/romm-sync.log with a bug "
                         "report.", when);
    } else if (since) {
        logf_("diagnosis: last started at %s, not running now", when);
        if (autorun)
            snprintf(out, n, "RommPS's sync service isn't running. It's in GoldHEN's AutoRun queue but didn't start "
                             "this time (last start %s).\n\n%s", when, how);
        else
            snprintf(out, n, "RommPS's sync service isn't running. It last started %s, and won't start after a reboot "
                             "unless it's in GoldHEN's AutoRun queue.\n\n%s", when, how);
    } else if (boot) {
        logf_("diagnosis: started but stopped before its log opened");
        snprintf(out, n, "RommPS's sync service started but stopped right away.\n\nRestart the console and jailbreak "
                         "again. If it keeps happening, send /data/romm-sync/boot.log with a bug report.");
    } else {
        logf_("diagnosis: never started");
        snprintf(out, n, "RommPS's sync service hasn't been started yet.\n\n%s%s",
                 installed > 0 ? "It's now in /data/payloads. " : "", how);
    }
    free(boot);
    free(log);
}

static void message(const char *text) {
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam user;
    memset(&param, 0, sizeof param);
    memset(&user, 0, sizeof user);
    param.baseParam.size = sizeof param.baseParam;
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&param.baseParam);
    param.size = sizeof param;
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    param.userId = user_id();
    user.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = text;
    if (sceMsgDialogInitialize() < 0 && sceMsgDialogGetStatus() == ORBIS_COMMON_DIALOG_STATUS_NONE) {
        logf_("message dialog unavailable: %s", text);
        return;
    }
    if (sceMsgDialogOpen(&param) < 0) {
        logf_("message dialog didn't open: %s", text);
        return;
    }
    while (sceMsgDialogUpdateStatus() == ORBIS_COMMON_DIALOG_STATUS_RUNNING) usleep(16 * 1000);
    sceMsgDialogTerminate();
}

/* The dialog's magic encodes this block's address, so it must not move. */
static web_dialog_param g_web __attribute__((aligned(16)));

static int open_web_dialog(const char *url, int mode) {
    memset(&g_web, 0, sizeof g_web);
    g_web.base.size = sizeof g_web.base;
    g_web.base.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&g_web.base);
    g_web.size = sizeof g_web;
    g_web.mode = mode;
    g_web.user_id = user_id();
    g_web.url = url;
    if (mode == 2) {
        g_web.width = 1920;
        g_web.height = 1080;
        g_web.header_width = 1920;
    }
    return sceWebBrowserDialogOpen(&g_web);
}

/* Full screen, then the browser's own layout, then the Browser app. Returns
 * when the dialog is closed, or straight away for the Browser app. */
static void show_web_ui(const char *url) {
    int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);
    logf_("web browser dialog module: 0x%08x", (unsigned)rc);
    rc = sceWebBrowserDialogInitialize();
    logf_("web browser dialog init: 0x%08x", (unsigned)rc);
    if ((rc = open_web_dialog(url, 2)) != 0) {
        logf_("full-screen web dialog: 0x%08x, trying the default layout", (unsigned)rc);
        rc = open_web_dialog(url, 1);
    }
    if (rc == 0) {
        logf_("web dialog open on %s", url);
        int status;
        while ((status = sceWebBrowserDialogUpdateStatus()) == ORBIS_COMMON_DIALOG_STATUS_RUNNING ||
               status == ORBIS_COMMON_DIALOG_STATUS_INITIALIZED)
            usleep(16 * 1000);
        logf_("web dialog closed (status %d)", status);
        sceWebBrowserDialogClose();
        sceWebBrowserDialogTerminate();
        return;
    }
    logf_("web dialog: 0x%08x, opening the Browser app instead", (unsigned)rc);
    rc = launch_web_browser(url, NULL);
    logf_("Browser app: 0x%08x", (unsigned)rc);
    if (rc != 0) {
        char text[256];
        snprintf(text, sizeof text, "Open %s in the web browser to use RommPS.", url);
        message(text);
    }
}

int main(void) {
    char url[64], running[32], text[640];
    int installed, port;

    sceSystemServiceHideSplashScreen();
    mkdir(DATA_DIR, 0777);
    logf_("RommPS launcher %s", APP_VERSION);
    sceUserServiceInitialize(NULL);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    sceCommonDialogInitialize();

    installed = install_payload();
    port = web_port();
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", port);
    int up = payload_version(port, running, sizeof running);
    /* Opened just after GoldHEN launched it: give it time to open its web page. */
    for (int i = 0; !up && i < 20 && starting(); i++) {
        if (i == 0) logf_("RomM Sync is starting; waiting for it");
        usleep(500 * 1000);
        up = payload_version(port, running, sizeof running);
    }
    logf_("RomM Sync on port %d: %s%s", port, up ? "running " : "not running", running);

    if (installed < 0) {
        message("RommPS couldn't copy its sync service to /data/payloads. Copy romm-sync-ps4.elf there over FTP, then run it from GoldHEN Settings > Payloader.");
    } else if (!up) {
        diagnose(installed, port, text, sizeof text);
        message(text);
    } else {
        if (installed)
            message("A new version of RommPS's sync service is in /data/payloads. It takes over after the next jailbreak.");
        show_web_ui(url);
    }

    logf_("launcher done");
    sceSystemServiceLoadExec("exit", NULL);
    return 0;
}
