#!/usr/bin/env bash
# End-to-end test of the host build against tests/mock_romm.py.
#   make host tools && tests/e2e.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
MOCK_PORT=8899 WEB_PORT=8781
API="http://127.0.0.1:${WEB_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT

fail() { echo "FAIL: $*"; echo "--- daemon log"; tail -30 "$T/daemon.log"; exit 1; }
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_sync() { # waits for the history length to reach $1
  for _ in $(seq 1 40); do
    local cur
    cur=$(curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']; print(s['sync_count'] if not s['running'] else -1)")
    [[ "$cur" -ge "$1" ]] && return 0; sleep 0.5
  done; fail "sync #$1 did not finish"
}
last() { curl -sf "$API/api/status" | python3 -c "import json,sys; h=json.load(sys.stdin)['sync']['history'][-1]; print(h.get('$1', h.get('error','')))"; }

REL="$T/release"; mkdir -p "$REL"
MOCK_RELEASE_DIR="$REL" python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
# The same mock over HTTPS with a self-signed certificate.
TLS_PORT=8898
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=127.0.0.1 -keyout "$T/tls.pem" -out "$T/tls.crt" 2>/dev/null
cat "$T/tls.crt" >> "$T/tls.pem"
python3 "$ROOT/tests/mock_romm.py" $TLS_PORT "$T/tls.pem" 2>"$T/mock-tls.log" &
RA="$T/RetroArch"; mkdir -p "$RA/roms/snes" "$RA/saves/Snes9x" "$T/data"
# RetroArch defaults: saves sorted into a folder per core name.
printf 'savefile_directory = ":/saves"\nsort_savefiles_enable = "true"\n' > "$RA/retroarch.cfg"
cat > "$T/data/config.json" <<EOF
{"web_port": $WEB_PORT, "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": true,
 "profiles": [{"id":"retroarch","name":"RetroArch","enabled":true,"root":"$RA",
   "rom_dir":"{root}/roms/{platform}","save_dir":"{root}/saves","state_dir":"{root}/states",
   "bios_dir":"{root}/system","save_exts":".srm","state_exts":".state*","retroarch_cfg":"{root}/retroarch.cfg",
   "platforms":[{"romm":["snes"],"dir":"snes","emulator":"snes9x","core_name":"Snes9x"},
     {"romm":["ps2"],"dir":"ps2","emulator":"pcsx2","core_name":"LRPS2","save_exts":".ps2",
      "sync_requires_option":{"key":"pcsx2_shared_memory_cards","value":"disabled","default":"enabled"}},
     {"romm":["psp"],"dir":"psp","emulator":"ppsspp","core_name":"PPSSPP","save_layout":"psp"},
     {"romm":["ngc"],"dir":"ngc","emulator":"dolphin","core_name":"Dolphin","save_layout":"gci"}]}]}
EOF
echo "ROM" > "$RA/roms/snes/Chrono Trigger (USA).sfc"     # present locally, matched by name
echo "local-save-v1" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
# A second, native-app style RetroArch install for the detection test.
HB="$T/homebrew/PPSA12345"; mkdir -p "$HB/config" "$HB/cores" "$HB/sce_sys"
echo 'savefile_directory = ":/savefiles"' > "$HB/config/retroarch.cfg"
printf 'corename = "Snes9x"\ndatabase = "Nintendo - Super Nintendo Entertainment System|Nintendo - Satellaview"\n' > "$HB/cores/snes9x_libretro.info"
printf 'corename = "LRPS2"\ndatabase = "Sony - PlayStation 2"\n' > "$HB/cores/pcsx2_libretro.info"
echo '{"titleId":"PPSA12345","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"RetroArch"}}}' > "$HB/sce_sys/param.json"
# A standalone SNES emulator from a test catalog, "installed" as TEST00001.
mkdir -p "$T/apps/TEST00001" "$T/snesemu"
cat > "$T/catalog.json" <<EOF
[{"id": "testsnes", "name": "Test SNES", "title_ids": ["TEST00001"],
  "profile": {"root": "$T/snesemu", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".srm", "platforms": [{"romm": ["snes", "sfam"], "dir": "snes"}]}}]
EOF
# An installed payload in a stand-in etaHEN folder, for the update test.
mkdir -p "$T/etaHEN/payloads"; echo "old-payload" > "$T/etaHEN/payloads/romm-sync.elf"
ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_AUTOSTART="$T/etaHEN/payloads" \
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" ROMM_SYNC_COVERS_TICK=1 \
  ROMM_SYNC_UPDATE_URL="http://127.0.0.1:$MOCK_PORT/_github/release" "$ROOT/build/host/romm-sync" >"$T/daemon.log" 2>&1 &
sleep 6   # let the (unpaired, skipped) startup sync pass

echo "0. self-signed HTTPS needs certificate checks turned off"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "untrusted certificate not reported: $r"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "an address without a scheme must not fall back to http on a certificate error: $r"
post /api/config '{"tls_verify":false}' >/dev/null
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *pending* ]] || fail "pairing with checks off failed: $r"
post /api/pair/forget >/dev/null; post /api/config '{"tls_verify":true}' >/dev/null

echo "1. setup: pair, detect emulators, preview, first sync"
post /api/pair/start "{\"server_url\":\"http://127.0.0.1:$MOCK_PORT\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
d=$(curl -sf "$API/api/setup/detect" | python3 -c "
import json,sys; c=json.load(sys.stdin)[0]; p=c['profile']['platforms']
print(c['title_id'], c['profile']['root'].endswith('PPSA12345'), [(x['dir'], x['core_name'], x.get('sync_saves', True)) for x in p])")
[[ "$d" == "PPSA12345 True [('ps2', 'LRPS2', True), ('snes', 'Snes9x', True), ('satellaview', 'Snes9x', True)]" ]] || fail "detection: $d"
sleep 1; [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")" == 0 ]] || fail "no sync may run before setup is finished"
pv=$(curl -sf "$API/api/setup/preview" | python3 -c "import json,sys; p=json.load(sys.stdin); print(len(p['uploads']), len(p['downloads']), p['local_games'])")
[[ "$pv" == "1 0 1" ]] || fail "preview: $pv"
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
wait_sync 1
[[ "$(last reason)" == "first sync" && "$(last uploaded)" == 1 ]] || fail "expected the first sync to upload the local save"

echo "2. game download brings its server save along"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null
post /api/download '{"rom_id":11,"name":"Super Metroid"}' >/dev/null
[[ "$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; print([d['name'] for d in json.load(sys.stdin) if d.get('rom_id') == 11][-1])")" == "Super Metroid" ]] \
  || fail "a download must show its name from the start"
wait_sync 2
[[ -f "$RA/roms/snes/Super Metroid (USA).sfc" ]] || fail "ROM not downloaded"
[[ "$(last reason)" == "game downloaded" ]] || fail "a finished download must sync the game's saves"
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "server save not downloaded"
for _ in $(seq 1 20); do
  dl=$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; d=[d for d in json.load(sys.stdin) if d.get('rom_id') == 11][-1]; print(d['status'], d.get('saves'))")
  [[ "$dl" == "done 1" ]] && break; sleep 0.25
done
[[ "$dl" == "done 1" ]] || fail "the download must end done, with the save it synced: $dl"

echo "2b. folder roms: one iso per folder, and a cue+bin set"
post /api/download '{"rom_id":22,"name":"Downhill Domination"}' >/dev/null
post /api/download '{"rom_id":25,"name":"War of the Monsters"}' >/dev/null
dl_wait() { # wait for a downloaded path to exist
  for _ in $(seq 1 30); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "not downloaded: $1"
}
D="$RA/roms/ps2/Downhill Domination (USA)"
W="$RA/roms/ps2/War of the Monsters (USA)"
dl_wait "$D/Downhill Domination (USA).iso"; dl_wait "$W/War of the Monsters (USA).cue"
[[ -d "$D" && -d "$W" ]] || fail "a folder rom must land in a folder named after its fs_name"
[[ "$(cat "$D/Downhill Domination (USA).iso")" == "ROM:Downhill Domination (USA).iso" ]] || fail "iso content wrong"
[[ -f "$W/War of the Monsters (USA).bin" ]] || fail "cue's bin not downloaded"
[[ "$(cat "$W/War of the Monsters (USA).bin")" == "ROM:War of the Monsters (USA).bin" ]] || fail "bin content wrong"
[[ ! -f "$RA/roms/ps2/Downhill Domination (USA)" ]] || fail "folder rom written as a single extensionless file"

ci=$(curl -sf "$API/api/roms?platform_id=1" | python3 -c "import json,sys; print(sorted(json.load(sys.stdin)['char_index'].items()))")
[[ "$ci" == "[('c', 0), ('e', 1), ('m', 2), ('s', 3)]" ]] || fail "library char_index for the A-Z ribbon: $ci"
inst=$(curl -sf "$API/api/installed" | python3 -c "import json,sys; print(sorted([(g['id'], g['name'], g['platform_id'] > 0) for g in json.load(sys.stdin)]))")
[[ "$inst" == "[(10, 'Chrono Trigger', True), (11, 'Super Metroid', True), (22, 'Downhill Domination', True), (25, 'War of the Monsters', True)]" ]] \
  || fail "the games on this console, for the app: $inst"
all=$(curl -sf "$API/api/roms?platform_id=0&search=metroid" | python3 -c "import json,sys; print([(g['name'], g['platform_id']) for g in json.load(sys.stdin)['items']])")
[[ "$all" == "[('Super Metroid', 1)]" ]] || fail "a search across every platform: $all"

echo "3. nothing changed, nothing transferred"
post /api/sync >/dev/null; wait_sync 3
[[ "$(last uploaded)$(last downloaded)" == "00" ]] || fail "expected nothing to transfer"

echo "4. save uploads while a game runs, downloads wait for it to close"
echo "PPSA00000" > "$T/data/foreground"; sleep 4
echo "local-save-v2" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
for _ in $(seq 1 16); do [[ "$(last reason)" == "save changed" && "$(last uploaded)" == 1 ]] && break; sleep 0.5; done
[[ "$(last reason)" == "save changed" && "$(last uploaded)" == 1 ]] || fail "change was not uploaded while playing"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"SM.srm","content":"from-other-device"}' >/dev/null
post /api/sync >/dev/null; sleep 2
[[ "$(last deferred)" == 1 && "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "download must wait while a game runs"
rm "$T/data/foreground"
for _ in $(seq 1 30); do [[ "$(last reason)" == "game exited" ]] && break; sleep 0.5; done
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "from-other-device" ]] || fail "deferred download not applied after exit"
sleep 3 # let the watcher see our own download settle (no-op sync)
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")

echo "5. conflict, keep the server copy"
sleep 1.1; curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":10,"slot":"autosave","file_name":"Chrono.srm","content":"other-device"}' >/dev/null
echo "local-save-v3" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last conflicts)" == 1 ]] || fail "conflict not detected"
post /api/conflicts/resolve "{\"path\":\"$RA/saves/Snes9x/Chrono Trigger (USA).srm\",\"keep\":\"server\"}" >/dev/null || fail "resolve failed"
[[ "$(cat "$RA/saves/Snes9x/Chrono Trigger (USA).srm")" == "other-device" ]] || fail "server copy not restored"
ls "$T/data/backups/10/" | grep -q "Chrono Trigger (USA).srm" || fail "no backup of the local save"


echo "6. stray save is left alone until moved"
echo "ROM" > "$RA/roms/snes/EarthBound (USA).sfc"
mkdir -p "$RA/saves/snes"; echo "old-layout" > "$RA/saves/snes/EarthBound (USA).srm"
sleep 3; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last uploaded)" == 0 ]] || fail "stray save must not be uploaded"
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']['strays']; print(len(s), s[0]['expected_path'] if s else '')")
[[ "$n" == "1 $RA/saves/Snes9x/EarthBound (USA).srm" ]] || fail "stray not reported correctly: $n"
post /api/strays/move "{\"path\":\"$RA/saves/snes/EarthBound (USA).srm\"}" >/dev/null || fail "move failed"
for _ in $(seq 1 20); do [[ "$(last uploaded)" == 1 ]] && break; sleep 0.5; done
[[ -f "$RA/saves/Snes9x/EarthBound (USA).srm" && "$(last uploaded)" == 1 ]] || fail "moved save not synced"

echo "7. PS2 per-game card syncs only with shared cards off"
mkdir -p "$RA/roms/ps2" "$RA/saves/LRPS2"; echo "ISO" > "$RA/roms/ps2/Okami.iso"; echo "card" > "$RA/saves/LRPS2/Okami.ps2"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if 'ps2' in m['platforms']][0])")
[[ "$note" == *pcsx2_shared_memory_cards* ]] || fail "expected a sync note for shared cards: $note"
sleep 3; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last uploaded)" == 0 ]] || fail "shared-card mode must not upload"
mkdir -p "$RA/LRPS2"; echo 'pcsx2_shared_memory_cards = "disabled"' > "$RA/LRPS2/LRPS2.opt"
post /api/sync >/dev/null; wait_sync $((n+2))
[[ "$(last uploaded)" == 1 ]] || fail "per-game card was not uploaded"

echo "8. PS2 shared card backup and restore"
mkdir -p "$RA/system/pcsx2/memcards"; echo "card-v1" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/config '{"ps2_cards":"backup"}' >/dev/null
post /api/sync >/dev/null; sleep 3
cards() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; c=json.load(sys.stdin)['cards']; print(c[0]['versions'] if c else 0)"; }
[[ "$(cards)" == 1 ]] || fail "card was not backed up"
echo "card-v2" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(cards)" == 2 ]] || fail "changed card was not backed up again"
post /api/sync >/dev/null; sleep 2
[[ "$(cards)" == 2 ]] || fail "unchanged card must not be uploaded"
echo "broken" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/memcards/restore '{"slot":1}' >/dev/null || fail "restore failed"
[[ "$(cat "$RA/system/pcsx2/memcards/Mcd001.ps2")" == "card-v2" ]] || fail "restored card has the wrong content"
ls "$T/data/backups/memcards/" | grep -q Mcd001.ps2 || fail "no local backup before restore"

dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
syncnow() { local n; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])"); post /api/sync >/dev/null; wait_sync $((n+1)); }

echo "9. RomM keeps only the newest versions per save"
post /api/config '{"server_versions":2}' >/dev/null
for v in v4 v5 v6; do sleep 1.1; echo "local-save-$v" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"; syncnow; done
n=$(dump | python3 -c "import json,sys; print(sum(1 for x in json.load(sys.stdin)['saves'] if x['rom_id']==10 and x['slot']=='autosave'))")
[[ "$n" == 2 ]] || fail "expected 2 versions on the server, found $n"

echo "10. states download, save history and restore"
post /api/config '{"states":"sync"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"emulator":"snes9x","file_name":"Super Metroid (USA).state1","content":"srv-state"}' >/dev/null
# Made by another core (or with no emulator at all): no emulator here can load it.
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"emulator":"bsnes","file_name":"Super Metroid (USA) [bsnes].state5","content":"other-core"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"file_name":"Super Metroid (USA) [unknown].state6","content":"no-emulator"}' >/dev/null
syncnow
! grep -rqs "other-core\|no-emulator" "$RA/states" || fail "a state from another core came down"
grep -q "skipped: made by bsnes" "$T/daemon.log" || fail "a skipped state must say why"
[[ "$(cat "$RA/states/Super Metroid (USA).state1" 2>/dev/null)" == "srv-state" ]] || fail "server state not downloaded"
# Two web player states for one game: each gets its own file, and they don't
# come down again on the next sync.
for ts in "2026-10-14 01-00-00-000" "2026-10-14 01-00-05-000"; do
  curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":11,\"emulator\":\"snes9x\",\"file_name\":\"Super Metroid (USA) [$ts].state\",\"content\":\"web-$ts\"}" >/dev/null
done
syncnow
[[ "$(cat "$RA/states/Super Metroid (USA).state")" == "web-2026-10-14 01-00-00-000" ]] || fail "first web state should take slot 0"
[[ "$(cat "$RA/states/Super Metroid (USA).state2")" == "web-2026-10-14 01-00-05-000" ]] || fail "second web state should take the next free slot"
[[ "$(cat "$RA/states/Super Metroid (USA).state1")" == "srv-state" ]] || fail "a downloaded state replaced another one"
syncnow
[[ "$(last states_downloaded)" == 0 ]] || fail "states came down again: $(last states_downloaded)"
# Saving over a state that came from elsewhere sends a new one; RomM's copy
# of the web state stays as it was.
echo "played-on-console" > "$RA/states/Super Metroid (USA).state"
syncnow
web=$(dump | python3 -c "import json,sys; print([s['file_name'] for s in json.load(sys.stdin)['states'] if s['rom_id']==11 and s['file_name'].endswith('.state')])")
[[ "$web" == *"Super Metroid (USA) [2026-10-14 01-00-00-000].state"* && "$web" == *"Super Metroid (USA) [PlayStation 5].state"* ]] \
  || fail "a state saved over a web state must go up as a new one: $web"
orig=$(curl -sf "$API/api/history?rom_id=11" >/dev/null; dump | python3 -c "import json,sys; print([s['file_size_bytes'] for s in json.load(sys.stdin)['states'] if s['file_name']=='Super Metroid (USA) [2026-10-14 01-00-00-000].state'][0])")
[[ "$orig" == 27 ]] || fail "the web state on RomM was changed (size $orig)"
syncnow
[[ "$(last states_downloaded)$(last states_uploaded)" == 00 ]] || fail "an uploaded state came back down or went up again"
# The player's own slot is never taken by a state from elsewhere.
echo "my-own-slot" > "$RA/states/Chrono Trigger (USA).state"
syncnow
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":10,"emulator":"snes9x","file_name":"Chrono Trigger (USA) [2026-10-14 02-00-00-000].state","content":"web-ct"}' >/dev/null
syncnow
[[ "$(cat "$RA/states/Chrono Trigger (USA).state")" == "my-own-slot" ]] || fail "a web state replaced the player's own slot"
[[ "$(cat "$RA/states/Chrono Trigger (USA).state1")" == "web-ct" ]] || fail "the web state should land in the next free slot"
old=$(curl -sf "$API/api/history?rom_id=10" | python3 -c "import json,sys; s=sorted(json.load(sys.stdin)['saves'], key=lambda x: x['updated_at']); print(s[0]['id'])")
want="$(curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/save_content?id=$old")"
post /api/restore "{\"kind\":\"save\",\"id\":$old}" >/dev/null || fail "restore failed"
[[ "$(cat "$RA/saves/Snes9x/Chrono Trigger (USA).srm")" == "$want" ]] || fail "restored save has the wrong content"
newest() { dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==10], key=lambda x: x['updated_at']); print(s[-1]['content'].strip())"; }
for _ in $(seq 1 20); do [[ "$(newest)" == "$want" ]] && break; sleep 0.5; done
[[ "$(newest)" == "$want" ]] || fail "restored save should upload as the newest version"

echo "11. per-game RetroArch overrides"
echo "ISO" > "$RA/roms/ps2/Other.iso"; echo "other-card" > "$RA/saves/LRPS2/Other.ps2"
echo 'pcsx2_shared_memory_cards = "enabled"' > "$RA/LRPS2/Other.opt"
mkdir -p "$RA/special" "$RA/Snes9x"; printf 'savefile_directory = ":/special"\nsort_savefiles_enable = "false"\n' > "$RA/Snes9x/EarthBound (USA).cfg"
echo "special-save" > "$RA/special/EarthBound (USA).srm"
syncnow
n=$(dump | python3 -c "import json,sys; print(sum(1 for x in json.load(sys.stdin)['saves'] if x['rom_id']==21))")
[[ "$n" == 0 ]] || fail "game with shared cards in its own options must not sync"
c=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==12], key=lambda x: x['updated_at']); print(s[-1]['content'].strip())")
[[ "$c" == "special-save" ]] || fail "per-game save folder not used (got $c)"

echo "12. PSP and GameCube saves as zips"
mkdir -p "$RA/roms/psp" "$RA/roms/ngc"
python3 "$ROOT/tests/make_disc.py" cso "$RA/roms/psp/Crisis Core (USA).iso" ULUS10336
python3 "$ROOT/tests/make_disc.py" psp "$RA/roms/psp/Crisis Core (USA).iso" ULUS10336
SD="$RA/saves/PPSSPP/PSP/SAVEDATA"; mkdir -p "$SD/ULUS10336DATA00" "$SD/ULUS10336GAMEDATA"
python3 "$ROOT/tests/make_disc.py" sfo "$SD/ULUS10336DATA00/PARAM.SFO" TITLE="Crisis Core" SAVEDATA_PARAMS=x
echo "psp-save-1" > "$SD/ULUS10336DATA00/DATA.BIN"
python3 "$ROOT/tests/make_disc.py" sfo "$SD/ULUS10336GAMEDATA/PARAM.SFO" TITLE="Crisis Core"
echo "install" > "$SD/ULUS10336GAMEDATA/BIG.BIN"
python3 "$ROOT/tests/make_disc.py" gc "$RA/roms/ngc/Wind Waker (USA).iso" GZLE01
CARD="$RA/saves/dolphin-emu/User/GC/USA/Card A"; mkdir -p "$CARD"; printf 'GZLE01zelda' > "$CARD/01-GZLE-zelda.gci"
syncnow
entries() { dump | python3 -c "
import json,sys,io,zipfile
s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==$1], key=lambda x: x['updated_at'])
import re; name=re.sub(r' \[[^]]*\]', '', s[-1]['file_name']) if s else ''
print(name, sorted(zipfile.ZipFile(io.BytesIO(s[-1]['content'].encode('latin1'))).namelist()) if s else 'none')"; }
[[ "$(entries 30)" == "ULUS10336.zip ['ULUS10336DATA00/DATA.BIN', 'ULUS10336DATA00/PARAM.SFO']" ]] || fail "PSP zip: $(entries 30)"
[[ "$(entries 40)" == "Wind Waker (USA).zip ['01-GZLE-zelda.gci']" ]] || fail "GC zip: $(entries 40)"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "unchanged PSP and GC saves must not upload again"
python3 - "$SD" > "$T/new.zip.b64" <<'PY'
import base64, io, sys, zipfile
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("ULUS10336DATA00/DATA.BIN", "psp-save-2\n")
    z.writestr("ULUS10336DATA00/PARAM.SFO", open(sys.argv[1] + "/ULUS10336DATA00/PARAM.SFO", "rb").read())
print(base64.b64encode(buf.getvalue()).decode())
PY
sleep 1.1
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d "{\"rom_id\":30,\"slot\":\"autosave\",\"file_name\":\"ULUS10336.zip\",\"content_b64\":\"$(cat "$T/new.zip.b64")\"}" >/dev/null
syncnow
[[ "$(cat "$SD/ULUS10336DATA00/DATA.BIN")" == "psp-save-2" ]] || fail "PSP save from the server not unpacked"
[[ -f "$SD/ULUS10336GAMEDATA/BIG.BIN" ]] || fail "game data folder must be left alone"
ls "$T/data/backups/30/" | grep -q "ULUS10336.zip" || fail "no backup of the PSP save"

echo "13. in-app update: only a correctly signed release installs"
SIGN="$ROOT/build/host/release-sign"
upd() { curl -sf "$API/api/update" | python3 -c "import json,sys; u=json.load(sys.stdin); print(u['$1'])"; }
# Publishes a release; $1 says what to break: tamper, wrongkey, unsigned, ps4 or nothing.
publish() {
  rm -f "$REL"/*; printf '\x7fELF new-payload %s' "$RANDOM" > "$REL/romm-sync.elf"
  "$SIGN" manifest v9.9.9 "$REL/romm-sync.elf" > "$REL/manifest.json"
  # ps4: a correctly signed manifest, but for the PS4 payload
  [[ "$1" == ps4 ]] && cp "$REL/romm-sync.elf" "$REL/romm-sync-ps4.elf" && "$SIGN" manifest v9.9.9 "$REL/romm-sync-ps4.elf" > "$REL/manifest.json"
  local key; key="$(cat "$ROOT/tests/update-test.key")"
  [[ "$1" == wrongkey ]] && key="$("$SIGN" keygen | sed -n 's/^secret: //p')"
  RELEASE_SIGNING_KEY="$key" "$SIGN" sign "$REL/manifest.json" > "$REL/manifest.sig" 2>/dev/null
  [[ "$1" == tamper ]] && python3 -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)" "$REL/romm-sync.elf"
  local names='"romm-sync.elf", "manifest.json", "manifest.sig"'; [[ "$1" == unsigned ]] && names='"romm-sync.elf"'
  python3 - "$REL" "http://127.0.0.1:$MOCK_PORT/_github/assets" "$names" <<'PY'
import json, sys
d, base, names = sys.argv[1], sys.argv[2], json.loads("[" + sys.argv[3] + "]")
json.dump({"tag_name": "v9.9.9", "body": "Test notes", "html_url": "https://example.invalid/r",
           "assets": [{"name": n, "browser_download_url": base + "/" + n} for n in names]}, open(d + "/release.json", "w"))
PY
  post /api/update/check >/dev/null || fail "update check refused"
  for _ in $(seq 1 20); do [[ "$(upd state)" != checking ]] && break; sleep 0.5; done
  [[ "$(upd available)" == True && "$(upd latest)" == 9.9.9 ]] || fail "release 9.9.9 not found: $(upd error)"
}
install_err() { # installs and prints the error it ends with, or the state it reached
  post /api/update/install >/dev/null || { echo refused; return; }
  for _ in $(seq 1 40); do local s; s="$(upd state)"; [[ "$s" == error ]] && { upd error; return; }; [[ "$s" == restarting ]] && { echo restarting; return; }; sleep 0.5; done
  echo "timeout: $(upd state)"
}
publish tamper;   r="$(install_err)"; [[ "$r" == *"signed checksum"* ]] || fail "tampered payload: $r"
publish wrongkey; r="$(install_err)"; [[ "$r" == *"signature doesn't match"* ]] || fail "untrusted key: $r"
publish unsigned; r="$(install_err)"; [[ "$r" == refused ]] || fail "unsigned release must be refused: $r"
publish ps4;      r="$(install_err)"; [[ "$r" == *"manifest is for romm-sync-ps4.elf"* ]] || fail "another console's payload: $r"
[[ "$(cat "$T/etaHEN/payloads/romm-sync.elf")" == old-payload && ! -f "$T/data/launched.elf" ]] || fail "a rejected update touched the payload"
publish ok;       r="$(install_err)"; [[ "$r" == restarting ]] || fail "good update: $r"
for _ in $(seq 1 10); do [[ -f "$T/data/launched.elf" ]] && break; sleep 0.5; done
cmp -s "$REL/romm-sync.elf" "$T/data/launched.elf" || fail "the new payload was not launched"
cmp -s "$REL/romm-sync.elf" "$T/etaHEN/payloads/romm-sync.elf" || fail "the installed payload was not replaced"
[[ ! -e "$T/etaHEN/payloads/romm-sync.elf.bak" ]] || fail "the update left a .bak of the old payload behind"

echo "14. diagnostic bundle: the logs, nothing that names the person"
echo "connecting to 10.0.16.84 from aa:bb:cc:dd:ee:ff, mail me@example.com, /user/home/1a2b3c4d/x, onlineId=SomePlayer99" >> "$T/data/romm-sync.log"
curl -sf "$API/api/diagnostics" > "$T/diag.zip" || fail "no diagnostic bundle"
python3 - "$T/diag.zip" "$T/data/config.json" <<'PY' || fail "diagnostic bundle"
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); cfg = json.load(open(sys.argv[2]))
names = z.namelist()
for want in ("report.txt", "romm-sync.log", "sync-log.json", "emulators-found.json", "emulator-settings.json", "kernel.log"):
    assert want in names, (want, names)
assert "RommPS diagnostics" in z.read("report.txt").decode()
text = "\n".join(z.read(n).decode("utf-8", "replace") for n in names)
host = cfg["server_url"].split("://")[1].split("/")[0].split(":")[0]
for secret in (cfg["token"], cfg["server_url"], host, cfg["device_id"], "10.0.16.84", "aa:bb:cc:dd:ee:ff", "me@example.com", "1a2b3c4d", "SomePlayer99"):
    assert secret and secret not in text, secret
assert "<ip>" in text and "<mac>" in text and "<email>" in text and "<server>" in text
PY
out=$(curl -sf -X POST "$API/api/diagnostics") || fail "saving a bundle refused"
[[ -f "$(echo "$out" | python3 -c "import json,sys; print(json.load(sys.stdin)['path'])")" ]] || fail "the saved bundle isn't there: $out"

echo "15. one emulator per system when several play it"
kinds=$(curl -sf "$API/api/setup/detect" | python3 -c "import json,sys; print([(c['kind'], c['title_id']) for c in json.load(sys.stdin)])")
[[ "$kinds" == *"('standalone', 'TEST00001')"* ]] || fail "the installed standalone emulator wasn't offered: $kinds"
snes_dirs() { curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin) if m['platforms'].split(',')[0] == 'snes'))"; }
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/snesemu\"],\"choices\":{\"snes\":\"$T/snesemu\"}}" >/dev/null || fail "setup with choices refused"
[[ "$(snes_dirs)" == "['$T/snesemu/roms']" ]] || fail "SNES should only go to the standalone emulator: $(snes_dirs)"
sel=$(curl -sf "$API/api/systems" | python3 -c "import json,sys; print([(s['system'], s['selected'], len(s['options'])) for s in json.load(sys.stdin)])")
[[ "$sel" == "[('snes', 'testsnes', 2)]" ]] || fail "systems: $sel"
post /api/systems '{"system":"snes","profile":"retroarch-PPSA12345"}' >/dev/null || fail "switching SNES refused"
[[ "$(snes_dirs)" == "['$HB/content/snes']" ]] || fail "SNES should be back with RetroArch: $(snes_dirs)"
curl -s -X POST "$API/api/systems" -d '{"system":"snes","profile":"nope"}' | grep -q error || fail "an unknown emulator must be refused"
[[ "$(snes_dirs)" == "['$HB/content/snes']" ]] || fail "a refused choice changed something: $(snes_dirs)"

echo "16. standalone emulators: saves by serial and hash, one save for two emulators, extras, shared cards"
# Like PSXS5 (cards named by disc serial), PS5N64 (saves by the game file's
# FNV-1a hash), a second N64 emulator keeping the same format (RetroArch-like
# names), PS5SX2 (two shared cards) and one found but not supported yet.
mkdir -p "$T/psxemu/games" "$T/psxemu/saves" "$T/psxemu/states" "$T/n64emu/games" "$T/n64emu/saves" \
         "$T/n64b/roms" "$T/n64b/saves" "$T/ps2emu/games" "$T/nope"
cat > "$T/catalog.json" <<EOF
[{"id": "testsnes", "name": "Test SNES", "title_ids": ["TEST00001"],
  "profile": {"root": "$T/snesemu", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".srm", "platforms": [{"romm": ["snes", "sfam"], "dir": "snes"}]}},
 {"id": "psxlike", "name": "PSX-like", "detect": ["$T/psxemu"],
  "profile": {"root": "$T/psxemu", "rom_dir": "{root}/games", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".mcd", "save_name": "{serial}_1{ext}", "save_stems": "{serial}_1",
              "state_exts": ".state*", "state_stems": "{rom_stem_safe}", "state_name": "{rom_stem_safe}{ext}",
              "save_name_by_core": {"mednafen_psx_hw": "{rom_stem}_1{ext}"},
              "state_format": "raw", "state_slot0": ".state0", "state_cores": "pcsx_rearmed,mednafen_psx_hw",
              "state_core_setting": {"file": "{root}/psxs5.ini", "key": "emulator",
                                     "values": {"0": "pcsx_rearmed", "2": "mednafen_psx_hw"}},
              "platforms": [{"romm": ["psx", "ps1"], "dir": "psx", "save_format": "psx-card"}]}},
 {"id": "n64like", "name": "N64-like", "detect": ["$T/n64emu"],
  "profile": {"root": "$T/n64emu", "rom_dir": "{root}/games", "save_dir": "{root}/saves", "state_dir": "{root}/saves",
              "save_exts": ".sav", "save_name": "{rom_fnv64}{ext}", "save_stems": "{rom_fnv64}",
              "platforms": [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]}},
 {"id": "n64b", "name": "Second N64", "detect": ["$T/n64b"],
  "profile": {"root": "$T/n64b", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "save_exts": ".srm",
              "platforms": [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]}},
 {"id": "ps2like", "name": "PS2-like", "detect": ["$T/ps2emu"],
  "profile": {"root": "$T/ps2emu", "rom_dir": "{root}/games", "sync_saves": false, "memcards": "{root}/Mcd001.ps2,{root}/Mcd002.ps2",
              "platforms": [{"romm": ["ps2"], "dir": "ps2"}]}},
 {"id": "nope", "name": "Not yet", "detect": ["$T/nope"], "ready": false, "note": "not supported yet",
  "profile": {"root": "$T/nope", "rom_dir": "{root}/games", "platforms": [{"romm": ["n64"], "dir": "n64"}]}}]
EOF
python3 "$ROOT/tests/make_disc.py" psxbin "$T/psxemu/games/Test Quest (USA).bin" SLUS_123.45
printf 'card-from-psx' > "$T/psxemu/saves/SLUS-12345_1.mcd"
printf 'STv4 PCSXra test-build' > "$T/psxemu/states/Test_Quest__USA_.state0"   # a bare PCSX-ReARMed state
printf 'emulator=0\n' > "$T/psxemu/psxs5.ini"
printf 'ROM:Test Racer (U).z64' > "$T/n64emu/games/Test Racer (U).z64"
cp "$T/n64emu/games/Test Racer (U).z64" "$T/n64b/roms/Test Racer (U).z64"
fnv=$(python3 -c "
h=0xcbf29ce484222325
for b in open('$T/n64emu/games/Test Racer (U).z64','rb').read(): h=((h^b)*0x100000001b3)&0xFFFFFFFFFFFFFFFF
print(f'{h:016x}')")
printf 'n64-save' > "$T/n64emu/saves/$fnv.sav"
printf 'card-ps2' > "$T/ps2emu/Mcd001.ps2"
printf 'ROM:Old Racer (U).z64' > "$T/n64emu/games/Old Racer (U).z64"   # already there before the extra is turned on
det=$(curl -sf "$API/api/setup/detect" | python3 -c "import json,sys; print(sorted((c['name'], c.get('ready', True)) for c in json.load(sys.stdin) if c['kind'] == 'standalone'))")
[[ "$det" == *"('Not yet', False)"* && "$det" == *"('PSX-like', True)"* ]] || fail "detection of the new kinds: $det"
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\"},\"also\":{\"n64\":[\"$T/n64b\"]}}" >/dev/null \
  || fail "setup with extras refused"
for _ in $(seq 1 20); do [[ -f "$T/n64b/roms/Old Racer (U).z64" ]] && break; sleep 0.25; done
[[ -f "$T/n64b/roms/Old Racer (U).z64" ]] || fail "turning an extra on must give it the games already there"
for _ in $(seq 1 20); do grep -q "NOTIFY.*now also sync with" "$T/daemon.log" && break; sleep 0.25; done
grep -q "NOTIFY\] RommPS: [0-9]* N64 games\{0,1\} now also sync with" "$T/daemon.log" || fail "no notice that the games now also sync with the extra"
dirs=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin)))")
[[ "$dirs" != *"$T/nope"* ]] || fail "an emulator not supported yet got a profile: $dirs"
post /api/config '{"states":"upload"}' >/dev/null
syncnow
dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
saves=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted((s['rom_id'], s['file_name'], s['slot']) for s in (d['saves'].values() if isinstance(d['saves'], dict) else d['saves']) if s['rom_id'] in (50, 60)))")
# (RomM puts the upload's time in the name: "SLUS-12345_1 [2026-...].mcd".)
[[ "$saves" == *"(50, 'SLUS-12345_1 ["*"].mcd', 'autosave')"* ]] || fail "the card named by serial didn't sync: $saves"
[[ "$saves" == *"(60, '$fnv ["*"].sav', 'autosave')"* ]] || fail "the save named by hash didn't sync: $saves"
[[ $(grep -o "(60," <<<"$saves" | wc -l) -eq 1 ]] || fail "two emulators sharing a save must upload one: $saves"
[[ "$(cat "$T/n64b/saves/Test Racer (U).srm" 2>/dev/null)" == "n64-save" ]] || fail "the shared save didn't reach the second emulator"
states=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted(s['file_name'] for s in (d['states'].values() if isinstance(d['states'], dict) else d['states']) if s['rom_id'] == 50))")
[[ "$states" == "['Test_Quest__USA_"*".state0']" ]] || fail "the state named by the safe name didn't sync: $states"
# A bare state goes up in RetroArch's container, as the core that made it, so
# RomM's web player can load it.
up=$(dump | python3 -c "import json,sys; print([(s['emulator'], s['file_size_bytes']) for s in json.load(sys.stdin)['states'] if s['rom_id'] == 50])")
[[ "$up" == "[('pcsx_rearmed', 48)]" ]] || fail "a PSXS5 state must go up wrapped, as pcsx_rearmed: $up"
c2=$(dump | python3 -c "import json,sys; print([c['versions'] for c in json.load(sys.stdin)['cards'] if c['emulator'] == 'ps2like' and c['name'].endswith('PS2-like slot 1')])")
[[ "$c2" == "[1]" ]] || fail "the shared PS2 card wasn't backed up: $c2"
# Web player states come down bare, into a slot of their own, and only for the
# core the emulator runs now.
post /api/config '{"states":"sync"}' >/dev/null
rast() { python3 -c "import json,sys,struct; m=sys.argv[1].encode(); print(json.dumps((b'RASTATE\x01MEM '+struct.pack('<I',len(m))+m+b'\0'*(-len(m)%8)+b'END \0\0\0\0').decode('latin1')))" "$1"; }
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":50,\"emulator\":\"pcsx_rearmed\",\"file_name\":\"Test Quest (USA) [web].state\",\"content\":$(rast 'STv4 PCSXra web-build')}" >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":50,\"emulator\":\"mednafen_psx_hw\",\"file_name\":\"Test Quest (USA) [beetle].state\",\"content\":$(rast 'MDFNSVST beetle')}" >/dev/null
syncnow
[[ "$(cat "$T/psxemu/states/Test_Quest__USA_.state1")" == "STv4 PCSXra web-build" ]] || fail "a web state must come down bare, in the next free slot"
[[ ! -e "$T/psxemu/states/Test_Quest__USA_.state2" ]] || fail "a Beetle state came down while the emulator runs PCSX-ReARMed"
printf 'emulator=2\n' > "$T/psxemu/psxs5.ini"
syncnow
[[ "$(cat "$T/psxemu/states/Test_Quest__USA_.state2")" == "MDFNSVST beetle" ]] || fail "switched to Beetle, its state must come down"
# With Beetle the card is named after the game file: still the game's card.
sleep 1; printf 'card-beetle' > "$T/psxemu/saves/Test Quest (USA)_1.mcd"
syncnow
card=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==50], key=lambda x: x['updated_at']); print(s[-1]['content'])")
[[ "$card" == "card-beetle" ]] || fail "the card Beetle names after the game file must sync: $card"
# A blank card an emulator just made under the other name never beats one with a save on it.
mkcard() { python3 -c "import sys; d=bytearray(131072); d[0:2]=b'MC'; d[128:128*16]=bytes([0xA0])*(128*15)
if sys.argv[2]=='save': d[128]=0x51; d[0x2000:0x2004]=b'GAME'
open(sys.argv[1],'wb').write(bytes(d))" "$1" "$2"; }
mkcard "$T/psxemu/saves/SLUS-12345_1.mcd" save; sleep 1; mkcard "$T/psxemu/saves/Test Quest (USA)_1.mcd" blank
syncnow
kind=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==50], key=lambda x: x['updated_at']); print('save' if 'GAME' in s[-1]['content'] else 'blank')")
[[ "$kind" == "save" ]] || fail "a blank card was uploaded over a card with a save"
# Nor does a blank card from RomM replace one with saves: that's a conflict to settle.
python3 -c "import base64,json,sys; d=bytearray(131072); d[0:2]=b'MC'; d[128:128*16]=bytes([0xA0])*(128*15)
json.dump({'rom_id': 50, 'slot': 'autosave', 'file_name': 'SLUS-12345_1.mcd', 'content_b64': base64.b64encode(bytes(d)).decode()}, open(sys.argv[1], 'w'))" "$T/blank.json"
sleep 1; curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d @"$T/blank.json" >/dev/null
syncnow
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(0 if d[128]==0x51 else 1)" "$T/psxemu/saves/SLUS-12345_1.mcd" \
  || fail "a blank card from RomM replaced a card with saves"
[[ "$(last conflicts)" -ge 1 ]] || fail "a blank card from RomM over one with saves must be a conflict"

post /api/config '{"states":"upload"}' >/dev/null
# A save from another device reaches both N64 emulators.
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":60,"slot":"autosave","file_name":"Test Racer (U).srm","content":"n64-from-elsewhere","emulator":"other"}' >/dev/null
syncnow
[[ "$(cat "$T/n64emu/saves/$fnv.sav")" == "n64-from-elsewhere" && "$(cat "$T/n64b/saves/Test Racer (U).srm")" == "n64-from-elsewhere" ]] \
  || fail "a downloaded save must reach every emulator that shares it"
# A download goes to the main emulator and is linked into the extra.
post /api/download '{"rom_id":61}' >/dev/null; sleep 2
[[ -f "$T/n64emu/games/Other Racer (U).z64" && -f "$T/n64b/roms/Other Racer (U).z64" ]] || fail "the download didn't reach the extra emulator"
python3 -c 'import os,sys; sys.exit(not os.path.samefile(sys.argv[1], sys.argv[2]))' \
  "$T/n64emu/games/Other Racer (U).z64" "$T/n64b/roms/Other Racer (U).z64" || fail "the extra copy should be a hard link"

echo "17. paired with another server, the old server's game and save ids are forgotten"
hist() { curl -sf "$API/api/status" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['sync']['history']))"; }
n=$(hist)
post /api/pair/forget >/dev/null
post /api/config '{"tls_verify":false}' >/dev/null
post /api/pair/start "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
grep -q "new pairing: the games are matched again" "$T/daemon.log" || fail "pairing again didn't forget the old server's records"
# Pairing syncs by itself: everything goes up to the new server, nothing fails.
wait_sync $((n + 1))
[[ "$(last reason)" == "paired" ]] || fail "expected the pairing's own sync: $(last reason)"
[[ "$(last failed)" == 0 ]] || fail "after pairing with another server nothing may fail: $(last failed) failed"
[[ "$(last uploaded)" -ge 1 ]] || fail "the saves must go up to the new server: $(last uploaded)"

echo "18. a game's details, the folder picker, and folders picked for a system no emulator plays"
det=$(curl -sf "$API/api/roms/11/details" | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(d.get('year'), d.get('publishers'), d.get('genres'), d.get('rating'), int(d.get('main_story_s',0)/3600), d.get('status'), d.get('play_ms'), d.get('sessions'))")
[[ "$det" == "1994 Nintendo Platform, Adventure 92.5 8 finished 5400000 2" ]] || fail "game details: $det"
root=$(curl -sf "$API/api/fs" | python3 -c "import json,sys; print(json.load(sys.stdin)['folders'])")
[[ "$root" == "['$T']" ]] || fail "the picker's roots: $root"
mkdir -p "$T/mine/games" "$T/mine/saves"
sub=$(curl -sf "$API/api/fs?path=$T/mine" | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['folders'], j['parent'] == '$T')")
[[ "$sub" == "['games', 'saves'] True" ]] || fail "listing a folder: $sub"
curl -s "$API/api/fs?path=/etc" | grep -q error || fail "the picker must stay inside its roots"
curl -s "$API/api/fs?path=$T/../.." | grep -q error || fail "the picker must refuse .."
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"slugs\":[\"psp\"],\"name\":\"PSP\",\"rom_dir\":\"$T/mine/games\",\"save_dir\":\"$T/mine/saves\"}]}" >/dev/null \
  || fail "setup with picked folders refused"
mine=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([(m['rom_dir'], m['save_dir']) for m in json.load(sys.stdin) if m['rom_dir'].startswith('$T/mine')])")
[[ "$mine" == "[('$T/mine/games', '$T/mine/saves')]" ]] || fail "the picked folders weren't used: $mine"
# An emulator with no preset, set up by its folders, for a system RetroArch plays too: an extra.
mkdir -p "$T/porp/games" "$T/porp/saves"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"id\":\"newemu-snes\",\"slugs\":[\"snes\",\"sfam\"],\"name\":\"NewEmu (SNES)\",\"rom_dir\":\"$T/porp/games\",\"save_dir\":\"$T/porp/saves\"}]}" >/dev/null \
  || fail "setup with an emulator's own folders refused"
porp=$(curl -sf "$API/api/config" | python3 -c "
import json,sys
for p in json.load(sys.stdin)['profiles']:
    if p['id'].startswith('custom-'): print(p['id'], p.get('extra'), p['rom_dir'] == '$T/porp/games')")
[[ "$porp" == "custom-newemu-snes ['snes'] True" ]] || fail "an emulator with no preset should be an extra next to RetroArch: $porp"
# Each system's folders: detection says where an emulator keeps them, and folders
# picked for one system in setup are used for it alone.
found=$(curl -sf "$API/api/setup/detect" | python3 -c "
import json,sys
for c in json.load(sys.stdin):
    if c['root']=='$HB': print(c['dirs']['snes']['rom_dir'] != '', 'fixed' in c)")
[[ "$found" == "True True" ]] || fail "detection must give each system's folders: $found"
mkdir -p "$T/mine/snes" "$T/mine/snessaves"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"systems\":{\"snes\":{\"main\":\"$HB\",\"on\":[\"$HB\"],
  \"folders\":{\"$HB\":{\"rom_dir\":\"$T/mine/snes\",\"save_dir\":\"$T/mine/snessaves\",\"state_dir\":\"\"}}}}}" >/dev/null \
  || fail "setup with a system's own folders refused"
sn=$(curl -sf "$API/api/paths" | python3 -c "
import json,sys
for m in json.load(sys.stdin):
    if 'snes' in m['platforms'].split(','): print(m['rom_dir'], m['save_dir'])
    elif m['rom_dir'].startswith('$T/mine/snes'): print('leaked', m['platforms'])")
grep -q "^$T/mine/snes $T/mine/snessaves$" <<<"$sn" && ! grep -q "^leaked" <<<"$sn" || fail "a system's picked folders must apply to it alone: $sn"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"id\":\"newemu-snes\",\"slugs\":[\"snes\",\"sfam\"],\"name\":\"NewEmu (SNES)\",\"rom_dir\":\"$T/porp/games\",\"save_dir\":\"$T/porp/saves\"}]}" >/dev/null

echo "19. every cover kept on the console, then only the missing and changed ones fetched"
# Paired with the HTTPS mock since scenario 17.
TM="https://127.0.0.1:$TLS_PORT"
hits() { curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; print(json.load(sys.stdin)['asset_hits'])"; }
covers_done() { # waits for a full pass to finish; prints done/total
  for _ in $(seq 1 120); do
    r=$(curl -sf "$API/api/status" | python3 -c "import json,sys; c=json.load(sys.stdin)['covers']; print(c['state'], c['done'], c['total'])")
    set -- $r; [[ "$1" == done && "$2" == "$3" && "$3" -gt 0 ]] && { echo "$2/$3"; return; }; sleep 0.5
  done; echo "timeout: $r"
}
post /api/covers/refresh >/dev/null
first=$(covers_done); [[ "$first" != timeout* ]] || fail "the full cover pass didn't finish: $first"
cached=$(ls "$T/data/cache/assets" | grep -vc '\.missing$')
[[ "$cached" -ge "${first#*/}" ]] || fail "every game's cover should be on the console: $cached files for $first"
before=$(hits)
p=$(curl -sf "$API/api/roms?platform_id=1&limit=1" | python3 -c "import json,sys; print(json.load(sys.stdin)['items'][0]['cover'])")
curl -sf -o /dev/null "$API/cover?p=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$p")" || fail "a cached cover wasn't served"
post /api/covers/refresh >/dev/null; sleep 3; covers_done >/dev/null
[[ "$(hits)" == "$before" ]] || fail "a second pass must not download covers it has: $before then $(hits)"
curl -sfk -X POST "$TM/_admin/cover_ts" -d '{"rom_id":11}' >/dev/null
post /api/covers/refresh >/dev/null; sleep 3; covers_done >/dev/null
[[ "$(hits)" == "$((before + 1))" ]] || fail "only the changed cover should be fetched again: $before then $(hits)"

echo "20. files that aren't games are hidden and never downloaded; versions say what sets them apart"
lib=$(curl -sf "$API/api/roms?platform_id=1&limit=50" | python3 -c "
import json,sys
for g in json.load(sys.stdin)['items']: print(g['id'], g.get('not_game', False), g.get('version', ''), g.get('versions', 0))")
grep -q "^13 True" <<<"$lib" || fail "systeminfo.txt must be marked as not a game: $lib"
grep -q "^14 False" <<<"$lib" || fail "a Mega Drive .md ROM is a game: $lib"
grep -q "^12 False USA · Rev 1 · Beta 2$" <<<"$lib" || fail "the version label: $lib"
grep -q "^10 False  0$" <<<"$lib" || fail "a game with one version has no label: $lib"
post /api/download '{"rom_id":13}' >/dev/null; sleep 1
grep -q "systeminfo.txt isn't a game" "$T/daemon.log" || fail "a file that isn't a game was downloaded"
[[ -z "$(find "$T" -name systeminfo.txt -not -path '*/data/*')" ]] || fail "systeminfo.txt reached an emulator's folder"

echo "21. one-file folder games: saves named after the file sync; old extensionless downloads are moved into the folder"
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":70,"platform_id":1,"name":"Folder Game","fs_name":"Folder Game",
  "files":[{"id":701,"file_name":"Folder Game (USA).sfc","content":"ROM:Folder Game (USA).sfc"}]}' >/dev/null
post /api/download '{"rom_id":70}' >/dev/null
for _ in $(seq 1 20); do got=$(find "$T" -path "*/Folder Game/Folder Game (USA).sfc" -not -path '*/data/*' | head -1); [[ -n "$got" ]] && break; sleep 0.25; done
[[ -n "$got" ]] || fail "a one-file folder game must download into its folder"
gdir=$(dirname "$(dirname "$got")")
sdir=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['save_dir'] for m in json.load(sys.stdin) if m['rom_dir']=='$gdir'][0])")
[[ -n "$sdir" ]] || fail "no save folder for $gdir"
mkdir -p "$sdir"; printf 'folder-save' > "$sdir/Folder Game (USA).srm"
syncnow
saved=$(curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; s=[x for x in json.load(sys.stdin)['saves'] if x['rom_id']==70]; print(s[-1]['content'] if s else 'none')")
[[ "$saved" == "folder-save" ]] || fail "a save named after the file inside a one-file folder must sync: $saved"
# What 1.1.1 left behind: the file named after the folder, no extension.
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":71,"platform_id":1,"name":"Bare Game","fs_name":"Bare Game",
  "files":[{"id":711,"file_name":"Bare Game (USA).sfc","content":"ROM:Bare Game (USA).sfc"}]}' >/dev/null
printf 'ROM:Bare Game (USA).sfc' > "$gdir/Bare Game"; printf 'bare-save' > "$sdir/Bare Game.srm"
syncnow
[[ -f "$gdir/Bare Game/Bare Game (USA).sfc" ]] || fail "an extensionless game must be moved into its folder under the file's name"
[[ "$(cat "$sdir/Bare Game (USA).srm" 2>/dev/null)" == "bare-save" && ! -e "$sdir/Bare Game.srm" ]] || fail "its save must take the file's name"

# A save from before RomM's sync slots (no slot) comes down with its game.
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":72,"platform_id":1,"name":"Old Save Game","fs_name":"Old Save Game (USA).sfc",
  "files":[{"id":721,"file_name":"Old Save Game (USA).sfc","content":"ROM:Old Save Game (USA).sfc"}]}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":72,"slot":null,"file_name":"Old Save Game (USA) [2026-08-01].srm","content":"from-before-slots"}' >/dev/null
post /api/download '{"rom_id":72}' >/dev/null
for _ in $(seq 1 40); do [[ -f "$sdir/Old Save Game (USA).srm" ]] && break; sleep 0.25; done
[[ "$(cat "$sdir/Old Save Game (USA).srm" 2>/dev/null)" == "from-before-slots" ]] || fail "a save with no slot must come down with its game"
syncnow
up=$(curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; print(sorted(str(s['slot']) for s in json.load(sys.stdin)['saves'] if s['rom_id']==72))")
[[ "$up" == "['None', 'autosave']" ]] || fail "it must then sync in this console's slot: $up"

echo "22. the sync log and the tracked saves"
ev=$(curl -sf "$API/api/events?limit=500" | python3 -c "
import json,sys; L=json.load(sys.stdin)
print(sorted({(e['kind'], e['action']) for e in L}))
print(next((e['game'] for e in L if e['rom_id']==70 and e['action']=='uploaded'), 'none'))")
grep -q "('save', 'uploaded')" <<<"$ev" && grep -q "('state', 'downloaded')" <<<"$ev" && grep -q "('state', 'skipped')" <<<"$ev" \
  || fail "the sync log is missing kinds of entries: $ev"
[[ "$(tail -1 <<<"$ev")" == "Folder Game" ]] || fail "a log entry must name its game: $ev"
one=$(curl -sf "$API/api/events?rom_id=70" | python3 -c "import json,sys; print({e['rom_id'] for e in json.load(sys.stdin)})")
[[ "$one" == "{70}" ]] || fail "the log for one game holds only that game: $one"
tr=$(curl -sf "$API/api/tracked" | python3 -c "
import json,sys; t=json.load(sys.stdin)
g={x['rom_id']: x for p in t['platforms'] for x in p['games']}
print(t['saves'] > 0, t['states'] > 0, g[70]['name'], [f['status'] for f in g[70]['files']])")
[[ "$tr" == "True True Folder Game ['synced']" ]] || fail "tracked saves by platform: $tr"
printf 'changed-again' > "$sdir/Folder Game (USA).srm"
st=$(curl -sf "$API/api/tracked?rom_id=70" | python3 -c "import json,sys; print([f['status'] for p in json.load(sys.stdin)['platforms'] for g in p['games'] for f in g['files']])")
[[ "$st" == "['changed']" ]] || fail "a save changed here must say so: $st"

echo "23. save sync off: RomM only as a game source"
post /api/config '{"save_sync":false}' >/dev/null
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":73,"platform_id":1,"name":"No Sync Game","fs_name":"No Sync Game (USA).sfc",
  "files":[{"id":731,"file_name":"No Sync Game (USA).sfc","content":"ROM:No Sync Game (USA).sfc"}]}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":73,"slot":"autosave","file_name":"No Sync Game (USA).srm","content":"stays-on-romm"}' >/dev/null
post /api/download '{"rom_id":73}' >/dev/null; post /api/sync >/dev/null; sleep 3
[[ -n "$(find "$T" -name "No Sync Game (USA).sfc" -not -path '*/data/*')" ]] || fail "games must still download with save sync off"
[[ ! -e "$sdir/No Sync Game (USA).srm" ]] || fail "no save may come down with save sync off"
[[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")" == "$n" ]] || fail "no sync may run with save sync off"
grep -q "skipped: save sync is off" "$T/daemon.log" || fail "a skipped sync must say why"
post /api/config '{"save_sync":true}' >/dev/null
syncnow
[[ "$(cat "$sdir/No Sync Game (USA).srm" 2>/dev/null)" == "stays-on-romm" ]] || fail "turned back on, the save must come down"
grep -q "\[NOTIFY\] RommPS: No Sync Game downloaded$" "$T/daemon.log" || fail "a download with save sync off must still say it's done"
grep -qE "\[NOTIFY\] RommPS: ([0-9]+ (saves?|states?) (up|down)(, )?)+$" "$T/daemon.log" || fail "a sync's notification must say what moved"
! grep -q "\[NOTIFY\] RommPS: 0 " "$T/daemon.log" || fail "a notification must not count nothing"

echo "24. save sync per emulator: saves only, then off, then everything"
path_of() { curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['$1'] for m in json.load(sys.stdin) if m['rom_dir']=='$gdir'][0])"; }
emu_root=$(path_of root); stdir=$(path_of state_dir)
[[ -n "$emu_root" && -n "$stdir" ]] || fail "no emulator folders for $gdir"
post /api/config '{"states":"sync"}' >/dev/null
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"saves\"}" >/dev/null || fail "choosing saves only refused"
[[ -z "$(path_of state_dir)" && -n "$(path_of save_dir)" ]] || fail "saves only: states must show as not synced"
curl -sfk -X POST "$TM/_admin/state" -d '{"rom_id":73,"emulator":"snes9x","file_name":"No Sync Game (USA).state1","content":"emu-state"}' >/dev/null
printf 'saves-only' > "$sdir/No Sync Game (USA).srm"
syncnow
[[ ! -e "$stdir/No Sync Game (USA).state1" ]] || fail "saves only: a state came down"
last73() { curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; s=[x for x in json.load(sys.stdin)['saves'] if x['rom_id']==73]; print(s[-1]['content'] if s else 'none')"; }
[[ "$(last73)" == "saves-only" ]] || fail "saves only: the save must still go up: $(last73)"
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"off\"}" >/dev/null
sleep 1.1; printf 'not-synced' > "$sdir/No Sync Game (USA).srm"
syncnow
[[ "$(last73)" == "saves-only" ]] || fail "off: a save went up"
[[ -z "$(path_of save_dir)" ]] || fail "off: saves must show as not synced"
post /api/setup/emulators "{\"roots\":[\"$emu_root\"]}" >/dev/null || true
[[ "$(curl -sf "$API/api/config" | python3 -c "import json,sys; print([p.get('sync','all') for p in json.load(sys.stdin)['profiles'] if p.get('root')=='$emu_root'])")" == *"'off'"* ]] || fail "setting up again must keep the emulator's choice"
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"all\"}" >/dev/null
syncnow
[[ "$(last73)" == "not-synced" ]] || fail "back on: the save must go up: $(last73)"
[[ "$(cat "$stdir/No Sync Game (USA).state1" 2>/dev/null)" == "emu-state" ]] || fail "back on: the state must come down"
curl -s -X POST "$API/api/emulators/sync" -d '{"root":"/nope","sync":"off"}' | grep -q error || fail "an unknown emulator must be refused"

echo "25. a save or state deleted from the console comes back from RomM"
rm -f "$sdir/No Sync Game (USA).srm"
syncnow
[[ "$(cat "$sdir/No Sync Game (USA).srm" 2>/dev/null)" == "not-synced" ]] || fail "a deleted save must come back"
curl -sf "$API/api/events?rom_id=73" | grep -q "Missing here" || fail "the sync log must say why it came back"
rm -f "$stdir/No Sync Game (USA).state1"
syncnow
[[ "$(cat "$stdir/No Sync Game (USA).state1" 2>/dev/null)" == "emu-state" ]] || fail "a deleted state must come back"

echo "PASS"
