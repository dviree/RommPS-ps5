/* For tests/gameid_test.sh: a disc's PlayStation serial and PS2 CRC.
 *   gameid_cli <disc>   prints "<serial> <crc>", "-" for what it can't read */
#include <stdio.h>

#include "cJSON.h"
#include "gameid.h"

/* state.c's, for gameid_from_library, which this doesn't call. */
const char *jget_str(const cJSON *o, const char *k, const char *def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char serial[32];
    uint32_t crc;
    int s = gameid_psx(argv[1], serial, sizeof serial), c = gameid_ps2_crc(argv[1], &crc);
    printf("%s ", s == 0 ? serial : "-");
    if (c == 0) printf("%08X\n", crc);
    else printf("-\n");
    return 0;
}
