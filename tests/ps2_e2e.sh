#!/usr/bin/env bash
# PS5SX2's memory card saves one game at a time (save_layout "ps2", src/ps2card.c),
# against tests/mock_romm.py: a game's saves come down onto its folder card,
# go up as sigil's one-game card, and other games' saves on the card stay.
#   make host && tests/ps2_e2e.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
MOCK_PORT=8897 WEB_PORT=8782
API="http://127.0.0.1:${WEB_PORT}" MOCK="http://127.0.0.1:${MOCK_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; [[ -n "${KEEP_T:-}" ]] || rm -rf "$T"' EXIT

fail() { echo "FAIL: $*"; echo "--- daemon log"; tail -60 "$T/daemon.log"; exit 1; }
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
count() { curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']; print(s['sync_count'] if not s['running'] else -1)"; }
wait_sync() { for _ in $(seq 1 60); do [[ "$(count)" -ge "$1" ]] && return 0; sleep 0.5; done; fail "sync #$1 did not finish"; }
syncnow() { local n; n=$(count); post /api/sync >/dev/null; wait_sync $((n + 1)); }
last() { curl -sf "$API/api/status" | python3 -c "import json,sys; h=json.load(sys.stdin)['sync']['history'][-1]; print(h.get('$1', h.get('error','')))"; }
# A zip of save folders, as RomM's clients sent PS2 saves before sigil: base64.
folders_zip() { python3 -c "
import base64,io,sys,zipfile
b=io.BytesIO(); z=zipfile.ZipFile(b,'w')
for a in sys.argv[1:]: n,_,v=a.partition('='); z.writestr(n,v)
z.close(); print(base64.b64encode(b.getvalue()).decode())" "$@"; }
# The same saves rewritten with other timestamps, as a game saving the same data does.
retime() { python3 -c "
import re,sys; p=sys.argv[1]; t=open(p).read()
open(p,'w').write(re.sub(r'timeModified: (\d+)', lambda m: 'timeModified: %d' % (int(m.group(1)) + 3600), t))" "$C/BASLUS-21059/_pcsx2_index"; }
server_save() { curl -sf -X POST "$MOCK/_admin/save" -d "{\"rom_id\":20,\"slot\":\"autosave\",\"file_name\":\"Okami.zip\",\"content_b64\":\"$1\"}" >/dev/null; }
newest() { curl -sf "$MOCK/_admin/dump" | python3 -c "
import json,re,sys
s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==20], key=lambda x: x['updated_at'])[-1]
print(re.sub(r' \[[^]]*\]', '', s['file_name']), s['file_size_bytes'], s['content'][:27])"; }

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
E="$T/PCSX2"; C="$E/Mcd001.ps2"   # no memcards/ folder: the cards sit in PS5SX2's own
mkdir -p "$E/games" "$E/settings" "$C" "$T/data"
python3 "$ROOT/tests/make_disc.py" psx "$E/games/Okami.iso" SLUS_210.59
: > "$C/_pcsx2_superblock"        # a new folder card, which sigil formats
printf 'MemoryCards/Slot1_Filename=Mcd001.ps2\n' > "$E/gs.ini"
python3 - "$ROOT/platform/ps5/emulators.json" "$E" "$WEB_PORT" > "$T/data/config.json" <<'PY'
import json, sys
p = next(e for e in json.load(open(sys.argv[1])) if e["id"] == "ps5sx2")["profile"]
p.update({"id": "ps5sx2", "name": "PS5SX2", "enabled": True, "root": sys.argv[2]})
p.pop("firmware", None)
print(json.dumps({"web_port": int(sys.argv[3]), "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": True,
                  "ps2_cards": "per_game", "profiles": [p]}))  # shared cards are the default
PY
server_save "$(folders_zip BASLUS-21059/BASLUS-21059=okami-v1 BASLUS-21059/icon.sys=icon)"
ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/none" ROMM_SYNC_APP_DIRS="$T/none" ROMM_SYNC_FS_ROOTS="$T" \
  "$ROOT/build/host/romm-sync" >"$T/daemon.log" 2>&1 &
for _ in $(seq 1 40); do curl -sf "$API/api/status" >/dev/null && curl -sf "$MOCK/api/heartbeat" >/dev/null && break; sleep 0.25; done
sleep 2   # let the (unpaired, skipped) startup sync pass

echo "1. RomM's save comes down onto the folder card"
post /api/pair/start "{\"server_url\":\"$MOCK\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
wait_sync 1
[[ "$(cat "$C/BASLUS-21059/BASLUS-21059" 2>/dev/null)" == okami-v1 ]] || fail "the save didn't come down onto the card"
[[ "$(cat "$C/BASLUS-21059/icon.sys")" == icon ]] || fail "the save's icon didn't come down"
[[ $(wc -c < "$C/_pcsx2_superblock") -eq 8192 ]] || fail "the new card wasn't formatted"
[[ "$(newest)" == "Okami.zip "* ]] || fail "a save that came down went back up: $(newest)"

echo "2. another game's save on the card changes nothing for this one"
mkdir -p "$C/BASLUS-20000"; printf other > "$C/BASLUS-20000/BASLUS-20000"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "an untouched save moved: $(last uploaded) up, $(last downloaded) down"

echo "3. the game saves: it goes up as sigil's card of its saves"
sleep 1.1; printf okami-v2 > "$C/BASLUS-21059/BASLUS-21059"
syncnow
[[ "$(last uploaded)" == 1 ]] || fail "the changed save didn't go up"
[[ "$(newest)" == "Okami.ps2 8650752 Sony PS2 Memory Card Format" ]] || fail "not sigil's card: $(newest)"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "the same save moved again"
sleep 1.1; retime
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "the same saves with other timestamps moved"

echo "4. a newer save from another device replaces only this game's, with a backup"
sleep 1.1; server_save "$(folders_zip BASLUS-21059/BASLUS-21059=okami-v3 BASLUS-21059/icon.sys=icon)"
syncnow
[[ "$(cat "$C/BASLUS-21059/BASLUS-21059")" == okami-v3 ]] || fail "the newer save didn't come down"
[[ "$(cat "$C/BASLUS-20000/BASLUS-20000")" == other ]] || fail "another game's save was touched"
ls "$T/data/backups/20/" 2>/dev/null | grep -q . || fail "no backup of the replaced save"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "it kept moving: $(last uploaded) up, $(last downloaded) down"
sleep 1.1; retime
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "RomM's saves with this console's timestamps moved"

echo "5. with no choice made, PS5SX2 keeps its shared cards: game saves don't sync on their own"
post /api/config '{"ps2_cards":""}' >/dev/null
[[ "$(curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(json.load(sys.stdin)['mode'])")" == shared ]] || fail "shared cards aren't the default"
sleep 1.1; printf okami-v4 > "$C/BASLUS-21059/BASLUS-21059"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "a game's save went up on its own with shared cards"
echo "ps2 e2e: ok"
