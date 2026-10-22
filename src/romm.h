/* RomM API client. Needs RomM 5.2.0 or newer. */
#ifndef ROMM_API_H
#define ROMM_API_H

#include <stdint.h>

#include "cJSON.h"
#include "http.h"

/* For GET /api/roms: by default every page also carries the ids of every
 * matching game and the platform's filter values, which nothing here uses and
 * which grow with the library. */
#define ROMS_LEAN "&with_rom_id_index=false&with_filter_values=false"

/* Scopes requested when pairing. */
#define ROMM_SCOPES                                                                           \
    "me.read", "platforms.read", "roms.read", "roms.user.read", "firmware.read", "assets.read", \
        "assets.write", "devices.read", "devices.write"

typedef struct {
    char device_code[128];
    char user_code[32];
    char verification_url[640];
    int interval;
    int expires_in;
} romm_device_flow;

typedef enum { POLL_OK, POLL_PENDING, POLL_SLOW_DOWN, POLL_DENIED, POLL_EXPIRED, POLL_ERROR } romm_poll;

/* JSON request to the paired server. Returns the parsed body (free it) or NULL. */
cJSON *romm_call(const char *method, const char *path, const cJSON *body, long *status);

/* Tells RomM the console's name when it changed (or was never sent): the
 * console was renamed, or its name set in the web UI. One request, only then. */
void romm_update_device_name(void);

/* GETs a paged RomM list such as /api/roms?..., limit items from offset, 60
 * at a time (the library's page), 16 on the PS4. RomM's game lists carry each
 * game's full metadata, so a page of 30 can be too much to parse in the PS4's
 * memory; a page that can't be taken in is asked for again, fewer games at a
 * time. cb gets each item with its page (first is set for the first page, and
 * cb is called once with a NULL item for an empty list); nonzero stops.
 * Returns how many items were seen, -1 on an error (status holds the HTTP
 * status). */
typedef int (*romm_list_cb)(const cJSON *item, const cJSON *page, int first, void *ctx);
int romm_list(const char *path, int offset, int limit, romm_list_cb cb, void *ctx, long *status);
/* Same, with an explicit server and auth, for use before pairing. */
cJSON *romm_call_raw(const char *base, const char *auth, const char *method, const char *path,
                     const cJSON *body, long *status);

/* 1 while the paired server isn't answering at all (see http_down), with what
 * to tell the user in msg: "RomM at 192.168.1.2:8085 isn't answering". */
int romm_no_answer(char *msg, int n);

/* While it isn't answering, finds out in the background when it does again
 * (one check at a time, paced as http_down says), so nothing waits for the
 * next sync to notice. */
void romm_check_back(void);

/* Checks the server and fills in its version. 0 on success. */
int romm_heartbeat(const char *base, char *version, int vn);

/* Device pairing: the user approves a code in RomM. */
int romm_device_init(const char *base, romm_device_flow *flow);
romm_poll romm_device_poll(const char *base, const char *device_code, char *token, int tn,
                           char *device_id, int dn);
/* Pairing with a code from RomM's Client API Tokens page. */
int romm_pair_exchange(const char *base, const char *code, char *token, int tn);
int romm_register_device(const char *base, const char *token, char *device_id, int dn);

/* Downloads a server path to dest, resuming if possible. */
int romm_download(const char *path, const char *dest, http_progress_fn cb, void *ud,
                  http_resp *resp);
/* Uploads one file (POST or PUT). Returns the parsed response. */
cJSON *romm_upload(const char *method, const char *path, const char *field, const char *file,
                   const char *upload_name, long *status);

/* The "detail" message from a RomM error response. */
void romm_error_detail(const cJSON *j, char *out, int n);

#endif
