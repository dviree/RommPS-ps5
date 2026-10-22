#include "jsonlean.h"

#include <string.h>

void jlean_init(jlean *j, const char *const *drop) {
    memset(j, 0, sizeof *j);
    j->drop = drop;
}

static int dropped(const jlean *j) {
    if (j->key_long) return 0;
    /* key holds the name with its quotes */
    for (const char *const *d = j->drop; d && *d; d++)
        if ((size_t)j->key_len == strlen(*d) + 2 && !memcmp(j->key + 1, *d, strlen(*d))) return 1;
    return 0;
}

/* The output for one call goes through a small buffer, flushed when full. */
typedef struct {
    char buf[512];
    size_t n;
    jlean_put put;
    void *ud;
    int ok;
} sink;

static void out(sink *s, const char *b, size_t n) {
    if (!s->ok) return;
    if (s->n + n > sizeof s->buf) {
        if (s->n && !s->put(s->ud, s->buf, s->n)) s->ok = 0;
        s->n = 0;
        if (n > sizeof s->buf) {
            if (s->ok && !s->put(s->ud, b, n)) s->ok = 0;
            return;
        }
    }
    memcpy(s->buf + s->n, b, n);
    s->n += n;
}

static void out1(sink *s, char c) { out(s, &c, 1); }

/* A member's name kept: its comma first, when one was written before it. */
static void keep_key(jlean *j, sink *s) {
    if (j->kept[j->depth - 1]) out1(s, ',');
    j->kept[j->depth - 1] = 1;
    out(s, j->key, (size_t)j->key_len);
}

int jlean_feed(jlean *j, const char *in, size_t n, jlean_put put, void *ud) {
    sink s = {.put = put, .ud = ud, .ok = 1};
    for (size_t i = 0; i < n && s.ok; i++) {
        const char c = in[i];
        if (j->skipping) {
            /* The value of a dropped member: up to where it ends. */
            if (j->skip_str) {
                if (j->skip_esc) j->skip_esc = 0;
                else if (c == '\\') j->skip_esc = 1;
                else if (c == '"') {
                    j->skip_str = 0;
                    if (!j->skip_nest) j->skipping = 0;
                }
                continue;
            }
            if (!j->skip_started) {
                if (c == ':' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
                j->skip_started = 1;
            }
            if (c == '"') {
                j->skip_str = 1;
                continue;
            }
            if (c == '{' || c == '[') {
                j->skip_nest++;
                continue;
            }
            if (j->skip_nest && (c == '}' || c == ']')) {
                if (!--j->skip_nest) j->skipping = 0;
                continue;
            }
            if (!j->skip_nest && (c == ',' || c == '}' || c == ']')) {
                j->skipping = 0; /* a number or literal ended: the character is the object's */
            } else {
                continue;
            }
        }
        if (j->in_key) {
            if (j->key_len < JLEAN_KEY + 2) j->key[j->key_len++] = c;
            else j->key_long = 1;
            const int closing = !j->esc && c == '"';
            j->esc = !j->esc && c == '\\';
            if (j->key_long) {
                /* Longer than any name dropped: kept, written as it comes. */
                if (j->key_len == JLEAN_KEY + 2) {
                    keep_key(j, &s);
                    j->key_len++;
                }
                out1(&s, c);
                if (closing) j->in_key = j->expect_key = 0;
                continue;
            }
            if (!closing) continue;
            j->in_key = 0;
            j->expect_key = 0;
            if (dropped(j)) {
                j->skipping = 1;
                j->skip_started = j->skip_nest = j->skip_str = j->skip_esc = 0;
            } else {
                keep_key(j, &s);
            }
            continue;
        }
        if (j->in_str) {
            out1(&s, c);
            if (j->esc) j->esc = 0;
            else if (c == '\\') j->esc = 1;
            else if (c == '"') j->in_str = 0;
            continue;
        }
        const int in_obj = j->depth > 0 && !j->deep && j->obj[j->depth - 1];
        switch (c) {
        case '{':
        case '[':
            if (j->deep || j->depth == JLEAN_DEPTH) {
                j->deep++;
            } else {
                j->obj[j->depth] = c == '{';
                j->kept[j->depth] = 0;
                j->depth++;
                j->expect_key = c == '{';
            }
            out1(&s, c);
            break;
        case '}':
        case ']':
            if (j->deep) j->deep--;
            else if (j->depth) j->depth--;
            j->expect_key = 0;
            out1(&s, c);
            break;
        case ',':
            if (in_obj) j->expect_key = 1; /* written with the next member kept */
            else out1(&s, c);
            break;
        case ':':
            j->expect_key = 0;
            out1(&s, c);
            break;
        case '"':
            if (in_obj && j->expect_key) {
                j->in_key = 1;
                j->key_len = 0;
                j->key_long = 0;
                j->esc = 0;
                j->key[j->key_len++] = c;
            } else {
                j->in_str = 1;
                j->esc = 0;
                out1(&s, c);
            }
            break;
        default:
            if (in_obj && j->expect_key && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) break;
            out1(&s, c);
        }
    }
    if (s.ok && s.n && !put(ud, s.buf, s.n)) s.ok = 0;
    return s.ok;
}
