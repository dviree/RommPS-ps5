#!/usr/bin/env bash
# Builds RommPS from scratch and packages it as build/release/PPSA76677.zip,
# the file ProsperoStore installs.
#
#   tools/package-app.sh v1.4.0
#
# Used by the release workflow (.github/workflows/app.yml) and by
# tools/release-app.sh. It expects build/ps5/romm-sync.elf to be the payload
# of that same release, since RommPS carries it and starts it, and the title
# art (pic0/pic1.dds, from platform/ps5/app/tools/title-art.sh) in place.
# The PS5 stack is PS5_STACK, or ../ps5-stack (tools/ps5-app-build.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/platform/ps5/app/rommps"
TAG="${1:?usage: tools/package-app.sh vX.Y.Z}"
die() { echo "package-app: $*" >&2; exit 1; }

# The catalog tells consoles about an update from contentVersion, so it must
# match the tag: v1.1.0 is 01.001.000.
[[ "$TAG" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "$TAG isn't a vX.Y.Z tag"
want=$(printf "%02d.%03d.%03d" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}")
have=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentVersion"])' "$APP/ps5/sce_sys/param.json")
[[ "$have" == "$want" ]] || die "contentVersion is $have, $TAG needs $want (ps5/sce_sys/param.json)"
grep -q "kMinPayload = \"${TAG#v}\"" "$APP/examples/rommps/kit/rommps_app.hpp" ||
    die "kMinPayload in rommps_app.hpp isn't ${TAG#v}"
for art in pic0 pic1; do
    [[ -s "$APP/ps5/sce_sys/$art.dds" ]] || die "$art.dds is missing: run platform/ps5/app/tools/title-art.sh"
done
[[ -s "$ROOT/build/ps5/romm-sync.elf" ]] || die "build/ps5/romm-sync.elf is missing"

# From scratch: a dev build left behind gives ninja nothing to do, and its
# notices still say "uncommitted changes", so the check below refuses it.
rm -rf "$APP/build/ps5" "$APP/dist/PPSA76677"
"$ROOT/tools/ps5-app-build.sh" bash -c 'cd /repo/platform/ps5/app/rommps && bash ps5/tools/build.sh rommps'
# The notices name the commit of every part; one built with uncommitted changes
# wouldn't be the source they point to.
dirty=$(python3 -c 'import json,sys; print(" ".join(p["id"] for p in json.load(open(sys.argv[1])) if p["source"].get("dirty")))' \
    "$APP/dist/PPSA76677/licenses/components.json")
[[ -z "$dirty" ]] || die "built from uncommitted changes in: $dirty"
cmp -s "$ROOT/build/ps5/romm-sync.elf" "$APP/dist/PPSA76677/assets/rommps/romm-sync.elf" ||
    die "the romm-sync.elf in RommPS isn't build/ps5/romm-sync.elf"

mkdir -p "$ROOT/build/release"
rm -f "$ROOT/build/release/PPSA76677.zip"
(cd "$APP/dist" && zip -qrX "$ROOT/build/release/PPSA76677.zip" PPSA76677 -x 'PPSA76677/screenshots/*')
(cd "$ROOT" && shasum -a 256 build/release/PPSA76677.zip)
