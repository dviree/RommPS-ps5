#!/usr/bin/env bash
# Disc ids from test images: PS1 serials, PS2 serials and PCSX2's CRC (make unit).
set -euo pipefail
CLI=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "gameid: $*" >&2; exit 1; }
expect() { local got; got=$("$CLI" "$1"); [[ "$got" == "$2" ]] || fail "$(basename "$1"): got '$got', want '$2'"; }

python3 "$ROOT/tests/make_disc.py" psx "$T/ps1.iso" SLUS_123.45
expect "$T/ps1.iso" "SLUS-12345 -"
python3 "$ROOT/tests/make_disc.py" psxbin "$T/ps1.bin" SCUS_941.94
expect "$T/ps1.bin" "SCUS-94194 -"
crc=$(python3 "$ROOT/tests/make_disc.py" ps2 "$T/ps2.iso" SLUS_205.52)
expect "$T/ps2.iso" "SLUS-20552 $crc"
crc=$(python3 "$ROOT/tests/make_disc.py" ps2 "$T/sub.iso" SLES_501.23 DATA)
expect "$T/sub.iso" "SLES-50123 $crc"
head -c 40000 /dev/urandom > "$T/junk.iso"
expect "$T/junk.iso" "- -"
echo "gameid: ok"
