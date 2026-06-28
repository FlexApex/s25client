#!/usr/bin/env bash
# Copyright (C) 2005 - 2026 Settlers Freaks <sf-team at siedler25.org>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run a single ai-battle game and collect ALL its artifacts under one folder in
#   <repo>/ai-battle-runs/games/<timestamp>_<map>/
# so ad-hoc runs are stored as systematically as eval.py runs (see ai-battle-runs/README.md).
#
# Usage:
#   tools/ai-eval/run-game.sh --map <map> --ai <a> --ai <b> [more ai-battle args...]
# Example:
#   tools/ai-eval/run-game.sh --map share/s25rttr/RTTR/MAPS/NEW/dreamland.swd \
#       --ai aijh --ai aijh --maxGF 200000 --inexhaustibleMines --goldDeposits 4
#
# This script supplies --stats/--replay/--save pointing into the run folder, so do NOT pass those
# yourself. All other ai-battle options are forwarded unchanged.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN="$REPO_ROOT/build/bin/ai-battle"
GAMES_DIR="$REPO_ROOT/ai-battle-runs/games"

if [ ! -x "$BIN" ]; then
    echo "ai-battle binary not found: $BIN" >&2
    echo "Build it: (cd build && make ai-battle -j)" >&2
    exit 1
fi

# Derive a label from the --map argument (basename without extension); default "game".
map_label="game"
prev=""
for a in "$@"; do
    if [ "$prev" = "--map" ] || [ "$prev" = "-m" ]; then
        map_label="$(basename "$a")"
        map_label="${map_label%.*}"
    fi
    prev="$a"
done

run_dir="$GAMES_DIR/$(date +%Y%m%d-%H%M%S)_${map_label}"
mkdir -p "$run_dir"

echo "ai-battle run -> $run_dir"
# Run, forwarding the user's args plus our managed output paths; tee the console to game.log.
set +e
"$BIN" "$@" \
    --stats "$run_dir/stats.csv" \
    --replay "$run_dir/replay.rpl" \
    --save "$run_dir/save.sav" \
    2>&1 | tee "$run_dir/game.log"
status=${PIPESTATUS[0]}
set -e

echo
echo "Artifacts in: $run_dir"
ls -1 "$run_dir" | sed 's/^/  /'
exit "$status"
