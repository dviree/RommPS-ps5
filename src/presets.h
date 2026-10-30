#ifndef ROMM_PRESETS_H
#define ROMM_PRESETS_H

#include "cJSON.h"

#define PRESETS_FORMAT 1

/* Pick up the latest valid presets. Called at startup. */
void presets_init(void);

/* The catalog currently in use. Newer file takes its place and the old one stays valid in case it's still being used. */
const cJSON *presets_catalog(void);

/* Looks for newer presets in the background, and downloads, verifies, and applies them.*/
int presets_check(void);

/* Tells preset version, source (built-in) or (downloaded), info about the state, errors, and last checked time */
cJSON *presets_status(void);

/* Compare the version code within date to see if this contains newer presets. */
int presets_version_cmp(const char *a, const char *b);

#endif