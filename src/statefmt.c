#include "statefmt.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "util.h"

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

state_kind state_kind_of(const unsigned char *b, size_t n) {
    if (n >= 20 && !memcmp(b, "#RZIPv\x01#", 8)) return STATE_RZIP;
    if (n >= 16 && !memcmp(b, "RASTATE\x01", 8)) return STATE_RASTATE;
    return STATE_RAW;
}

/* Unpacks an RZIP stream. Malloc'd, or NULL. */
static unsigned char *unrzip(const unsigned char *b, size_t n, size_t *out_n) {
    uint64_t total = le64(b + 12);
    if (total == 0 || total > (uint64_t)256 << 20) return NULL; /* states are tens of MB at most */
    unsigned char *out = malloc((size_t)total);
    if (!out) return NULL;
    size_t off = 20, got = 0;
    while (off + 4 <= n && got < total) {
        uint32_t len = le32(b + off);
        off += 4;
        if (len > n - off) break;
        uLongf room = (uLongf)(total - got);
        if (uncompress(out + got, &room, b + off, len) != Z_OK) break;
        got += room;
        off += len;
    }
    if (got != total) {
        free(out);
        return NULL;
    }
    *out_n = (size_t)total;
    return out;
}

/* The "MEM " block of a RASTATE, pointed into b. */
static const unsigned char *rastate_mem(const unsigned char *b, size_t n, size_t *mem_n) {
    size_t off = 8;
    while (off + 8 <= n) {
        uint32_t size = le32(b + off + 4);
        const unsigned char *tag = b + off;
        off += 8;
        if (size > n - off) return NULL;
        if (!memcmp(tag, "MEM ", 4)) {
            *mem_n = size;
            return b + off;
        }
        if (!memcmp(tag, "END ", 4)) return NULL;
        off += ((size_t)size + 7) & ~(size_t)7;
    }
    return NULL;
}

unsigned char *state_to_raw(const unsigned char *b, size_t n, size_t *out_n) {
    unsigned char *unpacked = NULL;
    if (state_kind_of(b, n) == STATE_RZIP) {
        unpacked = unrzip(b, n, &n);
        if (!unpacked) return NULL;
        b = unpacked;
    }
    unsigned char *out = NULL;
    if (state_kind_of(b, n) == STATE_RASTATE) {
        size_t mem_n;
        const unsigned char *mem = rastate_mem(b, n, &mem_n);
        if (mem && (out = malloc(mem_n ? mem_n : 1))) {
            memcpy(out, mem, mem_n);
            *out_n = mem_n;
        }
    } else if ((out = malloc(n ? n : 1))) { /* already bare */
        memcpy(out, b, n);
        *out_n = n;
    }
    free(unpacked);
    return out;
}

unsigned char *state_to_rastate(const unsigned char *b, size_t n, size_t *out_n) {
    state_kind k = state_kind_of(b, n);
    if (k == STATE_RZIP) return unrzip(b, n, out_n);
    if (k == STATE_RASTATE) {
        unsigned char *out = malloc(n);
        if (out) {
            memcpy(out, b, n);
            *out_n = n;
        }
        return out;
    }
    size_t pad = (8 - n % 8) % 8, total = 16 + n + pad + 8;
    unsigned char *out = calloc(1, total);
    if (!out) return NULL;
    memcpy(out, "RASTATE\x01" "MEM ", 12);
    put32(out + 12, (uint32_t)n);
    memcpy(out + 16, b, n);
    memcpy(out + 16 + n + pad, "END ", 4); /* size 0 */
    *out_n = total;
    return out;
}

const char *state_core_of(const unsigned char *b, size_t n) {
    size_t mem_n = n;
    if (state_kind_of(b, n) == STATE_RASTATE) {
        b = rastate_mem(b, n, &mem_n);
        if (!b) return NULL;
    }
    static const struct {
        const char *magic, *core;
    } sigs[] = {
        {"STv4 PCSXra", "pcsx_rearmed"},
        {"MDFNSVST", "mednafen_psx_hw"},
        {"DUCC", "swanstation"},
    };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        size_t l = strlen(sigs[i].magic);
        if (mem_n >= l && !memcmp(b, sigs[i].magic, l)) return sigs[i].core;
    }
    return NULL;
}

void emulator_key(const char *name, char *out, size_t n) {
    if (!n) return;
    static const char *const prefixes[] = {"retroarch-", "retroarch_", "libretro-", "libretro_"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t l = strlen(prefixes[i]);
        if (!strncasecmp(name, prefixes[i], l)) {
            name += l;
            break;
        }
    }
    size_t o = 0;
    for (; *name && o + 1 < n; name++) {
        unsigned char ch = (unsigned char)*name;
        out[o++] = ch == '-' || ch == ' ' ? '_' : (char)tolower(ch);
    }
    out[o] = 0;
    char *lib = strstr(out, "_libretro");
    if (lib) *lib = 0;
    /* Beetle PSX is "mednafen_psx_hw" in RetroArch and RomM's web player. */
    static const char *const aliases[][2] = {{"beetle_psx_hw", "mednafen_psx_hw"}, {"beetle_psx", "mednafen_psx"}};
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
        if (!strcmp(out, aliases[i][0])) str_copy(out, n, aliases[i][1]);
}
