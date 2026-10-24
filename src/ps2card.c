#include "ps2card.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sigil.h"
#include "card.h" /* third_party/sigil/src: sigil_card_sony_owner */

/* sigil's save root is the folder holding memcards/; the cards here may sit in
 * the emulator's folder itself (PS5SX2 without a memcards/ folder), so every
 * path sigil names is mapped onto ps2_cards.dir. */
#define ROOT "memcards/"

/* sigil.c resolves every platform slug, and with it links every disc reader;
 * only the sync code is taken here, and it only asks for "ps2".
 * shortcut: knows "ps2" only, take sigil.c if another platform syncs through sigil. */
sigil_platform sigil_platform_from_slug(const char *slug) {
    return slug && !strcmp(slug, "ps2") ? SIGIL_PLATFORM_PS2 : SIGIL_PLATFORM_AUTO;
}

typedef struct {
    char **items;
    size_t n, cap;
} str_list;

static int list_add(str_list *l, const char *s) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        char **grown = realloc(l->items, cap * sizeof *grown);
        if (!grown) return -1;
        l->items = grown;
        l->cap = cap;
    }
    if (!(l->items[l->n] = strdup(s))) return -1;
    l->n++;
    return 0;
}

static void list_free(str_list *l) {
    for (size_t i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
}

/* Every file below dir, as "<rel>/<name>". */
static int list_tree(str_list *l, const char *dir, const char *rel, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_MAX_LEN], name[PATH_MAX_LEN];
        path_join(path, sizeof path, dir, de->d_name);
        snprintf(name, sizeof name, "%s/%s", rel, de->d_name);
        if (dir_exists(path)) rc = depth < 4 ? list_tree(l, path, name, depth + 1) : 0;
        else rc = list_add(l, name);
    }
    closedir(d);
    return rc;
}

static int list_cards(const ps2_cards *c, str_list *l) {
    for (int s = 0; s < 2; s++) {
        const char *card = c->slot[s];
        if (!card[0] || strchr(card, '/') || (s == 1 && !strcmp(card, c->slot[0]))) continue;
        char path[PATH_MAX_LEN], rel[PATH_MAX_LEN];
        path_join(path, sizeof path, c->dir, card);
        snprintf(rel, sizeof rel, ROOT "%s", card);
        size_t before = l->n;
        int rc = dir_exists(path) ? list_tree(l, path, rel, 0) : file_exists(path) ? list_add(l, rel) : 0;
        if (rc) return rc;
        /* sigil tells a folder card by the files listed in it: an empty folder
         * (PS5SX2 makes memcards/MemoryCard empty) gets the empty superblock
         * PCSX2 gives a new folder card (FileMcd_CreateNewCard), else the save
         * would go on the other slot's card. */
        if (l->n == before && dir_exists(path)) {
            char sb[PATH_MAX_LEN];
            path_join(sb, sizeof sb, path, "_pcsx2_superblock");
            if (write_file_atomic(sb, "", 0) != 0) continue; /* unwritable: listed as before */
            snprintf(sb, sizeof sb, "%s/_pcsx2_superblock", rel);
            if (list_add(l, sb) != 0) return -1;
        }
    }
    return 0;
}

/* The real path of a save root path sigil names; -1 outside memcards/. */
static int real_path(const ps2_cards *c, const char *rel, char *out, size_t n) {
    if (strncmp(rel, ROOT, sizeof ROOT - 1) != 0 || strstr(rel, "..")) return -1;
    path_join(out, n, c->dir, rel + sizeof ROOT - 1);
    return 0;
}

static sigil_io *open_member(void *ctx, const char *rel) {
    char path[PATH_MAX_LEN];
    return real_path(ctx, rel, path, sizeof path) == 0 ? sigil_io_open_file(path) : NULL;
}

static int write_member(void *ctx, const char *rel, const uint8_t *data, size_t len) {
    char path[PATH_MAX_LEN];
    if (real_path(ctx, rel, path, sizeof path) != 0) return -1;
    mkdir_parent(path);
    return write_file_atomic(path, data, len);
}

/* A path ending in '/' is a save folder sigil emptied. */
static int remove_member(void *ctx, const char *rel) {
    char path[PATH_MAX_LEN];
    if (real_path(ctx, rel, path, sizeof path) != 0) return -1;
    size_t l = strlen(path);
    if (l && path[l - 1] == '/') {
        path[l - 1] = 0;
        return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

typedef struct {
    sigil_sync_request req;
    sigil_result result;
    sigil_save_option options[2];
    const char *ids[1];
    str_list listing;
} request;

static int request_init(request *q, const ps2_cards *c, const char *content_path, const char *serial) {
    memset(q, 0, sizeof *q);
    if (list_cards(c, &q->listing) != 0) return -1;
    q->result.struct_version = SIGIL_RESULT_V4;
    q->result.platform = SIGIL_PLATFORM_PS2;
    q->result.usage = SIGIL_USAGE_FOLDER_PREFIX;
    str_copy(q->result.title_id, sizeof q->result.title_id, serial);
    q->options[0] = (sigil_save_option){"Slot1_Filename", c->slot[0]};
    q->options[1] = (sigil_save_option){"Slot2_Filename", c->slot[1]};
    q->ids[0] = serial;
    sigil_sync_request *r = &q->req;
    r->struct_version = SIGIL_SYNC_REQUEST_V1;
    r->save.struct_version = SIGIL_SAVE_REQUEST_V1;
    r->save.layout = "pcsx2_standalone";
    r->save.platform = "ps2";
    r->save.content_path = content_path;
    r->save.result = &q->result;
    r->save.options = q->options;
    r->save.option_count = c->slot[1][0] ? 2 : 1;
    r->save.listing = (const char *const *)q->listing.items;
    r->save.listing_count = q->listing.n;
    r->save.open = open_member;
    r->save.open_ctx = (void *)c;
    r->game_ids = q->ids;
    r->game_id_count = 1;
    r->mode = SIGIL_SYNC_UNMANAGED;
    /* sync.c settles conflicts and keeps a backup before a download. */
    r->overwrite_local = 1;
    r->write = write_member;
    r->remove = remove_member;
    r->write_ctx = (void *)c;
    return 0;
}

static void log_refusal(const char *what, const char *serial, int rc, const sigil_sync_result *r) {
    LOGW("ps2 %s %s: sigil error %d%s%s", what, serial, rc, r && r->problem[0] ? ", " : "", r ? r->problem : "");
}

int ps2card_collect(const ps2_cards *c, const char *content_path, const char *serial, unsigned char **unit,
                    size_t *len, char hash[33], char artifact[256], char identity[33]) {
    *unit = NULL;
    *len = 0;
    hash[0] = artifact[0] = identity[0] = 0;
    request q;
    sigil_sync_result *r = NULL;
    int rc = request_init(&q, c, content_path, serial) == 0 ? sigil_collect(&q.req, &r) : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK && r->data) {
        *unit = r->data;
        *len = r->len;
        r->data = NULL;
        str_copy(hash, 33, r->content_hash);
        str_copy(artifact, 256, r->artifact);
        str_copy(identity, 33, r->identity_hash);
    } else if (rc != SIGIL_OK && rc != SIGIL_ERR_NOT_FOUND) {
        log_refusal("collect", serial, rc, r);
    }
    sigil_sync_result_free(r);
    list_free(&q.listing);
    return rc == SIGIL_OK || rc == SIGIL_ERR_NOT_FOUND ? 0 : -1;
}

int ps2card_restore(const ps2_cards *c, const char *content_path, const char *serial, const unsigned char *unit,
                    size_t len) {
    request q;
    sigil_sync_result *r = NULL;
    int rc = request_init(&q, c, content_path, serial) == 0 ? sigil_restore(&q.req, unit, len, &r) : SIGIL_ERR_OOM;
    if (rc != SIGIL_OK) log_refusal("restore", serial, rc, r);
    sigil_sync_result_free(r);
    list_free(&q.listing);
    return rc == SIGIL_OK ? 0 : -1;
}

int ps2card_folder_is_game(const char *name, const char *serial) {
    char owner[SIGIL_CARD_OWNER_MAX];
    sigil_card_sony_owner(name, owner);
    return owner[0] && !strcmp(owner, serial);
}

int ps2card_file_has(const char *path, const char *serial) {
    sigil_io *io = sigil_io_open_file(path);
    sigil_card_listing *l = NULL;
    int has = 0;
    if (io && sigil_card_list(io, &l) == SIGIL_OK)
        for (size_t i = 0; i < l->entry_count && !has; i++) has = !strcmp(l->entries[i].owner_id, serial);
    sigil_card_listing_free(l);
    if (io) sigil_io_close(io);
    return has;
}
