#include "n64.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md5.h"
#include "n64_save.h" /* sigil: the regions, and the .srm in and out */
#include "util.h"

/* The files, by region: .mpk holds the four paks, one after another. */
static const char *const EXTS[] = {".eep", ".sra", ".fla", ".mpk"};
#define NEXTS (sizeof EXTS / sizeof *EXTS)

/* A dump's byte order, from its first word: 0 .z64 (as the N64 reads it),
 * 2 .v64 (16-bit words swapped), 4 .n64 (32-bit words reversed), -1 none. */
static int rom_order(const unsigned char *b) {
    if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return 0;
    if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return 2;
    if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return 4;
    return -1;
}

static void to_z64(unsigned char *b, size_t n, int order) {
    for (size_t i = 0; order && i + order <= n; i += (size_t)order)
        for (int k = 0; k < order / 2; k++) {
            const unsigned char t = b[i + k];
            b[i + k] = b[i + order - 1 - k];
            b[i + order - 1 - k] = t;
        }
}

int n64_rom_md5(const char *rom, char out[33]) {
    FILE *fp = fopen(rom, "rb");
    if (!fp) return -1;
    unsigned char *buf = malloc(1 << 16), sum[16];
    romm_md5_CTX ctx;
    romm_md5_Init(&ctx);
    int order = -2;
    size_t got;
    while (buf && (got = fread(buf, 1, 1 << 16, fp)) > 0) {
        if (order == -2 && (got < 4 || (order = rom_order(buf)) < 0)) break;
        to_z64(buf, got, order);
        romm_md5_Update(&ctx, buf, (unsigned long)got);
    }
    fclose(fp);
    free(buf);
    if (order < 0) return -1;
    romm_md5_Final(sum, &ctx);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02X", sum[i]);
    return 0;
}

/* The header's CRCs ("XXXXXXXX YYYYYYYY", as the database writes them) and
 * its name, trailing spaces off. */
static int rom_header(const char *rom, char crc[18], char name[21]) {
    unsigned char h[0x40];
    FILE *fp = fopen(rom, "rb");
    const size_t got = fp ? fread(h, 1, sizeof h, fp) : 0;
    if (fp) fclose(fp);
    const int order = got == sizeof h ? rom_order(h) : -1;
    if (order < 0) return -1;
    to_z64(h, sizeof h, order);
    snprintf(crc, 18, "%02X%02X%02X%02X %02X%02X%02X%02X", h[0x10], h[0x11], h[0x12], h[0x13], h[0x14], h[0x15],
             h[0x16], h[0x17]);
    memcpy(name, h + 0x20, 20);
    name[20] = 0;
    for (int i = 19; i >= 0 && (name[i] == ' ' || name[i] == 0); i--) name[i] = 0;
    return 0;
}

/* The body of a database section, "[key]" on a line of its own. */
static const char *section(const char *ini, const char *key) {
    const size_t kl = strlen(key);
    for (const char *p = strchr(ini, '['); p; p = strchr(p + 1, '['))
        if ((p == ini || p[-1] == '\n') && !strncmp(p + 1, key, kl) && p[1 + kl] == ']') return p;
    return NULL;
}

/* A key's value in a section, up to the next one. */
static int value(const char *sec, const char *key, char *out, size_t n) {
    const size_t kl = strlen(key);
    for (const char *l = strchr(sec, '\n'); l && l[1] && l[1] != '['; l = strchr(l + 1, '\n')) {
        if (strncmp(l + 1, key, kl) || l[1 + kl] != '=') continue;
        const char *v = l + 2 + kl;
        size_t k = 0;
        while (v[k] && v[k] != '\r' && v[k] != '\n' && k + 1 < n) k++;
        memcpy(out, v, k);
        out[k] = 0;
        return 0;
    }
    return -1;
}

/* GoodName, its own or the one of the entry it refers to (RefMD5). */
static int good_name(const char *ini, const char *sec, char *out, size_t n) {
    char ref[40];
    if (value(sec, "GoodName", out, n) == 0) return 0;
    if (value(sec, "RefMD5", ref, sizeof ref) != 0 || !(sec = section(ini, ref))) return -1;
    return value(sec, "GoodName", out, n);
}

/* mupen64plus's get_save_filename(), save file name format 1. */
static int made_name(const char *ini_path, const char *rom, const char *md5, char *out, size_t n) {
    char *ini = read_file(ini_path, NULL);
    if (!ini) return -1;
    char good[256] = "", crc[18], header[21] = "", line[40];
    const char *sec = section(ini, md5);
    if (rom_header(rom, crc, header) != 0) crc[0] = 0;
    if (!sec && crc[0]) { /* by the header's CRCs, as it falls back to */
        snprintf(line, sizeof line, "\nCRC=%s", crc);
        const char *at = strstr(ini, line);
        while (at && at > ini && !(at[0] == '\n' && at[1] == '[')) at--;
        if (at && at[0] == '\n') sec = at + 1;
    }
    if (!sec || good_name(ini, sec, good, sizeof good) != 0) good[0] = 0;
    free(ini);
    if (good[0] && !strstr(good, "(unknown rom)"))
        snprintf(out, n, "%.32s-%.8s", good, md5);
    else if (header[0])
        snprintf(out, n, "%s-%.8s", header, md5);
    else
        snprintf(out, n, "unknown-%.8s", md5);
    for (char *c = out; *c; c++)
        if (strchr(":<>\"/\\|?*", *c)) *c = '_';
    return 0;
}

int n64_mupen_name(const char *dir, const char *ini, const char *rom, const char *md5, char *out, size_t n) {
    char tail[16];
    snprintf(tail, sizeof tail, "-%.8s", md5);
    DIR *d = opendir(dir);
    struct dirent *de;
    int found = 0;
    while (d && !found && (de = readdir(d))) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || (size_t)(dot - de->d_name) < strlen(tail) || strncmp(dot - strlen(tail), tail, strlen(tail))) continue;
        for (size_t i = 0; i < NEXTS && !found; i++)
            if (!strcmp(dot, EXTS[i]) && (size_t)(dot - de->d_name) < n) {
                snprintf(out, n, "%.*s", (int)(dot - de->d_name), de->d_name);
                found = 1;
            }
    }
    if (d) closedir(d);
    return found ? 0 : made_name(ini, rom, md5, out, n);
}

/* The newest of the game's files, 0 when there's none. */
static time_t files_mtime(const char *dir, const char *name) {
    time_t newest = 0;
    for (size_t i = 0; i < NEXTS; i++) {
        char p[PATH_MAX_LEN];
        snprintf(p, sizeof p, "%s/%s%s", dir, name, EXTS[i]);
        const time_t t = file_mtime(p);
        if (t > newest) newest = t;
    }
    return newest;
}

int n64_fold(const char *dir, const char *name, const char *shadow) {
    const time_t files = files_mtime(dir, name), had = file_mtime(shadow);
    if (!files || files <= had) return 0;
    n64_save *s = calloc(1, sizeof *s);
    uint8_t *blob = malloc(N64_BLOB_SIZE);
    size_t old_len = 0;
    char *old = read_file(shadow, &old_len);
    int rc = -1;
    if (s && blob) {
        for (size_t i = 0; i < NEXTS; i++) {
            char p[PATH_MAX_LEN];
            size_t len = 0;
            snprintf(p, sizeof p, "%s/%s%s", dir, name, EXTS[i]);
            char *data = read_file(p, &len);
            if (!data) continue;
            if (i == 3) /* the paks, one after another */
                for (size_t k = 0; k < N64_PAKS && len >= (k + 1) * N64_PAK_SIZE; k++)
                    n64_take(s, N64_PAK1 + (int)k, (const uint8_t *)data + k * N64_PAK_SIZE, N64_PAK_SIZE);
            else
                n64_take(s, i == 0 ? N64_EEPROM : i == 1 ? N64_SRAM : N64_FLASH, (const uint8_t *)data, len);
            free(data);
        }
        n64_give_blob(s, (const uint8_t *)old, old_len, blob);
        if (old && old_len == N64_BLOB_SIZE && !memcmp(old, blob, N64_BLOB_SIZE))
            rc = 0; /* the same save, written again */
        else if (mkdir_parent(shadow) == 0 && write_file_atomic(shadow, blob, N64_BLOB_SIZE) == 0)
            rc = 1;
        if (rc == 1) set_file_mtime(shadow, files);
    }
    free(old);
    free(blob);
    free(s);
    return rc;
}

/* One file of the game's: written whole and dated as the shadow. */
static int put(const char *dir, const char *name, const char *ext, const uint8_t *data, size_t len, time_t t) {
    char p[PATH_MAX_LEN];
    snprintf(p, sizeof p, "%s/%s%s", dir, name, ext);
    if (mkdir_parent(p) != 0 || write_file_atomic(p, data, len) != 0) return -1;
    set_file_mtime(p, t);
    return 0;
}

int n64_unfold(const char *dir, const char *name, const char *shadow, int force) {
    const time_t had = file_mtime(shadow);
    if (!had || (!force && files_mtime(dir, name) >= had)) return 0;
    size_t len = 0;
    char *blob = read_file(shadow, &len);
    n64_save *s = calloc(1, sizeof *s);
    uint8_t *buf = malloc(N64_PAKS * N64_PAK_SIZE);
    int rc = blob && s && buf ? 0 : -1, wrote = 0;
    if (rc == 0) {
        n64_take_blob(s, (const uint8_t *)blob, len);
        /* EEPROM at its largest (mupen64plus reads 0x800), SRAM and flash whole. */
        static const struct { int region; const char *ext; } ONE[] = {
            {N64_EEPROM, ".eep"}, {N64_SRAM, ".sra"}, {N64_FLASH, ".fla"}};
        for (size_t i = 0; i < sizeof ONE / sizeof *ONE && rc == 0; i++) {
            if (!s->present[ONE[i].region]) continue;
            const size_t n = n64_give(s, ONE[i].region, buf);
            rc = put(dir, name, ONE[i].ext, buf, n, had);
            wrote += rc == 0;
        }
        int paks = 0;
        for (int k = 0; k < (int)N64_PAKS; k++) paks |= s->present[N64_PAK1 + k];
        if (paks && rc == 0) {
            /* The paks the save doesn't use keep what the file has, else come formatted. */
            char p[PATH_MAX_LEN];
            size_t have = 0;
            snprintf(p, sizeof p, "%s/%s.mpk", dir, name);
            char *mpk = read_file(p, &have);
            for (size_t k = 0; k < N64_PAKS; k++) {
                uint8_t *at = buf + k * N64_PAK_SIZE;
                if (s->present[N64_PAK1 + k]) n64_give(s, N64_PAK1 + (int)k, at);
                else if (mpk && have >= (k + 1) * N64_PAK_SIZE) memcpy(at, mpk + k * N64_PAK_SIZE, N64_PAK_SIZE);
                else n64_pak_formatted(at);
            }
            free(mpk);
            rc = put(dir, name, ".mpk", buf, N64_PAKS * N64_PAK_SIZE, had);
            wrote += rc == 0;
        }
    }
    free(blob);
    free(s);
    free(buf);
    return rc < 0 ? -1 : wrote > 0;
}
