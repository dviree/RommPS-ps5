#include "http.h"
#include "jsonlean.h"

#include <pthread.h>

#include <curl/curl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "util.h"
#ifdef ROMM_EMBED_CA
#include "cacert_pem.h" /* generated from the SDK's Mozilla CA bundle */
#endif

static char g_ca_bundle[PATH_MAX_LEN];
/* Built-in roots plus the user's cacert.pem, if any. Only on the consoles,
 * which have no system CA store. */
static char *g_ca_blob;
static size_t g_ca_blob_len;
static int g_tls_verify = 1;
static char g_user_agent[64];

/* DNS answers and TLS sessions shared by every request, from any thread, so a
 * cover, a list page or a save skips the lookup and the full TLS handshake.
 * Not the connections themselves: libcurl doesn't support sharing those
 * between threads that use them at the same time. */
static CURLSH *g_share;
static pthread_mutex_t g_share_locks[CURL_LOCK_DATA_LAST];

/* The connections instead stay with the handles, which are kept between
 * requests: the next cover, list page or save to the same server goes over a
 * connection already open (two per handle at most), not a new one. A handle
 * can move from thread to thread as long as only one uses it at a time, so
 * each request takes an idle one (the last given back, whose connection is
 * the freshest) or a new one, and gives it back; POOL_MAX are kept. */
#define POOL_MAX 8
static CURL *g_pool[POOL_MAX];
static int g_npool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static CURL *handle_take(void) {
    CURL *c = NULL;
    pthread_mutex_lock(&g_pool_lock);
    if (g_npool) c = g_pool[--g_npool];
    pthread_mutex_unlock(&g_pool_lock);
    if (!c) return curl_easy_init();
    curl_easy_reset(c); /* the options go; its connections stay */
    return c;
}

static void handle_give(CURL *c) {
    pthread_mutex_lock(&g_pool_lock);
    const int kept = g_npool < POOL_MAX;
    if (kept) g_pool[g_npool++] = c;
    pthread_mutex_unlock(&g_pool_lock);
    if (!kept) curl_easy_cleanup(c);
}

static double mono_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A server that doesn't answer at all (no connection) isn't asked again by
 * every thread, each waiting out the connect timeout: requests to it fail at
 * once, and one at a time checks whether it's back, after 5 s, then twice as
 * long each time it still isn't, up to 30 s. */
#define DOWN_FIRST 5.0
#define DOWN_MOST  30.0
static pthread_mutex_t g_down_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_down[256]; /* its scheme://host:port, "" while all answer */
static double g_down_until, g_down_wait;
static int g_down_probing;

static void url_origin(const char *url, char *out, size_t n) {
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    snprintf(out, n, "%.*s", (int)(host - url + (ptrdiff_t)strcspn(host, "/?#")), url);
}

int http_down(const char *url) {
    char o[256];
    url_origin(url, o, sizeof o);
    pthread_mutex_lock(&g_down_lock);
    int down = g_down[0] && !strcmp(o, g_down);
    pthread_mutex_unlock(&g_down_lock);
    return down;
}

/* 0 to go ahead (*probe: this one checks whether it's back), -1 to fail at once. */
static int down_gate(const char *url, int *probe, http_resp *out) {
    char o[200];
    url_origin(url, o, sizeof o);
    *probe = 0;
    pthread_mutex_lock(&g_down_lock);
    int wait = 0;
    if (g_down[0] && !strcmp(o, g_down)) {
        if (g_down_probing || mono_sec() < g_down_until) wait = 1;
        else g_down_probing = *probe = 1;
    }
    pthread_mutex_unlock(&g_down_lock);
    if (!wait) return 0;
    memset(out, 0, sizeof *out);
    snprintf(out->error, sizeof out->error, "%s isn't answering", o);
    out->no_answer = 1;
    return -1;
}

static http_back_fn g_on_back;

void http_on_back(http_back_fn fn) { g_on_back = fn; }

static void down_note(const char *url, int no_answer, int probe) {
    char o[256];
    url_origin(url, o, sizeof o);
    int back = 0;
    pthread_mutex_lock(&g_down_lock);
    const int same = !strcmp(o, g_down);
    if (no_answer) {
        if (!same) {
            str_copy(g_down, sizeof g_down, o);
            g_down_wait = DOWN_FIRST;
            LOGW("%s isn't answering: asking again in %.0f s", o, g_down_wait);
        } else if (probe) {
            g_down_wait = g_down_wait * 2 > DOWN_MOST ? DOWN_MOST : g_down_wait * 2;
        }
        g_down_until = mono_sec() + g_down_wait;
    } else if (same) {
        LOGI("%s answers again", o);
        g_down[0] = 0;
        back = 1;
    }
    if (probe) g_down_probing = 0;
    pthread_mutex_unlock(&g_down_lock);
    if (back && g_on_back) g_on_back(o);
}

/* No connection at all: refused, no such host, or the connect timeout. */
static int no_answer(CURL *c, CURLcode rc) {
    if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST) return 1;
    if (rc != CURLE_OPERATION_TIMEDOUT) return 0;
    /* Timed out before a single line of an answer: a server that's hung
     * (paused, wedged) takes the connection and then says nothing, which is
     * as good as not being there. One that's slow mid-answer isn't that. */
    long status = 0, headers = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(c, CURLINFO_HEADER_SIZE, &headers);
    return status == 0 && headers == 0;
}

static void share_lock(CURL *c, curl_lock_data data, curl_lock_access access, void *ud) {
    (void)c, (void)access, (void)ud;
    if (data < CURL_LOCK_DATA_LAST) pthread_mutex_lock(&g_share_locks[data]);
}

static void share_unlock(CURL *c, curl_lock_data data, void *ud) {
    (void)c, (void)ud;
    if (data < CURL_LOCK_DATA_LAST) pthread_mutex_unlock(&g_share_locks[data]);
}

void http_global_init(const char *ca_bundle, int tls_verify) {
    curl_global_init(CURL_GLOBAL_ALL);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_share_locks[i], NULL);
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    snprintf(g_user_agent, sizeof g_user_agent, "%s/%s", plat_info()->client, APP_VERSION);
    if (ca_bundle && file_exists(ca_bundle)) str_copy(g_ca_bundle, sizeof g_ca_bundle, ca_bundle);
    g_tls_verify = tls_verify;
#ifdef ROMM_EMBED_CA
    /* A cacert.pem is added to the built-in roots, so a private CA works too. */
    size_t ulen = 0;
    char *user = g_ca_bundle[0] ? read_file(g_ca_bundle, &ulen) : NULL;
    g_ca_blob_len = sizeof CACERT_PEM - 1 + (user ? ulen + 1 : 0);
    g_ca_blob = malloc(g_ca_blob_len);
    if (g_ca_blob) {
        memcpy(g_ca_blob, CACERT_PEM, sizeof CACERT_PEM - 1);
        if (user) {
            g_ca_blob[sizeof CACERT_PEM - 1] = '\n';
            memcpy(g_ca_blob + sizeof CACERT_PEM, user, ulen);
        }
    }
    free(user);
    LOGI("libcurl %s, CA bundle: built-in%s%s", curl_version_info(CURLVERSION_NOW)->version,
         g_ca_bundle[0] ? " + " : "", g_ca_bundle);
#else
    LOGI("libcurl %s, CA bundle: %s", curl_version_info(CURLVERSION_NOW)->version,
         g_ca_bundle[0] ? g_ca_bundle : "(default)");
#endif
}

void http_set_tls_verify(int verify) { g_tls_verify = verify; }

void http_resp_free(http_resp *r) {
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

typedef struct {
    char *buf;
    size_t len, cap;
} membuf;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    membuf *m = ud;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4096;
        while (cap < m->len + n + 1) cap *= 2;
        char *nb = realloc(m->buf, cap);
        if (!nb) return 0;
        m->buf = nb;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, ptr, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

#ifdef ROMM_NETLOG
/* NETLOG=1 builds: curl's running commentary (DNS, connect, TLS, headers) goes
 * to the network log only, so a timeout says which step it died in. Headers
 * that carry credentials or session cookies are left out. */
static int curl_trace(CURL *c, curl_infotype type, char *data, size_t size, void *ud) {
    static const char *const tags[] = {[CURLINFO_TEXT] = "*", [CURLINFO_HEADER_IN] = "<", [CURLINFO_HEADER_OUT] = ">"};
    char line[300];
    (void)c;
    (void)ud;
    if (type != CURLINFO_TEXT && type != CURLINFO_HEADER_IN && type != CURLINFO_HEADER_OUT) return 0;
    while (size > 0) {
        size_t n = 0;
        while (n < size && data[n] != '\n') n++;
        size_t take = n;
        while (take > 0 && data[take - 1] == '\r') take--;
        int secret = !strncasecmp(data, "Authorization:", 14) || !strncasecmp(data, "Cookie:", 7) ||
                     !strncasecmp(data, "Set-Cookie:", 11);
        if (take && !(type != CURLINFO_TEXT && secret)) {
            snprintf(line, sizeof line, "curl %s %.*s", tags[type], (int)(take > 280 ? 280 : take), data);
            netlog_send(line);
        }
        if (n < size) n++;
        data += n;
        size -= n;
    }
    return 0;
}
#endif

/* errbuf, CURL_ERROR_SIZE bytes or NULL, gets curl's own account of a
 * failure: which host, which phase, how long it waited. */
static CURL *easy_new(const char *url, struct curl_slist **hdrs, const char *auth, int verify, char *errbuf) {
    CURL *c = handle_take();
    if (!c) return NULL;
    if (errbuf) {
        errbuf[0] = 0;
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    }
    if (g_share) curl_easy_setopt(c, CURLOPT_SHARE, g_share);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, g_user_agent);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_MAXCONNECTS, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* Give up on stalled transfers rather than using a total timeout. */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
#ifdef ROMM_NETLOG
    curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, curl_trace);
    curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
#endif
    if (g_ca_blob) {
        struct curl_blob blob = {g_ca_blob, g_ca_blob_len, CURL_BLOB_NOCOPY};
        curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    } else if (g_ca_bundle[0]) {
        curl_easy_setopt(c, CURLOPT_CAINFO, g_ca_bundle);
    }
    if (!verify) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    *hdrs = curl_slist_append(*hdrs, "Accept: application/json");
    if (auth && *auth) {
        char h[1024];
        snprintf(h, sizeof h, "Authorization: %s", auth);
        *hdrs = curl_slist_append(*hdrs, h);
    }
    return c;
}

static int finish(CURL *c, CURLcode rc, membuf *m, http_resp *out, const char *url, const char *errbuf, int probe) {
    memset(out, 0, sizeof *out);
    out->no_answer = rc != CURLE_OK && no_answer(c, rc);
    out->aborted = rc == CURLE_ABORTED_BY_CALLBACK;
    down_note(url, out->no_answer, probe);
    if (rc != CURLE_OK) {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
        /* A download (CURLOPT_FAILONERROR) stopped by the server's answer:
         * say which, so a 404 isn't "HTTP response code said error". */
        if (rc == CURLE_HTTP_RETURNED_ERROR) {
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
            snprintf(out->error, sizeof out->error, "HTTP %ld", out->status);
        }
        /* "Timeout was reached" alone can't tell a server that never answered
         * from one that went quiet halfway, or a console with no network. */
        long os_err = 0;
        curl_easy_getinfo(c, CURLINFO_OS_ERRNO, &os_err);
        const char *detail = errbuf && errbuf[0] ? errbuf : out->error;
        if (os_err) LOGW("HTTP %s: %s (errno %ld: %s)", url, detail, os_err, strerror((int)os_err));
        else LOGW("HTTP %s: %s", url, detail);
        free(m ? m->buf : NULL);
        return -1;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
    if (m) {
        out->body = m->buf;
        out->len = m->len;
    }
    if (out->status >= 400) {
        snprintf(out->error, sizeof out->error, "HTTP %ld", out->status);
        LOGD("HTTP %ld for %s: %.200s", out->status, url, out->body ? out->body : "");
    }
    return 0;
}

/* A reply kept lean: the members named in drop never reach memory. */
typedef struct {
    membuf m;
    jlean j;
} lean_buf;

static int lean_put(void *ud, const char *bytes, size_t n) {
    return mem_write((char *)bytes, 1, n, ud) == n;
}

static size_t lean_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    lean_buf *l = ud;
    return jlean_feed(&l->j, ptr, size * nmemb, lean_put, &l->m) ? size * nmemb : 0;
}

static int request_impl(const char *method, const char *url, const char *auth, const char *content_type,
                        const void *body, size_t body_len, int verify, const char *const *drop, http_resp *out) {
    memset(out, 0, sizeof *out);
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, verify, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        return -1;
    }
    if (content_type) {
        char h[256];
        snprintf(h, sizeof h, "Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, h);
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    /* Compressed answers where the server offers them: a page of RomM's games
     * is about 1.4 MB of JSON, and a seventh of that gzipped. */
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    if (strcmp(method, "GET") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(body ? body_len : 0));
    }
    lean_buf lean;
    if (drop) {
        memset(&lean, 0, sizeof lean);
        jlean_init(&lean.j, drop);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, lean_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &lean);
    } else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    }
    CURLcode rc = curl_easy_perform(c);
    if (drop) m = lean.m;
    int r = finish(c, rc, &m, out, url, errbuf, probe);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return r;
}

int http_request(const char *method, const char *url, const char *auth, const char *content_type,
                 const void *body, size_t body_len, http_resp *out) {
    return request_impl(method, url, auth, content_type, body, body_len, g_tls_verify, NULL, out);
}

int http_get_lean(const char *url, const char *auth, const char *const *drop, http_resp *out) {
    return request_impl("GET", url, auth, NULL, NULL, 0, g_tls_verify, drop, out);
}

int http_get_verified(const char *url, http_resp *out) {
    return request_impl("GET", url, NULL, NULL, NULL, 0, 1, NULL, out);
}

int http_tls_untrusted(const char *url) {
    struct curl_slist *hdrs = NULL;
    CURL *c = easy_new(url, &hdrs, NULL, 1, NULL);
    if (!c) return 0;
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return rc == CURLE_PEER_FAILED_VERIFICATION;
}

int http_upload_file(const char *method, const char *url, const char *auth, const char *field,
                     const char *file_path, const char *upload_name, http_resp *out) {
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    struct curl_slist *hdrs = NULL;
    membuf m = {0};
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, g_tls_verify, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        return -1;
    }
    curl_mime *mime = curl_mime_init(c);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, field);
    curl_mime_filedata(part, file_path);
    curl_mime_filename(part, upload_name ? upload_name : path_basename(file_path));
    curl_mime_type(part, "application/octet-stream");
    /* "Expect: 100-continue" confuses some reverse proxies. Just don't. */
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    if (strcmp(method, "POST") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(c);
    int r = finish(c, rc, &m, out, url, errbuf, probe);
    curl_mime_free(mime);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return r;
}

typedef struct {
    http_progress_fn fn;
    void *ud;
    int64_t offset;
} progress_ctx;

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                   curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    progress_ctx *p = ud;
    if (!p->fn) return 0;
    return p->fn(p->ud, p->offset + dlnow, dltotal ? p->offset + dltotal : 0);
}

/* Downloads read and write in bigger blocks than curl's 16 KB, and ask for a
 * bigger socket buffer, which keeps a slow or bursty link (PS4 Wi-Fi) busier.
 * Kept modest: memory is tight for the PS4 payload. */
#define DOWNLOAD_BUFFER (256 * 1024)
#define DOWNLOAD_SOCKET_BUFFER (1024 * 1024)

static int big_rcvbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    int size = DOWNLOAD_SOCKET_BUFFER;
    (void)ud;
    if (purpose == CURLSOCKTYPE_IPCXN) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    return CURL_SOCKOPT_OK;
}

static int download_impl(const char *url, const char *auth, const char *dest, int resume, int verify,
                         http_progress_fn progress, void *ud, http_resp *out) {
    int probe;
    if (down_gate(url, &probe, out)) return -1;
    char part[PATH_MAX_LEN];
    snprintf(part, sizeof part, "%s.part", dest);
    mkdir_parent(dest);

    int64_t offset = resume ? file_size(part) : -1;
    if (offset < 0) offset = 0;
    FILE *f = fopen(part, offset > 0 ? "ab" : "wb");
    if (!f) {
        down_note(url, 0, probe);
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "cannot open %s", part);
        return -1;
    }
    setvbuf(f, NULL, _IOFBF, DOWNLOAD_BUFFER);

    struct curl_slist *hdrs = NULL;
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = easy_new(url, &hdrs, auth, verify, errbuf);
    if (!c) {
        down_note(url, 0, probe);
        fclose(f);
        return -1;
    }
    progress_ctx pc = {progress, ud, offset};
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    if (offset > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)offset);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)DOWNLOAD_BUFFER);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, big_rcvbuf);
    double started = mono_sec();
    CURLcode rc = curl_easy_perform(c);
    fclose(f);
    double secs = mono_sec() - started;
    curl_off_t got = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &got);
    if (rc == CURLE_OK && got > 1024 * 1024 && secs > 0)
        LOGI("downloaded %.1f MB in %.1fs (%.2f MB/s)", got / 1048576.0, secs, got / 1048576.0 / secs);

    int r = finish(c, rc, NULL, out, url, errbuf, probe);
    if (r == 0 && out->status == 200 && offset > 0) {
        /* The server ignored Range, so start over. */
        LOGW("server ignored Range for %s; restarting download", url);
        curl_slist_free_all(hdrs);
        handle_give(c);
        unlink(part);
        return download_impl(url, auth, dest, 0, verify, progress, ud, out);
    }
    if (r == 0 && out->status >= 200 && out->status < 300) {
        if (move_file(part, dest) != 0) {
            snprintf(out->error, sizeof out->error, "cannot move into %s", dest);
            r = -1;
        }
    } else if (r == 0) {
        r = -1;
    }
    if (r != 0 && rc == CURLE_HTTP_RETURNED_ERROR) unlink(part);
    curl_slist_free_all(hdrs);
    handle_give(c);
    return r;
}

int http_download(const char *url, const char *auth, const char *dest, int resume,
                  http_progress_fn progress, void *ud, http_resp *out) {
    return download_impl(url, auth, dest, resume, g_tls_verify, progress, ud, out);
}

int http_download_verified(const char *url, const char *dest, http_progress_fn progress, void *ud,
                           http_resp *out) {
    return download_impl(url, NULL, dest, 0, 1, progress, ud, out);
}

char *http_escape(const char *s) {
    char *e = curl_easy_escape(NULL, s, 0);
    char *r = e ? strdup(e) : strdup("");
    curl_free(e);
    return r;
}
