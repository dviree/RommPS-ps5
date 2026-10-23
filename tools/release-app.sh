#!/usr/bin/env bash
# Builds RommPS for a release here, and attaches PPSA76677.zip to its GitHub release.
#
#   tools/release-app.sh v1.1.0            build and package it, in build/release/
#   tools/release-app.sh v1.1.0 --upload   and attach it to the release v1.1.0
#
# The release workflow builds and attaches the zip itself (.github/workflows/app.yml);
# a PS5 release isn't published without it. This is the fallback for a release
# that went out without one anyway, which is how the store sat on 1.2.1 for a
# day after 1.3.0. The homebrew catalog picks the release up from the asset's
# name, so it stays PPSA76677.zip, and an asset already published is never replaced.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:?usage: tools/release-app.sh vX.Y.Z [--upload]}"
die() { echo "release-app: $*" >&2; exit 1; }

cd "$ROOT"
git diff --quiet && git diff --cached --quiet || die "commit or stash your changes first"
[[ "$(git rev-parse HEAD)" == "$(git rev-parse "$TAG^{commit}" 2>/dev/null)" ]] || die "check out $TAG first"
if [[ "${2:-}" == --upload ]] && gh release view "$TAG" --json assets -q '.assets[].name' | grep -qx PPSA76677.zip; then
    die "$TAG already has PPSA76677.zip; publish a new release instead of replacing it"
fi

# RommPS carries the payload and starts it when it isn't running: the
# release's own romm-sync.elf, the same file the in-app updater installs.
mkdir -p build/ps5
gh release download "$TAG" -p romm-sync.elf -O build/ps5/romm-sync.elf --clobber
(cd build/ps5 && gh release download "$TAG" -p SHA256SUMS -O - | grep " romm-sync.elf$" | shasum -a 256 -c -) ||
    die "build/ps5/romm-sync.elf isn't $TAG's"

tools/package-app.sh "$TAG"

if [[ "${2:-}" == --upload ]]; then
    gh release upload "$TAG" build/release/PPSA76677.zip
    local_sum=$(shasum -a 256 build/release/PPSA76677.zip | cut -d' ' -f1)
    remote_sum=$(gh release download "$TAG" -p PPSA76677.zip -O - | shasum -a 256 | cut -d' ' -f1)
    [[ "$local_sum" == "$remote_sum" ]] || die "the PPSA76677.zip on $TAG isn't the one built here"
    echo "$TAG has PPSA76677.zip ($local_sum). The catalog bot proposes it within 8 hours;"
    echo "for sooner, open the update yourself (docs/release-checklist.md)."
fi
