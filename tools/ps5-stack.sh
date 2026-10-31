#!/usr/bin/env bash
# Checks out the PS5 stack RommPS builds against, at the commits a release
# uses, for the release workflow (or anyone without ../ps5-stack):
#
#   tools/ps5-stack.sh <dir>     PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK in <dir>
#
# PS5_Vulkan is pinned here. Everything else is pinned where it's used, and
# read from there so there's one place to bump: Mesa by PS5_Vulkan's
# tools/build-radv.sh, the payload SDK by PS5_Vulkan's
# tools/setup-native-dependencies.sh and by RommPS's ps5/tools/setup-sdk.sh.
# Every script that uses them does `git archive <pin>`, so only those commits
# are fetched, not the histories (Mesa's alone is a gigabyte).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STACK="${1:?usage: tools/ps5-stack.sh <dir>}"

# 5b5e4fc: what 1.3.0 was built with. RADV comes out of this one's pin of PS5_Mesa.
vulkan_revision=5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5

# fetch <repo> <commit>...: a repository holding just those commits, the first checked out
fetch() {
    local repo="$1" dir="$STACK/$1"
    shift
    [[ -d $dir/.git ]] || git init -q "$dir"
    git -C "$dir" remote get-url origin >/dev/null 2>&1 ||
        git -C "$dir" remote add origin "https://github.com/mihawk-99/$repo.git"
    git -C "$dir" fetch -q --depth 1 origin "$@"
    git -C "$dir" -c advice.detachedHead=false checkout -q "$1"
    echo "$repo at ${1:0:11}"
}
pin() { sed -n "s/^$2=//p" "$1"; }

fetch PS5_Vulkan "$vulkan_revision"
fetch PS5_Mesa "$(pin "$STACK/PS5_Vulkan/tools/build-radv.sh" mesa_revision)"
fetch PS5_PayloadSDK "$(pin "$ROOT/platform/ps5/app/rommps/ps5/tools/setup-sdk.sh" sdk_revision)" \
    "$(pin "$STACK/PS5_Vulkan/tools/setup-native-dependencies.sh" sdk_revision)"
