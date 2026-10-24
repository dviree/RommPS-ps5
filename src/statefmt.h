/* Save state files: the containers emulators keep them in, and the names
 * RomM clients give the emulator that made them.
 *
 * RetroArch (and RomM's web player) wrap a core's state:
 *   "RASTATE\x01", then blocks of a 4-byte tag, a little-endian u32 size and
 *   the data padded to 8 bytes: "MEM " (the core's state) ... "END ".
 * and RetroArch compresses that by default:
 *   "#RZIPv\x01#", u32 chunk size, u64 total size, then chunks of a u32
 *   length and a zlib stream.
 * Standalone emulators built on a libretro core (PSXS5) keep the core's
 * state bare, which is what "MEM " holds. */
#ifndef ROMM_STATEFMT_H
#define ROMM_STATEFMT_H

#include <stddef.h>

typedef enum { STATE_RAW, STATE_RASTATE, STATE_RZIP } state_kind;

state_kind state_kind_of(const unsigned char *b, size_t n);

/* The core's own state, out of whatever container it's in. Returns malloc'd
 * bytes, or NULL when the container is broken. */
unsigned char *state_to_raw(const unsigned char *b, size_t n, size_t *out_n);

/* An uncompressed RetroArch state: what RomM's web player saves and loads.
 * A bare state is wrapped, a compressed one unpacked. Malloc'd, or NULL. */
unsigned char *state_to_rastate(const unsigned char *b, size_t n, size_t *out_n);

/* Unpacks a compressed (RZIP) state file into out_path, chunk by chunk, so a
 * big state never has to fit in memory whole (the PS4 payload has little).
 * 0 when done; -1 if in_path isn't a whole RZIP state or out_path can't be
 * written (nothing is left at out_path then). */
int state_unrzip_file(const char *in_path, const char *out_path);
/* Wraps a bare core state file in RetroArch's container ("MEM " then "END "),
 * streamed the same way. 0 when done, -1 otherwise (nothing left behind). */
int state_wrap_file(const char *in_path, const char *out_path);
/* The core's own state out of a state file, whatever its container
 * (state_to_raw, streamed). 0 when done, -1 otherwise (nothing left behind). */
int state_unwrap_file(const char *in_path, const char *out_path);
/* The libretro core a bare (or wrapped) state comes from, by its signature:
 * "pcsx_rearmed", "mednafen_psx_hw", "swanstation", or NULL. */
const char *state_core_of(const unsigned char *b, size_t n);

/* RomM's emulator names, made comparable: lower case, "-" as "_", without
 * the "retroarch-"/"libretro-" prefixes and "_libretro..." suffixes other
 * clients add ("mGBA", "retroarch-mgba", "mgba_libretro_ios" -> "mgba"). */
void emulator_key(const char *name, char *out, size_t n);

#endif
