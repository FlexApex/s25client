#!/usr/bin/env bash
# Run an ai-battle game with the LLM-driven AI: starts the sidecar, runs the game, cleans up.
#
# Usage:
#   extras/ai-battle/run_llm_game.sh <map.SWD> [extra ai-battle args...]
#
# Defaults to a 2v2 on the user's ruleset (inexhaustible mines, gold->granite) with the LLM team
# (players 0,2) vs the heuristic AIJH (players 1,3). Set BLOCK_MS=25000 for synchronous, reproducible
# LLM-in-the-loop runs; leave it 0 (default) for fast async play. Config comes from .env (LLM_URL,
# LLM_APIKEY, LLM_MODEL). Run from the repo root.
set -euo pipefail

MAP="${1:?usage: run_llm_game.sh <map.SWD> [extra args...]}"
shift || true

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SPOOL="${SPOOL:-/tmp/rttr_llm_$$}"
BLOCK_MS="${BLOCK_MS:-0}"
MAXGF="${MAXGF:-200000}"
STATS="${STATS:-/tmp/llm_game.csv}"
rm -rf "$SPOOL"; mkdir -p "$SPOOL"

echo ">> verifying LLM endpoint..."
python3 extras/ai-battle/llm_sidecar.py --selftest

echo ">> starting sidecar on $SPOOL"
python3 extras/ai-battle/llm_sidecar.py --spool "$SPOOL" &
SIDE=$!
trap 'kill $SIDE 2>/dev/null || true; rm -rf "$SPOOL"' EXIT

echo ">> running game (BLOCK_MS=$BLOCK_MS, maxGF=$MAXGF)"
RTTR_RTTR_DIR="$ROOT/data/RTTR" RTTR_LLM_SPOOL="$SPOOL" RTTR_LLM_BLOCK_MS="$BLOCK_MS" \
  build/bin/ai-battle -m "$MAP" \
  --ai llm --ai aijh --ai llm --ai aijh \
  --teams "0,2;1,3" --inexhaustibleMines --goldDeposits 4 \
  --maxGF "$MAXGF" --stats "$STATS" --statsInterval 25000 \
  "$@"

echo ">> done. stats: $STATS"
