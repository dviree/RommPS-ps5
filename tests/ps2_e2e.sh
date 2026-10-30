#!/usr/bin/env bash
# PS2 memory cards, one per game (save_layout "ps2", pcsx2_cards, src/ps2card.c),
# against tests/mock_romm.py:
#   - PS5SX2 (the PS5 preset): the game's settings get its own card, its saves
#     on the shared card are copied there, and that card syncs as sigil's
#     one-game card; the shared card stays as it was
#   - nothing on the cards or in the settings changes while the emulator runs
#   - PCSX2's own settings (an [MemoryCards] section in <serial>_<crc>.ini),
#     as RetroArch's PS5SX2 core keeps them
#   - Crux2, which gives each game a card itself (named by its game database)
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
grep -q "Its newer save comes down once PS5SX2 is closed" "$T/daemon.log" || fail "no toast saying the newer save waits for the emulator to close"
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

echo "7. two emulators play the game: the one with none of its saves gets RomM's, which the other holds"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
rm -rf "$OWN"   # PS5SX2's card for the game is gone; RetroArch's core still has the save RomM has
python3 - "$ROOT/platform/ps5/emulators.json" "$E" "$T/data/config.json" <<'PY'
import json, sys
p = next(e for e in json.load(open(sys.argv[1]))["emulators"] if e["id"] == "ps5sx2")["profile"]
p.update({"id": "ps5sx2", "name": "PS5SX2", "enabled": True, "root": sys.argv[2]})
p.pop("firmware", None)
c = json.load(open(sys.argv[3]))
c["profiles"].append(p)  # after the core, which holds the save
json.dump(c, open(sys.argv[3], "w"))
PY
start
syncnow
has "$RC/Okami.ps2" okami-v6 || fail "RetroArch's core lost the game's save"
has "$OWN" okami-v6 || fail "PS5SX2 didn't get the game's save its other emulator holds"

echo "8. a save made in one emulator reaches the other: the copy saved last goes up, the other gets it from RomM"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
rm -rf "$OWN"; mkdir -p "$OWN/BASLUS-21059"; : > "$OWN/_pcsx2_superblock"   # PS5SX2 saves anew (a folder card)
printf okami-v8 > "$OWN/BASLUS-21059/BASLUS-21059"; printf icon > "$OWN/BASLUS-21059/icon.sys"
start
syncnow
has "$RC/Okami.ps2" okami-v8 || fail "RetroArch's core didn't get the save made in PS5SX2"
[[ "$(cat "$OWN/BASLUS-21059/BASLUS-21059")" == okami-v8 ]] || fail "PS5SX2's new save changed"

echo "9. Crux2 names each game's card itself, by its game database's group: RomM's save lands there,"
echo "   in /data/crux2's stand-in (user_dir); its numbered states sync, its undo state doesn't"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
X="$T/homebrew/PPSA99249"; U="$T/crux2"; GROUP="$U/memcards/SLUS-20999.ps2"
mkdir -p "$X/games" "$X/resources" "$U/memcards/Mcd001.ps2" "$U/inis" "$U/sstates"
cp "$E/games/Okami.iso" "$X/games/"
: > "$U/memcards/Mcd001.ps2/_pcsx2_superblock"
# Okami joins SLUS-20999's group through SLUS-21115, which names both forms of list item.
cat > "$X/resources/GameIndex.yaml" <<'YAML'
SLUS-20999:
  name: "Earlier"
  memcardFilters:
    - "SLUS-20999"
    - "SLUS-21115"
SLUS-21059:
  name: "Okami"
  region: "NTSC-U"
SLUS-21115:
  name: "Later"
  memcardFilters:
    - SLUS-21115 # itself
    - "SLUS-21059"
  gameFixes:
    - SoftwareRendererFMVHack
YAML
printf '[MemoryCards]\nSlot1_Filename = Mcd001.ps2\n\n[PS5]\nCardPerGame = true\n' > "$U/inis/PCSX2.ini"
ST="$U/sstates/SLUS-21059 ($(cat "$T/crc"))"
printf crux-state-1 > "$ST.01.p2s"; printf crux-undo > "$ST.undo.p2s"
python3 - "$ROOT/platform/ps5/emulators.json" "$X" "$U" "$T/data/config.json" <<'PY'
import json, sys
p = next(e for e in json.load(open(sys.argv[1]))["emulators"] if e["id"] == "crux2")["profile"]
p.update({"id": "crux2", "name": "Crux2", "enabled": True, "root": sys.argv[2], "user_dir": sys.argv[3] + ",{root}"})
p.pop("firmware", None)
c = json.load(open(sys.argv[4]))
c["profiles"] = [p]
c["ps2_cards"] = ""
c["states"] = "upload"
json.dump(c, open(sys.argv[4], "w"))
PY
start
syncnow
has "$GROUP" okami-v8 || fail "RomM's save didn't land on the card Crux2 gives the game ($(ls "$U/memcards"))"
[[ "$(ls "$U/memcards/Mcd001.ps2")" == _pcsx2_superblock ]] || fail "the shared card changed: $(ls "$U/memcards/Mcd001.ps2")"
states=$(curl -sf "$MOCK/_admin/dump" | python3 -c "import json,re,sys; print(sorted(re.sub(r' \[[^]]*\]', '', s['file_name']) for s in json.load(sys.stdin)['states'] if s['rom_id'] == 20))")
[[ "$states" == "['SLUS-21059 ($(cat "$T/crc")).01.p2s']" ]] || fail "Crux2's states on RomM: $states"
echo "   with Crux2's own cards off, slot 1 is its shared card again"
printf '[MemoryCards]\nSlot1_Filename = Mcd001.ps2\n\n[PS5]\nCardPerGame = false\n' > "$U/inis/PCSX2.ini"
[[ "$(curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(json.load(sys.stdin)['mode'])")" == shared ]] ||
  fail "Crux2's setting off didn't make its cards shared"

echo "10. an emulator turned on with a newer save of its own, never synced, gets RomM's; its own doesn't go up"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
printf '[MemoryCards]\nSlot1_Filename = Mcd001.ps2\n\n[PS5]\nCardPerGame = true\n' > "$U/inis/PCSX2.ini"   # its own cards on again
R2="$T/homebrew/PPSA99170"; RC2="$R2/system/pcsx2/memcards"
mkdir -p "$R2/games" "$RC2/Okami.ps2/BASLUS-21059" "$R2/system/ps5sx2/gamesettings"
cp "$E/games/Okami.iso" "$R2/games/"
# A formatted folder card, as PCSX2 keeps one (its superblock says so at 0x16).
python3 -c "import sys; b=bytearray(0x2000); b[0x16]=0x6F; open(sys.argv[1],'wb').write(bytes(b))" "$RC2/Okami.ps2/_pcsx2_superblock"
printf okami-stale > "$RC2/Okami.ps2/BASLUS-21059/BASLUS-21059"; printf icon > "$RC2/Okami.ps2/BASLUS-21059/icon.sys"
printf '[MemoryCards]\nSlot1_Filename = Okami.ps2\n' > "$R2/system/ps5sx2/gamesettings/SLUS-21059_$(cat "$T/crc").ini"
saves() { curl -sf "$MOCK/_admin/dump" | python3 -c "import json,sys; print(len([s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 20]))"; }
before=$(saves)
python3 - "$ROOT/platform/retroarch_cores.json" "$R2" "$T/data/config.json" <<'PY'
import json, sys
rule = next(r for r in json.load(open(sys.argv[1]))["rules"] if r.get("names") == ["PS5SX2"])["entry"]
c = json.load(open(sys.argv[3]))
e = dict(rule, romm=["ps2"], dir="ps2", emulator="ps5sx2", core_name="PS5SX2")
c["profiles"].append({"id": "pcsx2core2", "name": "Another PS5SX2 core", "enabled": True, "root": sys.argv[2],
                      "rom_dir": "{root}/games", "bios_dir": "{root}/system", "platforms": [e]})
c.setdefault("extra", {})
json.dump(c, open(sys.argv[3], "w"))
PY
start
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "a save RomM never saw went up and beat the one it has"
[[ "$(cat "$RC2/Okami.ps2/BASLUS-21059/BASLUS-21059")" == okami-v8 ]] || fail "the emulator just turned on didn't get RomM's save over its own"
[[ "$(saves)" == "$before" ]] || fail "RomM's saves changed: $before -> $(saves)"

echo "11. a save made while RommPS watched an extra emulator run goes up, though RomM never saw that card, and reaches the others"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
R3="$T/homebrew/PPSA99171"; RC3="$R3/system/pcsx2/memcards"
mkdir -p "$R3/games" "$RC3/Okami.ps2/BASLUS-21059" "$R3/system/ps5sx2/gamesettings"
cp "$E/games/Okami.iso" "$R3/games/"
python3 -c "import sys; b=bytearray(0x2000); b[0x16]=0x6F; open(sys.argv[1],'wb').write(bytes(b))" "$RC3/Okami.ps2/_pcsx2_superblock"
printf okami-old > "$RC3/Okami.ps2/BASLUS-21059/BASLUS-21059"; printf icon > "$RC3/Okami.ps2/BASLUS-21059/icon.sys"
touch -d "2 days ago" "$RC3/Okami.ps2/BASLUS-21059/BASLUS-21059" "$RC3/Okami.ps2/BASLUS-21059/icon.sys" "$RC3/Okami.ps2"
printf '[MemoryCards]\nSlot1_Filename = Okami.ps2\n' > "$R3/system/ps5sx2/gamesettings/SLUS-21059_$(cat "$T/crc").ini"
python3 - "$ROOT/platform/retroarch_cores.json" "$R3" "$T/data/config.json" <<'PY'
import json, sys
rule = next(r for r in json.load(open(sys.argv[1]))["rules"] if r.get("names") == ["PS5SX2"])["entry"]
c = json.load(open(sys.argv[3]))
e = dict(rule, romm=["ps2"], dir="ps2", emulator="ps5sx2", core_name="PS5SX2")
# An extra (another emulator for a system that has its main one), as on a console.
c["profiles"].append({"id": "pcsx2core3", "name": "A third PS5SX2 core", "enabled": True, "root": sys.argv[2],
                      "rom_dir": "{root}/games", "bios_dir": "{root}/system", "platforms": [e], "extra": ["ps2"]})
json.dump(c, open(sys.argv[3], "w"))
PY
# It runs from the start, so nothing comes down onto its card; it's played and saved.
echo PPSA99171 > "$T/data/alive"; echo PPSA99171 > "$T/data/foreground"
start
for _ in $(seq 1 40); do grep -q "game PPSA99171 started" "$T/daemon.log" && break; sleep 0.25; done
grep -q "game PPSA99171 started" "$T/daemon.log" || fail "the run wasn't seen"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "a card RomM never saw went up before anyone played"
sleep 1.1; printf okami-played > "$RC3/Okami.ps2/BASLUS-21059/BASLUS-21059"
rm "$T/data/alive" "$T/data/foreground"
for _ in $(seq 1 40); do grep -q "game PPSA99171 closed" "$T/daemon.log" && break; sleep 0.25; done
syncnow
[[ "$(last uploaded)" == 1 ]] || fail "the save made while it ran didn't go up: $(last uploaded) up"
has "$RC2/Okami.ps2/BASLUS-21059/BASLUS-21059" okami-played || fail "the other emulator didn't get the save made while this one ran"
grep -q "1 save up, now in .*Another PS5SX2 core" "$T/daemon.log" || fail "the toast must say which emulators got the save"
echo "ps2 e2e: ok"
