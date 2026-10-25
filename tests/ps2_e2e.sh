#!/usr/bin/env bash
# PS2 memory cards, one per game (save_layout "ps2", pcsx2_cards, src/ps2card.c),
# against tests/mock_romm.py:
#   - PS5SX2 (the PS5 preset): the game's settings get its own card, its saves
#     on the shared card are copied there, and that card syncs as sigil's
#     one-game card; the shared card stays as it was
#   - nothing on the cards or in the settings changes while the emulator runs
#   - PCSX2's own settings (an [MemoryCards] section in <serial>_<crc>.ini),
#     as RetroArch's PS5SX2 core keeps them
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
server_save() { curl -sf -X POST "$MOCK/_admin/save" -d "{\"rom_id\":20,\"slot\":\"autosave\",\"file_name\":\"Okami.zip\",\"content_b64\":\"$1\"}" >/dev/null; }
newest() { curl -sf "$MOCK/_admin/dump" | python3 -c "
import json,re,sys
s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==20], key=lambda x: x['updated_at'])[-1]
print(re.sub(r' \[[^]]*\]', '', s['file_name']), s['file_size_bytes'], s['content'][:27])"; }
has() { grep -a -q "$2" "$1" 2>/dev/null; } # a save's data on a file card
start() {
  ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_APP_DIRS="$T/none" ROMM_SYNC_FS_ROOTS="$T" \
    ROMM_SYNC_CATALOG="$ROOT/platform/ps5/emulators.json" \
    "$ROOT/build/host/romm-sync" >>"$T/daemon.log" 2>&1 &
  DAEMON=$!
  for _ in $(seq 1 40); do curl -sf "$API/api/status" >/dev/null && break; sleep 0.25; done
}

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
E="$T/PCSX2"; C="$E/Mcd001.ps2"; OWN="$E/Okami.ps2"   # no memcards/ folder: the cards sit in PS5SX2's own
mkdir -p "$E/games" "$E/settings" "$T/data" "$T/homebrew"
python3 "$ROOT/tests/make_disc.py" ps2 "$E/games/Okami.iso" SLUS_210.59 >"$T/crc"
# The shared card in slot 1, a folder card holding the game's save and another game's.
mkdir -p "$C/BASLUS-21059" "$C/BASLUS-20000"
: > "$C/_pcsx2_superblock"
printf okami-v0 > "$C/BASLUS-21059/BASLUS-21059"; printf icon > "$C/BASLUS-21059/icon.sys"
printf other > "$C/BASLUS-20000/BASLUS-20000"
printf 'MemoryCards/Slot1_Filename=Mcd001.ps2\n' > "$E/gs.ini"
printf '# Okami (SLUS-21059): written from the PS5SX2 shelf\nupscale_multiplier=3\n' > "$E/settings/Okami.ini"
python3 - "$ROOT/platform/ps5/emulators.json" "$E" "$WEB_PORT" > "$T/data/config.json" <<'PY'
import json, sys
p = next(e for e in json.load(open(sys.argv[1]))["emulators"] if e["id"] == "ps5sx2")["profile"]
p.update({"id": "ps5sx2", "name": "PS5SX2", "enabled": True, "root": sys.argv[2]})
p.pop("firmware", None)
print(json.dumps({"web_port": int(sys.argv[3]), "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": True,
                  "profiles": [p]}))  # one card per game, the default
PY
for _ in $(seq 1 40); do curl -sf "$MOCK/api/heartbeat" >/dev/null && break; sleep 0.25; done
start
sleep 2   # let the (unpaired, skipped) startup sync pass

echo "1. the game gets its own card, with its saves; the shared card stays as it was"
before=$(cd "$C" && find . -type f | sort | xargs md5sum)
post /api/pair/start "{\"server_url\":\"$MOCK\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
wait_sync 1
[[ "$(cat "$E/settings/Okami.ini")" == $'# Okami (SLUS-21059): written from the PS5SX2 shelf\nupscale_multiplier=3\nMemoryCards/Slot1_Filename=Okami.ps2' ]] ||
  fail "the game's settings: $(cat "$E/settings/Okami.ini")"
[[ $(wc -c < "$OWN") -eq 8650752 ]] && has "$OWN" okami-v0 || fail "the game's own card doesn't hold its save"
has "$OWN" other && fail "another game's save went on its card"
[[ "$(cd "$C" && find . -type f | sort | xargs md5sum)" == "$before" ]] || fail "the shared card changed"
[[ "$(newest)" == "Okami.ps2 8650752 Sony PS2 Memory Card Format" ]] || fail "not sigil's card on RomM: $(newest)"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "an untouched save moved: $(last uploaded) up, $(last downloaded) down"

echo "2. a newer save from another device comes down onto the game's card, with a backup"
sleep 1.1; server_save "$(folders_zip BASLUS-21059/BASLUS-21059=okami-v3 BASLUS-21059/icon.sys=icon)"
syncnow
has "$OWN" okami-v3 || fail "the newer save didn't come down onto the game's card"
has "$C/BASLUS-21059/BASLUS-21059" okami-v0 || fail "the shared card's copy was touched"
ls "$T/data/backups/20/" 2>/dev/null | grep -q . || fail "no backup of the replaced save"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "it kept moving: $(last uploaded) up, $(last downloaded) down"

echo "3. nothing comes down while PS5SX2 runs (a resting console still has its processes)"
echo PPSA99203 > "$T/data/alive"
sleep 1.1; server_save "$(folders_zip BASLUS-21059/BASLUS-21059=okami-v5 BASLUS-21059/icon.sys=icon)"
syncnow
has "$OWN" okami-v5 && fail "a save came down onto the card of a running emulator"
grep -q "comes down once PS5SX2 is closed" "$T/daemon.log" || fail "no word on why it waits"
rm "$T/data/alive"
syncnow
has "$OWN" okami-v5 || fail "the save didn't come down once PS5SX2 closed"

echo "4. one card per game is the default; shared cards, once chosen, stop it, and games keep the cards they have"
post /api/config '{"ps2_cards":""}' >/dev/null
[[ "$(curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(json.load(sys.stdin)['mode'])")" == per_game ]] || fail "one card per game isn't the default"
post /api/config '{"ps2_cards":"shared"}' >/dev/null
[[ "$(curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(json.load(sys.stdin)['mode'])")" == shared ]] || fail "shared cards weren't taken"
sleep 1.1; server_save "$(folders_zip BASLUS-21059/BASLUS-21059=okami-v6 BASLUS-21059/icon.sys=icon)"
syncnow
has "$OWN" okami-v6 && fail "a game's save came down on its own with shared cards"
grep -q "Slot1_Filename=Okami.ps2" "$E/settings/Okami.ini" || fail "the game's card was taken away"

echo "5. PCSX2's [MemoryCards] in <serial>_<crc>.ini, by RetroArch's core rule, not while it runs;"
echo "   RomM's save lands on the game's new card, never on the shared one in slot 2"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
R="$T/homebrew/PPSA99169"; RC="$R/system/pcsx2/memcards"
mkdir -p "$R/games" "$RC/Mcd001.ps2" "$RC/Mcd002.ps2/BASLUS-20000" "$R/system/ps5sx2/gamesettings"
cp "$E/games/Okami.iso" "$R/games/"
: > "$RC/Mcd001.ps2/_pcsx2_superblock"   # no save of the game here
: > "$RC/Mcd002.ps2/_pcsx2_superblock"; printf other > "$RC/Mcd002.ps2/BASLUS-20000/BASLUS-20000"
INI="$R/system/ps5sx2/gamesettings/SLUS-21059_$(cat "$T/crc").ini"
python3 - "$ROOT/platform/retroarch_cores.json" "$R" "$T/data/config.json" <<'PY'
import json, sys
rule = next(r for r in json.load(open(sys.argv[1]))["rules"] if r.get("names") == ["PS5SX2"])["entry"]
c = json.load(open(sys.argv[3]))
e = dict(rule, romm=["ps2"], dir="ps2", emulator="ps5sx2", core_name="PS5SX2")
c["profiles"] = [{"id": "pcsx2core", "name": "PS5SX2 core", "enabled": True, "root": sys.argv[2],
                  "rom_dir": "{root}/games", "bios_dir": "{root}/system", "platforms": [e]}]
c["ps2_cards"] = ""
json.dump(c, open(sys.argv[3], "w"))
PY
echo PPSA99169 > "$T/data/alive"
start
syncnow
[[ -e "$INI" || -e "$RC/Okami.ps2" ]] && fail "its settings or cards changed while it ran"
rm "$T/data/alive"
syncnow
[[ "$(cat "$INI" 2>/dev/null)" == $'[MemoryCards]\nSlot1_Filename = Okami.ps2' ]] || fail "the game's settings: $(cat "$INI" 2>/dev/null)"
has "$RC/Okami.ps2" okami-v6 || fail "RomM's save didn't come down onto the game's own card"
[[ -e "$RC/Mcd002.ps2/BASLUS-21059" ]] && fail "RomM's save went on the shared card in slot 2"
[[ "$(ls "$RC/Mcd002.ps2")" == $'BASLUS-20000\n_pcsx2_superblock' ]] || fail "the shared card in slot 2 changed: $(ls "$RC/Mcd002.ps2")"
echo "6. an update keeps shared cards on a console that synced them, once; others get one card per game"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
mode() { curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(json.load(sys.stdin)['mode'])"; }
cfg() { python3 - "$T/data/config.json" "$@" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
for kv in sys.argv[2:]:
    k, _, v = kv.partition("=")
    if v == "-": c.pop(k, None)
    else: c[k] = v
json.dump(c, open(sys.argv[1], "w"))
PY
}
# 1.3.0's setup saved "per_game" even with the choice hidden; its shared card synced.
cfg ps2_cards=per_game ps2_cards_checked=-
python3 - "$T/data/state.json" "$RC/Mcd001.ps2" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
s.setdefault("memcards", {})[sys.argv[2]] = {"card_id": 7, "card": "x", "hash": "x", "version_id": 1, "synced_at": 1}
json.dump(s, open(sys.argv[1], "w"))
PY
start
[[ "$(mode)" == shared ]] || fail "a console that synced shared cards didn't keep them"
[[ "$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('ps2_cards_checked'))" "$T/data/config.json")" == True ]] ||
  fail "the check isn't remembered"
post /api/config '{"ps2_cards":"per_game"}' >/dev/null
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
start
[[ "$(mode)" == per_game ]] || fail "the user's choice after the check didn't stay"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
cfg ps2_cards=per_game ps2_cards_checked=-
python3 - "$T/data/state.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
s["memcards"] = {k: v for k, v in s.get("memcards", {}).items() if not k.endswith(".ps2")}
json.dump(s, open(sys.argv[1], "w"))
PY
start
[[ "$(mode)" == per_game ]] || fail "a console with no shared cards synced didn't get one card per game"
echo "ps2 e2e: ok"
