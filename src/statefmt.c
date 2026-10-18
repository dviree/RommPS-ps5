#include "statefmt.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
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

int state_unrzip_file(const char *in_path, const char *out_path) {
    unsigned char head[20], *packed = NULL, *chunk = NULL;
    size_t packed_cap = 0;
    uint64_t got = 0, total = 0;
    int ok = 0;
    FILE *in = fopen(in_path, "rb"), *out = NULL;
    if (!in || fread(head, 1, sizeof head, in) != sizeof head || memcmp(head, "#RZIPv\x01#", 8) != 0) goto done;
    uint32_t chunk_size = le32(head + 8);
    total = le64(head + 12);
    if (!chunk_size || chunk_size > 64u << 20 || !total || total > (uint64_t)256 << 20) goto done;
    if (!(chunk = malloc(chunk_size)) || !(out = fopen(out_path, "wb"))) goto done;
    while (got < total) {
        unsigned char lenb[4];
        if (fread(lenb, 1, 4, in) != 4) goto done;
        uint32_t len = le32(lenb);
        if (len > packed_cap) {
            unsigned char *grown = realloc(packed, len);
            if (!grown) goto done;
            packed = grown;
            packed_cap = len;
        }
        uLongf room = chunk_size;
        if (fread(packed, 1, len, in) != len || uncompress(chunk, &room, packed, len) != Z_OK ||
            fwrite(chunk, 1, room, out) != room)
            goto done;
        got += room;
    }
    ok = got == total;
done:
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    free(packed);
    free(chunk);
    if (!ok && out) remove(out_path);
    return ok ? 0 : -1;
}

int state_wrap_file(const char *in_path, const char *out_path) {
    unsigned char buf[64 * 1024], head[16];
    int ok = 0;
    FILE *in = fopen(in_path, "rb"), *out = NULL;
    if (!in || fseek(in, 0, SEEK_END) != 0) goto done;
    long size = ftell(in);
    if (size <= 0 || size > (256L << 20) || fseek(in, 0, SEEK_SET) != 0 || !(out = fopen(out_path, "wb"))) goto done;
    memcpy(head, "RASTATE\x01" "MEM ", 12);
    put32(head + 12, (uint32_t)size);
    if (fwrite(head, 1, 16, out) != 16) goto done;
    long left = size;
    while (left > 0) {
        size_t want = left < (long)sizeof buf ? (size_t)left : sizeof buf;
        if (fread(buf, 1, want, in) != want || fwrite(buf, 1, want, out) != want) goto done;
        left -= (long)want;
    }
    static const unsigned char zeros[8], end[8] = {'E', 'N', 'D', ' ', 0, 0, 0, 0};
    size_t pad = (8 - (size_t)size % 8) % 8;
    ok = fwrite(zeros, 1, pad, out) == pad && fwrite(end, 1, 8, out) == 8;
done:
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    if (!ok && out) remove(out_path);
    return ok ? 0 : -1;
}

/* Copies len bytes from in (already positioned) to out. */
static int copy_bytes(FILE *in, FILE *out, uint64_t len) {
    unsigned char buf[64 * 1024];
    while (len > 0) {
        size_t want = len < sizeof buf ? (size_t)len : sizeof buf;
        if (fread(buf, 1, want, in) != want || fwrite(buf, 1, want, out) != want) return -1;
        len -= want;
    }
    return 0;
}

int state_unwrap_file(const char *in_path, const char *out_path) {
    unsigned char head[20];
    char unpacked[1100] = "";
    int ok = 0;
    FILE *in = fopen(in_path, "rb"), *out = NULL;
    size_t got = in ? fread(head, 1, sizeof head, in) : 0;
    if (!in) return -1;
    if (state_kind_of(head, got) == STATE_RZIP) {
        /* Unpacked next to the result first, then unwrapped from there. */
        fclose(in);
        snprintf(unpacked, sizeof unpacked, "%s.unrzip", out_path);
        if (state_unrzip_file(in_path, unpacked) != 0) return -1;
        in = fopen(unpacked, "rb");
        got = in ? fread(head, 1, sizeof head, in) : 0;
        if (!in) goto done;
    }
    if (!(out = fopen(out_path, "wb"))) goto done;
    if (state_kind_of(head, got) == STATE_RASTATE) {
        long off = 8; /* blocks of a tag, a u32 size and the data padded to 8 */
        for (;;) {
            unsigned char block[8];
            if (fseek(in, off, SEEK_SET) != 0 || fread(block, 1, 8, in) != 8 || !memcmp(block, "END ", 4)) goto done;
            uint32_t size = le32(block + 4);
            if (!memcmp(block, "MEM ", 4)) {
                ok = copy_bytes(in, out, size) == 0;
                break;
            }
            off += 8 + (long)(((size_t)size + 7) & ~(size_t)7);
        }
    } else { /* already bare: as it is */
        unsigned char buf[64 * 1024];
        size_t n;
        ok = fseek(in, 0, SEEK_SET) == 0;
        while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    }
done:
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    if (unpacked[0]) remove(unpacked);
    if (!ok) remove(out_path);
    return ok ? 0 : -1;
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
