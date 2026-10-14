#!/usr/bin/env bash
# Upgrade test: each released version sets up and syncs a console against
# tests/mock_romm.py, then this build takes over its data folder. Nothing may
# move that didn't change, the settings and emulators stay, and syncing goes on.
#   make host && tests/upgrade.sh [old-binary ...]
# With no arguments, the releases are built from their tags first (git worktrees
# in build/upgrade/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NEW="$ROOT/build/host/romm-sync"
MOCK_PORT=8897 WEB_PORT=8783
API="http://127.0.0.1:${WEB_PORT}"

olds=("$@")
if [[ ${#olds[@]} -eq 0 ]]; then
    mkdir -p "$ROOT/build/upgrade"
    for t in $(git -C "$ROOT" tag --sort=version:refname | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$'); do
        w="$ROOT/build/upgrade/$t"
        [[ -d "$w" ]] || git -C "$ROOT" worktree add -q --detach "$w" "$t"
        [[ -x "$w/build/host/romm-sync" ]] || make -C "$w" host >/dev/null
        olds+=("$w/build/host/romm-sync")
    done
fi

T=""
cleanup() { kill $(jobs -p) 2>/dev/null || true; if [[ -n "$T" ]]; then rm -rf "$T"; fi; }
trap cleanup EXIT
fail() { echo "FAIL ($label): $*"; echo "--- log"; tail -40 "$T/daemon.log"; exit 1; }
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
status() { curl -sf "$API/api/status"; }
count() { status | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']; print(s['sync_count'] if not s['running'] else -1)"; }
syncnow() {
    local n; n=$(count); post /api/sync >/dev/null
    for _ in $(seq 1 60); do [[ "$(count)" -gt "$n" ]] && return 0; sleep 0.5; done; fail "the sync didn't finish"
}
last() { status | python3 -c "import json,sys; h=json.load(sys.stdin)['sync']['history'][-1]; print(h.get('$1', h.get('error','')))"; }
up() { for _ in $(seq 1 40); do curl -sf "$API/api/status" >/dev/null 2>&1 && return 0; sleep 0.25; done; fail "the web UI didn't come up"; }
dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
snapshot() { # every save and state here, with its hash
    (cd "$RA" && find saves states roms -type f -exec md5sum {} + 2>/dev/null | sort)
}
start() { # binary
    ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/none" ROMM_SYNC_AUTOSTART="$T/autostart" ROMM_SYNC_APP_DIRS="$T/none" \
      ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_FS_ROOTS="$T" "$1" >>"$T/daemon.log" 2>&1 &
    PID=$!
    up
}

for OLD in "${olds[@]}"; do
    label="$(basename "$(dirname "$(dirname "$(dirname "$OLD")")")")" # build/upgrade/<tag>/build/host/romm-sync
    T="$(mktemp -d)"
    python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
    MOCK=$!
    sleep 0.5
    RA="$T/RetroArch"; mkdir -p "$RA/roms/snes" "$RA/saves/Snes9x" "$RA/states/Snes9x" "$T/data" "$T/autostart" "$T/none"
    echo '[]' > "$T/catalog.json"
    printf 'savefile_directory = ":/saves"\nsort_savefiles_enable = "true"\nsavestate_directory = ":/states"\nsort_savestates_enable = "true"\n' > "$RA/retroarch.cfg"
    cat > "$T/data/config.json" <<EOF
{"web_port": $WEB_PORT, "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": true, "conflict_policy": "ask",
 "states": "sync", "notify": false, "keep_backups": 3,
 "profiles": [{"id":"retroarch","name":"RetroArch","enabled":true,"root":"$RA",
   "rom_dir":"{root}/roms/{platform}","save_dir":"{root}/saves","state_dir":"{root}/states",
   "bios_dir":"{root}/system","save_exts":".srm","state_exts":".state*","retroarch_cfg":"{root}/retroarch.cfg",
   "platforms":[{"romm":["snes"],"dir":"snes","emulator":"snes9x","core_name":"Snes9x"},
     {"romm":["ngc"],"dir":"ngc","emulator":"dolphin","core_name":"Dolphin","save_layout":"gci"}]}]}
EOF
    echo "ROM" > "$RA/roms/snes/Chrono Trigger (USA).sfc"
    echo "local-save-v1" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
    printf 'local-state-1' > "$RA/states/Snes9x/Chrono Trigger (USA).state1"
    echo "ROM" > "$RA/roms/snes/Super Metroid (USA).sfc"
    # GameCube: a game with a .gci save (synced as a zip), from RomM's catalog of the mock.
    mkdir -p "$RA/roms/ngc" "$RA/saves/dolphin-emu/User/GC/USA/Card A"
    python3 "$ROOT/tests/make_disc.py" gc "$RA/roms/ngc/Wind Waker (USA).iso" GZLE01
    python3 "$ROOT/tests/gccard.py" gci "$RA/saves/dolphin-emu/User/GC/USA/Card A/01-GZLE-gczelda.gci" GZLE01 gczelda 1 0x10
    # A state on RomM the old version brings down.
    curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"emulator":"snes9x","file_name":"Super Metroid (USA).state1","content":"srv-state"}' >/dev/null
    curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null

    # The old version: paired, set up, synced twice.
    start "$OLD"
    post /api/pair/start "{\"server_url\":\"http://127.0.0.1:$MOCK_PORT\"}" >/dev/null || fail "pairing refused"
    for _ in $(seq 1 20); do [[ "$(status | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
    post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null 2>&1 || true
    syncnow; syncnow
    [[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm" 2>/dev/null)" == "server-save" ]] || fail "the old version didn't sync (setup)"
    [[ -n "$(find "$RA/states" -name 'Super Metroid*' 2>/dev/null)" ]] || fail "the old version didn't bring the state down (setup)"
    dump | python3 -c "import json,sys; sys.exit(0 if any(s['rom_id']==40 for s in json.load(sys.stdin)['saves']) else 1)" ||
        fail "the old version didn't send the GameCube save (setup)"
    before=$(snapshot)
    saves_before=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d['saves']), len(d['states']))")
    kill "$PID"; wait "$PID" 2>/dev/null || true

    # This build takes over.
    start "$NEW"
    for _ in $(seq 1 40); do [[ "$(count)" -ge 1 ]] && break; sleep 0.5; done # its startup sync
    syncnow
    v=$(status | python3 -c "import json,sys; print(json.load(sys.stdin)['version'])")
    [[ "$(snapshot)" == "$before" ]] || fail "files here changed on upgrade: $(diff <(echo "$before") <(snapshot) | head -5)"
    [[ "$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d['saves']), len(d['states']))")" == "$saves_before" ]] ||
        fail "RomM got something new on upgrade"
    h=$(status | python3 -c "import json,sys; print([(h['reason'], h['uploaded'], h['downloaded'], h['conflicts'], h['states_uploaded'], h['states_downloaded']) for h in json.load(sys.stdin)['sync']['history'][-2:]])")
    [[ "$h" != *", 1"* && "$h" != *"conflict"* ]] || true
    python3 - "$h" <<'PY' || fail "a sync after the upgrade moved something: $h"
import ast, sys
for reason, up, down, conflicts, sup, sdown in ast.literal_eval(sys.argv[1]):
    assert up == down == conflicts == sup == sdown == 0
PY
    cfg=$(curl -sf "$API/api/config" | python3 -c "
import json,sys; c=json.load(sys.stdin)
print(c['conflict_policy'], c['states'], c['save_sync'], c['notify'], c['keep_backups'], [p['id'] for p in c['profiles']])")
    [[ "$cfg" == "ask sync True False 3 ['retroarch']" ]] || fail "settings changed on upgrade: $cfg"
    tr=$(curl -sf "$API/api/tracked" | python3 -c "
import json,sys; t=json.load(sys.stdin)
print(sorted((f['status']) for p in t['platforms'] for g in p['games'] for f in g['files']))")
    [[ "$tr" != *"changed"* && "$tr" != *"conflict"* && "$tr" == *"synced"* ]] || fail "tracked saves after the upgrade: $tr"
    curl -sf "$API/api/events" | python3 -c "import json,sys; assert isinstance(json.load(sys.stdin), list)" || fail "no sync log"
    # And it keeps syncing.
    sleep 1.1; echo "local-save-v2" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
    syncnow
    [[ "$(last uploaded)" == 1 ]] || fail "a changed save after the upgrade must go up"
    echo "ok   $label -> $v"
    kill "$PID" "$MOCK" 2>/dev/null || true; wait 2>/dev/null || true
    rm -rf "$T"; T=""
done
echo "PASS"
