#!/usr/bin/env bash
# PS5 Vulkan Template - the UI kit this title builds with, at a pinned revision.
#
#   ps5/ui/setup-kit.sh       export it into .deps/hui (once a revision); prints the path
#
# The kit's own fonts are baked with printable ASCII and a handful of symbols;
# RommPS shows names it didn't write (Pokémon, Luigi’s Mansion), so the export
# bakes them again from the same TTFs with the kit's "european" glyph set
# (accented Latin, Cyrillic, curly quotes, euro, trade mark). Same faces, same
# sizes and SDF ranges as the kit's tools/bake-fonts.sh; only the set and the
# atlas (2048, which the bigger set needs) differ.
#
# The kit is PS5_VKHomebrewUI, my fork of BlackBearReloaded's ps5-homebrew-ui
# with a Vulkan backend. The pinned revision is exported with git archive from
# the sibling checkout (../PS5_VKHomebrewUI, or PS5_VKHOMEBREWUI), or from
# GitHub when there is none, so a build never depends on a working tree.
# ps5/ui/ui.cmake runs this when the build is configured.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
kit="$root/.deps/hui"
fork="${PS5_VKHOMEBREWUI:-$root/../PS5_VKHomebrewUI}"
# 2188642: the Vulkan backend, with import_texture (a program's own image)
revision=2188642afbcc8d84d36149124f3d9e39dc1b1a05

# What is exported, beside the revision: a change to the list exports again
stamp="$revision src assets/fonts assets/audio third_party/fonts fonts:european-2048"
if [[ ! -f $kit/.revision || $(<"$kit/.revision") != "$stamp" ]]; then
    if ! git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null; then
        fork="$root/.deps/PS5_VKHomebrewUI.git"
        [[ -d $fork ]] || git clone --quiet --bare https://github.com/mihawk-99/PS5_VKHomebrewUI.git "$fork" >&2
        git -C "$fork" fetch --quiet origin "$revision" >&2 || true
        git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null || {
            echo "PS5_VKHomebrewUI $revision is neither in ../PS5_VKHomebrewUI nor on GitHub" >&2
            exit 2
        }
    fi
    rm -rf -- "$kit"
    mkdir -p "$kit"
    # The code, the assets a title ships (the TTFs are for the samples' ImGui
    # windows, ps5/ui/overlay_theme.cpp, and the font bake), the font baker,
    # and the licence texts
    git -C "$fork" archive "$revision" src assets/fonts assets/audio third_party/fonts \
        third_party/stb/stb_truetype.h tools/font-baker LICENSE THIRD_PARTY_NOTICES.md | tar -x -C "$kit"
    # A host tool: the build machine's compiler, not the console's. The glyphs
    # a face lacks are skipped (Press Start 2P has few beyond ASCII); the app
    # asks the font before drawing one (has_glyph).
    cxx=$(command -v "${HOST_CXX:-clang++}" || command -v c++) || { echo "no host C++ compiler to bake the fonts" >&2; exit 2; }
    mkdir -p "$kit/build"
    "$cxx" -std=c++20 -O2 -w "$kit/tools/font-baker/bake_font.cpp" -o "$kit/build/bake_font" >&2
    while read -r source output size range; do
        "$kit/build/bake_font" "$kit/third_party/fonts/$source" "$kit/assets/fonts/$output.huifont" \
            "$size" "$range" 2048 european 2>&1 | grep -v '^skipping U+' >&2
    done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8
Inter-SemiBold.ttf inter-semibold 56 8
Montserrat-Medium.ttf montserrat-medium 56 8
DejaVuSansMono.ttf dejavu-sans-mono 52 8
PressStart2P-Regular.ttf press-start-2p 32 4
PatrickHand-Regular.ttf patrick-hand 56 8
FONTS
    echo "$stamp" > "$kit/.revision"
fi
echo "$kit"
