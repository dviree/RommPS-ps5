/* Shared by the PS5's platform.c and mounts.c. */
#ifndef ROMM_PS5_H
#define ROMM_PS5_H

#include <stdint.h>
#include <sys/types.h>

/* The title id ("PPSA99248", into 16 bytes) and app id of the app a process
 * belongs to. 0 on success, -1 for a process that isn't an app's. */
int ps5_proc_app(pid_t pid, char title[16], uint32_t *app_id);

#endif
