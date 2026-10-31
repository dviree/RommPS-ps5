#include "xboxhdd.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "util.h"

/* The retail partition table, which xemu's images keep: E: is the data
 * drive, 0xABE80000 in and 4.7 GiB long. */
#define E_OFFSET 0xABE80000ULL
#define E_SIZE   0x1312D6000ULL
#define DIRENT   64
#define ATTR_DIR 0x10
#define NAME_MAX_FATX 42

struct xhdd {
    int fd;
    /* qcow2: guest offsets go through two tables to clusters in the file */
    int qcow;
    unsigned cbits;
    uint64_t cs, *l1, **l2, l1_at;
    unsigned char *zbuf; /* the last compressed cluster read, inflated */
    uint64_t zentry;
    uint32_t l1n;
    uint64_t refs_at, refs_n, eof; /* writing: the refcount table, the file's end */
    int refbits;
    /* FATX on E: */
    uint32_t ccs, nclus, root;
    int esz;
    uint64_t fat, data;
    unsigned char *fatbuf;
    size_t fat_lo, fat_hi; /* writing: the bytes of it changed */
    uint32_t hint;         /* where to look for a free cluster */
};

static uint64_t be64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int file_read(const xhdd *h, uint64_t off, void *buf, size_t n) {
    return pread(h->fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

#define L2_OFFSET  0x00fffffffffffe00ULL
#define L2_COMPRESSED (1ULL << 62)
#define L2_ZERO    1ULL

static const uint64_t *l2_table(xhdd *h, uint32_t i) {
    if (i >= h->l1n) return NULL;
    const uint64_t at = h->l1[i] & L2_OFFSET;
    if (!at) return NULL;
    if (!h->l2[i]) {
        unsigned char *raw = malloc(h->cs);
        uint64_t *t = malloc(h->cs);
        if (!raw || !t || file_read(h, at, raw, h->cs) != 0) {
            free(raw);
            free(t);
            return NULL;
        }
        for (uint64_t k = 0; k < h->cs / 8; k++) t[k] = be64(raw + k * 8);
        free(raw);
        h->l2[i] = t;
    }
    return h->l2[i];
}

/* A compressed cluster, inflated into h->zbuf: raw deflate, from the entry's
 * offset over its count of 512-byte sectors. */
static int inflate_cluster(xhdd *h, uint64_t e) {
    if (h->zbuf && h->zentry == e) return 0;
    const unsigned xb = 62 - (h->cbits - 8);
    const uint64_t off = e & ((1ULL << xb) - 1), sectors = (e >> xb) & ((1ULL << (62 - xb)) - 1);
    const size_t len = (size_t)((sectors + 1) * 512 - (off & 511));
    if (!h->zbuf) h->zbuf = malloc(h->cs);
    unsigned char *in = malloc(len);
    if (!h->zbuf || !in) {
        free(in);
        return -1;
    }
    const ssize_t got = pread(h->fd, in, len, (off_t)off); /* the last one may run past the end */
    z_stream z;
    memset(&z, 0, sizeof z);
    int rc = got > 0 && inflateInit2(&z, -15) == Z_OK ? 0 : -1;
    if (rc == 0) {
        z.next_in = in;
        z.avail_in = (uInt)got;
        z.next_out = h->zbuf;
        z.avail_out = (uInt)h->cs;
        const int r = inflate(&z, Z_FINISH);
        rc = (r == Z_STREAM_END || r == Z_BUF_ERROR) && z.avail_out == 0 ? 0 : -1;
        inflateEnd(&z);
    }
    free(in);
    h->zentry = rc == 0 ? e : 0;
    return rc;
}

/* Reads the disk as the Xbox sees it. Clusters never written read as zeros. */
static int disk_read(xhdd *h, uint64_t off, void *buf, size_t n) {
    if (!h->qcow) return file_read(h, off, buf, n);
    unsigned char *o = buf;
    while (n) {
        const uint64_t within = off & (h->cs - 1);
        size_t take = (size_t)(h->cs - within);
        if (take > n) take = n;
        const uint64_t cl = off >> h->cbits, per = h->cs / 8;
        if ((cl / per) >> 32) return -1;
        const uint64_t *t = l2_table(h, (uint32_t)(cl / per));
        const uint64_t e = t ? t[cl % per] : 0;
        if (e & L2_COMPRESSED) { /* qemu-img convert -c, as blank disks come */
            if (inflate_cluster(h, e) != 0) return -1;
            memcpy(o, h->zbuf + within, take);
        } else if (!(e & L2_OFFSET) || (e & L2_ZERO)) {
            memset(o, 0, take);
        } else if (file_read(h, (e & L2_OFFSET) + within, o, take) != 0) {
            return -1;
        }
        o += take;
        off += take;
        n -= take;
    }
    return 0;
}

static int qcow_open(xhdd *h) {
    unsigned char hd[104];
    if (file_read(h, 0, hd, 72) != 0) return -1;
    if (memcmp(hd, "QFI\xfb", 4) != 0) return 0; /* a raw image */
    const uint32_t version = be32(hd + 4);
    h->qcow = 1;
    h->cbits = be32(hd + 20);
    if ((version != 2 && version != 3) || be64(hd + 8) || be32(hd + 32) || be32(hd + 60) || h->cbits < 9 || h->cbits > 21) {
        LOGW("xbox: the hard disk image uses qcow2 features this can't read");
        return -1;
    }
    if (version == 3) {
        if (file_read(h, 72, hd + 72, 32) != 0) return -1;
        /* Only "dirty" is fine; corrupt, external data, zstd and extended L2 aren't. */
        if (be64(hd + 72) & ~1ULL) {
            LOGW("xbox: the hard disk image uses qcow2 features this can't read");
            return -1;
        }
    }
    h->cs = 1ULL << h->cbits;
    h->refbits = version == 3 ? 1 << be32(hd + 96) : 16;
    h->refs_at = be64(hd + 48);
    h->refs_n = (uint64_t)be32(hd + 56) * h->cs / 8;
    h->l1_at = be64(hd + 40);
    h->l1n = be32(hd + 36);
    if (!h->l1n || h->l1n > 65536) return -1;
    unsigned char *raw = malloc((size_t)h->l1n * 8);
    h->l1 = malloc((size_t)h->l1n * 8);
    h->l2 = calloc(h->l1n, sizeof *h->l2);
    int rc = raw && h->l1 && h->l2 && file_read(h, be64(hd + 40), raw, (size_t)h->l1n * 8) == 0 ? 0 : -1;
    for (uint32_t i = 0; rc == 0 && i < h->l1n; i++) h->l1[i] = be64(raw + i * 8);
    free(raw);
    return rc;
}

static int fatx_open(xhdd *h) {
    unsigned char sb[16];
    if (disk_read(h, E_OFFSET, sb, sizeof sb) != 0 || memcmp(sb, "FATX", 4) != 0) return -1;
    const uint32_t spc = le32(sb + 8);
    if (!spc || spc > 1024 || (spc & (spc - 1))) return -1;
    h->ccs = spc * 512;
    h->nclus = (uint32_t)(E_SIZE / h->ccs);
    h->esz = h->nclus < 0xFFF0 ? 2 : 4;
    h->root = le32(sb + 12);
    h->fat = E_OFFSET + 0x1000;
    const uint64_t fat_size = ((uint64_t)h->nclus * (uint64_t)h->esz + 0xFFF) & ~0xFFFULL;
    h->data = h->fat + fat_size;
    h->fatbuf = malloc(fat_size);
    h->fat_lo = (size_t)fat_size;
    h->hint = 1;
    return h->fatbuf && disk_read(h, h->fat, h->fatbuf, fat_size) == 0 ? 0 : -1;
}

xhdd *xhdd_open(const char *image, int writable) {
    xhdd *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fd = open(image, writable ? O_RDWR : O_RDONLY);
    if (h->fd < 0 || qcow_open(h) != 0 || fatx_open(h) != 0) {
        if (h->fd >= 0) LOGW("xbox: can't read the saves in %s", image);
        xhdd_close(h);
        return NULL;
    }
    return h;
}

void xhdd_close(xhdd *h) {
    if (!h) return;
    if (h->fd >= 0) close(h->fd);
    for (uint32_t i = 0; h->l2 && i < h->l1n; i++) free(h->l2[i]);
    free(h->l2);
    free(h->l1);
    free(h->fatbuf);
    free(h->zbuf);
    free(h);
}

/* ---- FATX ---- */

static uint32_t fat_next(const xhdd *h, uint32_t c) {
    if (h->esz == 2) {
        const uint32_t v = (uint32_t)h->fatbuf[c * 2] | (uint32_t)h->fatbuf[c * 2 + 1] << 8;
        return v >= 0xFFF0 ? 0 : v;
    }
    const uint32_t v = le32(h->fatbuf + (size_t)c * 4);
    return v >= 0xFFFFFFF0u ? 0 : v;
}

static int cluster_ok(const xhdd *h, uint32_t c) { return c >= 1 && c < h->nclus; }

static uint64_t cluster_at(const xhdd *h, uint32_t c) { return h->data + (uint64_t)(c - 1) * h->ccs; }

/* A chain's clusters, at most max of them; -1 on a broken or looping chain. */
static int chain(const xhdd *h, uint32_t first, uint32_t *out, int max) {
    int n = 0;
    for (uint32_t c = first; c; c = fat_next(h, c)) {
        if (!cluster_ok(h, c) || n == max) return -1;
        out[n++] = c;
    }
    return n;
}

/* A whole directory's entries, malloc'd; *n is their count. */
static unsigned char *read_dir(xhdd *h, uint32_t first, int *n) {
    uint32_t cl[256];
    const int nc = chain(h, first, cl, 256);
    if (nc < 1) return NULL;
    unsigned char *d = malloc((size_t)nc * h->ccs);
    for (int i = 0; d && i < nc; i++)
        if (disk_read(h, cluster_at(h, cl[i]), d + (size_t)i * h->ccs, h->ccs) != 0) {
            free(d);
            return NULL;
        }
    *n = (int)((size_t)nc * h->ccs / DIRENT);
    return d;
}

/* An entry's name, "" when it's free or deleted; 1 past the last one. */
static int entry_name(const unsigned char *e, char out[NAME_MAX_FATX + 1]) {
    out[0] = 0;
    if (e[0] == 0x00 || e[0] == 0xFF) return 1;
    if (e[0] == 0xE5 || e[0] > NAME_MAX_FATX) return 0;
    memcpy(out, e + 2, e[0]);
    out[e[0]] = 0;
    return 0;
}

/* The entry named name in the directory, as an index into *dir. */
static int find_entry(xhdd *h, uint32_t first, const char *name, unsigned char **dir) {
    int n = 0;
    *dir = read_dir(h, first, &n);
    for (int i = 0; *dir && i < n; i++) {
        char nm[NAME_MAX_FATX + 1];
        if (entry_name(*dir + i * DIRENT, nm)) break;
        if (nm[0] && !strcasecmp(nm, name)) return i;
    }
    return -1;
}

/* FATX times count from 2000: the date in the high half, the time in the low. */
static time_t fatx_time(uint32_t v) {
    const unsigned d = v >> 16, t = v & 0xFFFF;
    int y = 2000 + (int)(d >> 9), m = (int)(d >> 5 & 15), day = (int)(d & 31);
    if (m < 1 || m > 12 || day < 1) return 0;
    /* days from 1970-01-01 to the date (Howard Hinnant's days_from_civil) */
    y -= m <= 2;
    const int era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long days = (long)era * 146097 + (long)yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return (time_t)(days * 86400 + (long)(t >> 11) * 3600 + (long)(t >> 5 & 63) * 60 + (long)(t & 31) * 2);
}

static int copy_out(xhdd *h, uint32_t first, const char *to, int depth) {
    int n = 0, rc = 0;
    unsigned char *d = read_dir(h, first, &n);
    if (!d || mkdir_p(to) != 0) {
        free(d);
        return -1;
    }
    for (int i = 0; i < n && rc == 0; i++) {
        const unsigned char *e = d + i * DIRENT;
        char nm[NAME_MAX_FATX + 1], path[PATH_MAX_LEN];
        if (entry_name(e, nm)) break;
        if (!nm[0] || strchr(nm, '/') || !strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        path_join(path, sizeof path, to, nm);
        const uint32_t start = le32(e + 44), size = le32(e + 48);
        if (e[1] & ATTR_DIR) {
            rc = depth < 8 ? copy_out(h, start, path, depth + 1) : -1;
            continue;
        }
        FILE *f = fopen(path, "wb");
        unsigned char *buf = malloc(h->ccs);
        uint32_t left = size;
        for (uint32_t c = start; f && buf && left && rc == 0; c = fat_next(h, c)) {
            const size_t take = left < h->ccs ? left : h->ccs;
            if (!cluster_ok(h, c) || disk_read(h, cluster_at(h, c), buf, take) != 0 || fwrite(buf, 1, take, f) != take) rc = -1;
            left -= (uint32_t)take;
        }
        if (!f || !buf || left) rc = -1;
        if (f) fclose(f);
        free(buf);
        if (rc == 0) set_file_mtime(path, fatx_time(le32(e + 56)));
    }
    free(d);
    return rc;
}

int xhdd_get_save(xhdd *h, const char *id, const char *dir) {
    unsigned char *top = NULL, *udata = NULL;
    int rc = -1, at = find_entry(h, h->root, "UDATA", &top);
    if (!top) goto out;
    rc = 0;
    if (at < 0 || !(top[at * DIRENT + 1] & ATTR_DIR)) goto out;
    int g = find_entry(h, le32(top + at * DIRENT + 44), id, &udata);
    if (!udata) {
        rc = -1;
        goto out;
    }
    if (g < 0 || !(udata[g * DIRENT + 1] & ATTR_DIR)) goto out;
    char nm[NAME_MAX_FATX + 1], to[PATH_MAX_LEN];
    entry_name(udata + g * DIRENT, nm);
    path_join(to, sizeof to, dir, nm);
    rc = copy_out(h, le32(udata + g * DIRENT + 44), to, 0) == 0 ? 1 : -1;
    if (rc < 0) LOGW("xbox: the save %s on the hard disk is damaged", nm);
out:
    free(top);
    free(udata);
    return rc;
}

/* ---- Writing ----
 * Only ever into a copy of the image (xhdd_put_save_file), renamed over the
 * original once everything is in, so a restore that fails halfway leaves
 * the disk as it was. */

static void put_be64(unsigned char *p, uint64_t v) {
    for (int i = 7; i >= 0; i--, v >>= 8) p[i] = (unsigned char)v;
}
static void put_le32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++, v >>= 8) p[i] = (unsigned char)v;
}

static int file_write(const xhdd *h, uint64_t off, const void *buf, size_t n) {
    return pwrite(h->fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

#define L2_COPIED (1ULL << 63)

/* Adds delta to the references to the cluster at off. The refcount block for
 * it is made when there's none yet, as long as it can count itself (it lands
 * at the file's end, which is past every 2 GiB a 64 KiB block covers but the last). */
static int ref_add(xhdd *h, uint64_t off, int delta) {
    if (h->refbits != 16) return -1;
    const uint64_t per = h->cs / 2, cl = off >> h->cbits, idx = cl / per;
    unsigned char e[8];
    if (idx >= h->refs_n || file_read(h, h->refs_at + idx * 8, e, 8) != 0) return -1;
    uint64_t block = be64(e) & ~(h->cs - 1);
    if (!block) {
        if (delta < 0) return -1;
        block = h->eof;
        if ((block >> h->cbits) / per != idx) return -1;
        unsigned char *z = calloc(1, h->cs);
        if (!z) return -1;
        const uint64_t self = (block >> h->cbits) % per;
        z[self * 2 + 1] = 1;
        int rc = file_write(h, block, z, h->cs);
        free(z);
        put_be64(e, block);
        if (rc != 0 || file_write(h, h->refs_at + idx * 8, e, 8) != 0) return -1;
        h->eof += h->cs;
    }
    unsigned char c[2];
    const uint64_t at = block + (cl % per) * 2;
    if (file_read(h, at, c, 2) != 0) return -1;
    const int v = c[0] << 8 | c[1];
    if (v + delta < 0 || v + delta > 0xFFFF) return -1;
    c[0] = (unsigned char)((v + delta) >> 8);
    c[1] = (unsigned char)(v + delta);
    return file_write(h, at, c, 2);
}

/* A new cluster at the end of the file, zeroed and counted. */
static int new_cluster(xhdd *h, uint64_t *out) {
    unsigned char *z = calloc(1, h->cs);
    const uint64_t at = h->eof;
    int rc = z ? file_write(h, at, z, h->cs) : -1;
    free(z);
    if (rc != 0) return -1;
    h->eof += h->cs;
    if (ref_add(h, at, 1) != 0) return -1;
    *out = at;
    return 0;
}

/* Where the guest cluster lives in the file, giving it a place first when
 * it has none (or is only marked as zeros). */
static int host_cluster(xhdd *h, uint64_t cl, uint64_t *out) {
    const uint64_t per = h->cs / 8;
    const uint32_t i = (uint32_t)(cl / per);
    if (i >= h->l1n) return -1;
    if (!(h->l1[i] & L2_OFFSET)) {
        uint64_t t;
        unsigned char e[8];
        if (new_cluster(h, &t) != 0) return -1;
        h->l1[i] = t | L2_COPIED;
        put_be64(e, h->l1[i]);
        if (file_write(h, h->l1_at + (uint64_t)i * 8, e, 8) != 0) return -1;
    }
    uint64_t *t = (uint64_t *)l2_table(h, i);
    if (!t) return -1;
    uint64_t v = t[cl % per];
    if (v & L2_COMPRESSED) {
        /* Out to a cluster of its own; the compressed data's clusters lose a reference. */
        const unsigned xb = 62 - (h->cbits - 8);
        const uint64_t off = v & ((1ULL << xb) - 1), end = (off & ~511ULL) + (((v >> xb) & ((1ULL << (62 - xb)) - 1)) + 1) * 512;
        uint64_t at;
        if (inflate_cluster(h, v) != 0 || new_cluster(h, &at) != 0 || file_write(h, at, h->zbuf, h->cs) != 0) return -1;
        for (uint64_t c = off >> h->cbits; c <= (end - 1) >> h->cbits; c++)
            if (ref_add(h, c << h->cbits, -1) != 0) return -1;
        unsigned char e[8];
        t[cl % per] = at | L2_COPIED;
        put_be64(e, t[cl % per]);
        if (file_write(h, (h->l1[i] & L2_OFFSET) + (cl % per) * 8, e, 8) != 0) return -1;
        h->zentry = 0;
        *out = at;
        return 0;
    }
    if ((v & L2_OFFSET) && !(v & L2_ZERO)) {
        if (!(v & L2_COPIED)) return -1; /* shared with a snapshot: not ours to change */
        *out = v & L2_OFFSET;
        return 0;
    }
    uint64_t at = v & L2_OFFSET;
    if (at) { /* preallocated, reading as zeros until now */
        unsigned char *z = calloc(1, h->cs);
        int rc = z ? file_write(h, at, z, h->cs) : -1;
        free(z);
        if (rc != 0) return -1;
    } else if (new_cluster(h, &at) != 0) {
        return -1;
    }
    unsigned char e[8];
    t[cl % per] = at | L2_COPIED;
    put_be64(e, t[cl % per]);
    if (file_write(h, (h->l1[i] & L2_OFFSET) + (cl % per) * 8, e, 8) != 0) return -1;
    *out = at;
    return 0;
}

static int disk_write(xhdd *h, uint64_t off, const void *buf, size_t n) {
    if (!h->qcow) return file_write(h, off, buf, n);
    const unsigned char *p = buf;
    while (n) {
        const uint64_t within = off & (h->cs - 1);
        size_t take = (size_t)(h->cs - within);
        if (take > n) take = n;
        uint64_t at;
        if (host_cluster(h, off >> h->cbits, &at) != 0 || file_write(h, at + within, p, take) != 0) return -1;
        p += take;
        off += take;
        n -= take;
    }
    return 0;
}

static void fat_set(xhdd *h, uint32_t c, uint32_t v) {
    const size_t at = (size_t)c * (size_t)h->esz;
    if (h->esz == 2) {
        h->fatbuf[at] = (unsigned char)v;
        h->fatbuf[at + 1] = (unsigned char)(v >> 8);
    } else {
        put_le32(h->fatbuf + at, v);
    }
    if (at < h->fat_lo) h->fat_lo = at;
    if (at + (size_t)h->esz > h->fat_hi) h->fat_hi = at + (size_t)h->esz;
}

static uint32_t fat_raw(const xhdd *h, uint32_t c) {
    return h->esz == 2 ? (uint32_t)h->fatbuf[c * 2] | (uint32_t)h->fatbuf[c * 2 + 1] << 8 : le32(h->fatbuf + (size_t)c * 4);
}

static void free_chain(xhdd *h, uint32_t first) {
    for (uint32_t c = first, n = 0; cluster_ok(h, c) && n < h->nclus; n++) {
        const uint32_t next = fat_next(h, c);
        fat_set(h, c, 0);
        if (c < h->hint) h->hint = c;
        c = next;
    }
}

/* A chain of count free clusters, linked and ended; 0 when the disk is full. */
static uint32_t alloc_chain(xhdd *h, uint32_t count) {
    uint32_t first = 0, prev = 0;
    for (uint32_t c = h->hint; count && c < h->nclus; c++) {
        if (fat_raw(h, c) != 0) continue;
        fat_set(h, c, h->esz == 2 ? 0xFFFF : 0xFFFFFFFFu);
        if (prev) fat_set(h, prev, c);
        else first = c;
        prev = c;
        count--;
        h->hint = c + 1;
    }
    if (count) {
        if (first) free_chain(h, first);
        return 0;
    }
    return first;
}

static uint32_t fatx_now(time_t t) {
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year < 100) return 0x00210000; /* 2000-01-01 */
    return (uint32_t)((tm.tm_year - 100) << 25 | (tm.tm_mon + 1) << 21 | tm.tm_mday << 16 | tm.tm_hour << 11 | tm.tm_min << 5 |
                      tm.tm_sec / 2);
}

static void make_entry(unsigned char *e, const char *name, int dir, uint32_t first, uint32_t size, time_t t) {
    memset(e, 0xFF, DIRENT);
    e[0] = (unsigned char)strlen(name);
    e[1] = dir ? ATTR_DIR : 0;
    memcpy(e + 2, name, strlen(name));
    put_le32(e + 44, first);
    put_le32(e + 48, size);
    const uint32_t ft = fatx_now(t);
    put_le32(e + 52, ft);
    put_le32(e + 56, ft);
    put_le32(e + 60, ft);
}

static int valid_name(const char *name) {
    const size_t n = strlen(name);
    return n && n <= NAME_MAX_FATX && !strpbrk(name, "<>:\"/\\|*?");
}

/* A file's data into a new chain. */
static int put_file(xhdd *h, const char *path, uint32_t *first, uint32_t *size) {
    const int64_t len = file_size(path);
    if (len < 0 || len > 0xFFFFFFFFLL) return -1;
    *size = (uint32_t)len;
    *first = 0;
    if (!len) return 0;
    *first = alloc_chain(h, (uint32_t)((len + h->ccs - 1) / h->ccs));
    if (!*first) return -1;
    FILE *f = fopen(path, "rb");
    unsigned char *buf = malloc(h->ccs);
    int rc = f && buf ? 0 : -1;
    for (uint32_t c = *first; rc == 0 && c; c = fat_next(h, c)) {
        memset(buf, 0, h->ccs);
        if (fread(buf, 1, h->ccs, f) == 0 || disk_write(h, cluster_at(h, c), buf, h->ccs) != 0) rc = -1;
    }
    if (f) fclose(f);
    free(buf);
    return rc;
}

/* A folder from the host, its tree written into new chains. */
static int put_tree(xhdd *h, const char *from, uint32_t *first, int depth) {
    DIR *d = opendir(from);
    if (!d || depth > 8) {
        if (d) closedir(d);
        return -1;
    }
    struct dirent *de;
    int n = 0, cap = 0, rc = 0;
    unsigned char *ents = NULL;
    while (rc == 0 && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[PATH_MAX_LEN];
        path_join(path, sizeof path, from, de->d_name);
        if (!valid_name(de->d_name)) {
            LOGW("xbox: %s can't go on the hard disk, its name doesn't fit", de->d_name);
            rc = -1;
            break;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            unsigned char *more = realloc(ents, (size_t)cap * DIRENT);
            if (!more) {
                rc = -1;
                break;
            }
            ents = more;
        }
        const int is_dir = dir_exists(path);
        uint32_t c = 0, size = 0;
        rc = is_dir ? put_tree(h, path, &c, depth + 1) : put_file(h, path, &c, &size);
        if (rc == 0) make_entry(ents + (size_t)n++ * DIRENT, de->d_name, is_dir, c, size, file_mtime(path));
    }
    closedir(d);
    /* A directory has at least one cluster; what's past its entries reads as the end. */
    const size_t bytes = (size_t)n * DIRENT, nc = bytes / h->ccs + 1;
    unsigned char *buf = rc == 0 ? malloc(nc * h->ccs) : NULL;
    *first = buf ? alloc_chain(h, (uint32_t)nc) : 0;
    if (!*first) rc = -1;
    if (rc == 0) {
        memset(buf, 0xFF, nc * h->ccs);
        if (bytes) memcpy(buf, ents, bytes);
        size_t i = 0;
        for (uint32_t c = *first; rc == 0 && c; c = fat_next(h, c), i++)
            if (disk_write(h, cluster_at(h, c), buf + i * h->ccs, h->ccs) != 0) rc = -1;
    }
    free(buf);
    free(ents);
    return rc;
}

/* Frees what the entry holds: its chain, and a folder's tree. */
static void free_entry(xhdd *h, const unsigned char *e, int depth) {
    const uint32_t first = le32(e + 44);
    if ((e[1] & ATTR_DIR) && depth < 8) {
        int n = 0;
        unsigned char *d = read_dir(h, first, &n);
        for (int i = 0; d && i < n; i++) {
            char nm[NAME_MAX_FATX + 1];
            if (entry_name(d + i * DIRENT, nm)) break;
            if (nm[0]) free_entry(h, d + i * DIRENT, depth + 1);
        }
        free(d);
    }
    free_chain(h, first);
}

/* Writes a directory's entries back after a change. */
static int write_dir(xhdd *h, uint32_t first, const unsigned char *d) {
    uint32_t cl[256];
    const int nc = chain(h, first, cl, 256);
    if (nc < 1) return -1;
    for (int i = 0; i < nc; i++)
        if (disk_write(h, cluster_at(h, cl[i]), d + (size_t)i * h->ccs, h->ccs) != 0) return -1;
    return 0;
}

/* Puts the entry e in the directory: a deleted slot, the end, or a new cluster. */
static int add_entry(xhdd *h, uint32_t first, const unsigned char *e) {
    int n = 0;
    unsigned char *d = read_dir(h, first, &n);
    if (!d) return -1;
    int at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (d[i * DIRENT] == 0xE5 || d[i * DIRENT] == 0x00 || d[i * DIRENT] == 0xFF) at = i;
    if (at < 0) { /* full: one more cluster on its chain */
        uint32_t last = first, c;
        while ((c = fat_next(h, last))) last = c;
        const uint32_t more = alloc_chain(h, 1);
        unsigned char *bigger = more ? realloc(d, (size_t)n * DIRENT + h->ccs) : NULL;
        if (!bigger) {
            free(d);
            return -1;
        }
        d = bigger;
        fat_set(h, last, more);
        memset(d + (size_t)n * DIRENT, 0xFF, h->ccs);
        at = n;
        n += (int)(h->ccs / DIRENT);
    }
    memcpy(d + (size_t)at * DIRENT, e, DIRENT);
    const int rc = write_dir(h, first, d);
    free(d);
    return rc;
}

static int fat_flush(xhdd *h) {
    if (h->fat_hi <= h->fat_lo) return 0;
    return disk_write(h, h->fat + h->fat_lo, h->fatbuf + h->fat_lo, h->fat_hi - h->fat_lo);
}

/* E:\UDATA\<id> becomes the folder from: the old one's clusters are freed. */
static int put_save(xhdd *h, const char *id, const char *from) {
    unsigned char *top = NULL, *udata = NULL, e[DIRENT];
    int rc = -1, at = find_entry(h, h->root, "UDATA", &top);
    uint32_t ud = 0, save = 0;
    if (!top) goto out;
    if (at >= 0 && (top[at * DIRENT + 1] & ATTR_DIR)) {
        ud = le32(top + at * DIRENT + 44);
    } else { /* a disk no game has saved on yet */
        ud = alloc_chain(h, 1);
        unsigned char *blank = ud ? malloc(h->ccs) : NULL;
        if (!blank) goto out;
        memset(blank, 0xFF, h->ccs);
        const int w = disk_write(h, cluster_at(h, ud), blank, h->ccs);
        free(blank);
        make_entry(e, "UDATA", 1, ud, 0, time(NULL));
        if (w != 0 || add_entry(h, h->root, e) != 0) goto out;
    }
    /* Named as the zip has it, so its hash stays RomM's (FATX doesn't mind
     * the case), else as it was, else as xemu names one. */
    char name[NAME_MAX_FATX + 1];
    snprintf(name, sizeof name, "%s", id);
    for (char *p = name; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    int g = find_entry(h, ud, id, &udata);
    if (!udata) goto out;
    if (g >= 0) {
        unsigned char *old = udata + g * DIRENT;
        entry_name(old, name);
        free_entry(h, old, 0);
        old[0] = 0xE5;
        if (write_dir(h, ud, udata) != 0) goto out;
    }
    if (!strcasecmp(path_basename(from), id)) snprintf(name, sizeof name, "%s", path_basename(from));
    if (put_tree(h, from, &save, 0) != 0) goto out;
    make_entry(e, name, 1, save, 0, file_mtime(from));
    if (add_entry(h, ud, e) != 0 || fat_flush(h) != 0) goto out;
    rc = 0;
out:
    free(top);
    free(udata);
    return rc;
}

int xhdd_put_save_file(const char *image, const char *id, const char *from) {
    char tmp[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.romm", image);
    unlink(tmp);
    if (copy_file(image, tmp) != 0) {
        LOGE("xbox: no room to copy the hard disk image for a restore");
        unlink(tmp);
        return -1;
    }
    xhdd *h = xhdd_open(tmp, 1);
    int rc = -1;
    if (h) {
        struct stat st;
        h->eof = fstat(h->fd, &st) == 0 ? ((uint64_t)st.st_size + h->cs - 1) & ~(h->cs - 1) : 0;
        rc = h->eof || !h->qcow ? put_save(h, id, from) : -1;
        if (rc == 0 && fsync(h->fd) != 0) rc = -1;
        xhdd_close(h);
    }
    if (rc == 0 && rename(tmp, image) != 0) rc = -1;
    if (rc != 0) {
        LOGE("xbox: couldn't put the save %s on the hard disk; it's as it was", id);
        unlink(tmp);
    }
    return rc;
}
