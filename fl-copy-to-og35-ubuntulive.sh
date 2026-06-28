#!/usr/bin/env bash
#
# Rebuild + sync the RTTR game + the LLM-AI opponent to the remote test machine.
#
#   Usage:  ./fl-copy-to-og35-ubuntulive.sh
#           BUILD=0 ./fl-copy-to-og35-ubuntulive.sh    # skip the rebuild, sync only
#
# 1. Rebuilds build/ incrementally (no-op / seconds if the tree is already fresh).
# 2. rsyncs the runnable parts of build/ (bin + drivers + data) plus the LLM-AI
#    opponent's runtime (Python sidecar, .env credentials, launcher).
#
# rsync transfers only changed files, so large unchanging data (maps/worlds in
# build/share) is NOT re-sent on every run — only the relinked binaries move.
# build/libs (~112 MB of object files) is never copied; it is not needed to run.
#
# NOTE: the binary was compiled on Debian 13. The target must have glibc >= 2.38,
# Boost 1.83, lua5.3, libminiupnpc18, libbz2, libsamplerate0, and SDL2 + SDL2_mixer.
# If sonames mismatch (e.g. Ubuntu 22.04 / older Boost), rebuild from source there.
# The LLM sidecar needs python3 (stdlib only) and network access to LLM_URL.

set -euo pipefail

HOST="ubuntu@og35-ubuntulive"
HOST="ubuntu@ubuntu"
PORT=18332
REMOTE_DIR='~/rttr'        # game lands here; run from this dir on the target

# rsync over the non-standard ssh port; -a preserves perms/mtimes (so the
# size+mtime skip works), -z compresses on the wire, progress2 shows a total.
RSYNC=(rsync -ah -z --info=progress2 -e "ssh -p $PORT")

# run relative to the repo root (this script's location) so build/* resolves
cd "$(dirname "$0")"

if [[ "${BUILD:-1}" != 0 ]]; then
    echo ">> rebuilding (incremental — fast/no-op if the tree is already up to date) ..."
    cmake --build build -j"$(nproc)" --target s25client ai-battle
fi

echo ">> creating $REMOTE_DIR on $HOST ..."
ssh -p "$PORT" "$HOST" "mkdir -p $REMOTE_DIR"

echo ">> syncing game (bin + lib + share; unchanged files skipped) ..."
"${RSYNC[@]}" build/bin build/lib build/share "$HOST:$REMOTE_DIR/"

echo ">> syncing LLM AI opponent (sidecar + credentials) ..."
# The sidecar searches for .env in its CWD, its own dir, and ../.. — so placing
# llm_sidecar.py and .env together in $REMOTE_DIR lets it find the creds itself.
# WARNING: .env holds the LLM API key, so this ships a secret to the target host.
"${RSYNC[@]}" extras/ai-battle/llm_sidecar.py .env "$HOST:$REMOTE_DIR/"
"${RSYNC[@]}" .env.nvidia-input "$HOST:$REMOTE_DIR/" || echo "   (.env.nvidia-input absent, skipped)"

echo ">> uploading target-tailored LLM launcher (run_llm_game.sh) ..."
# Built for the flat ~/rttr layout: ./bin/ai-battle, sidecar+.env alongside it,
# and the binary finds RTTR data via its own prefix (no RTTR_RTTR_DIR needed).
LAUNCH="$(mktemp)"
cat > "$LAUNCH" <<'REMOTE_EOF'
#!/usr/bin/env bash
# Run a headless ai-battle with the LLM AI (LLM team vs heuristic AIJH).
#   ./run_llm_game.sh <map.swd> [extra ai-battle args...]
# BLOCK_MS=25000 -> synchronous, reproducible LLM-in-the-loop; 0 (default) -> async.
# Without a reachable model it falls back to the heuristic strategist and still plays.
set -euo pipefail
MAP="${1:?usage: ./run_llm_game.sh <map.swd> [extra ai-battle args...]}"; shift || true
cd "$(dirname "$0")"                       # ~/rttr : has bin/, llm_sidecar.py, .env
SPOOL="${SPOOL:-/tmp/rttr_llm_$$}"
BLOCK_MS="${BLOCK_MS:-0}"; MAXGF="${MAXGF:-200000}"; STATS="${STATS:-/tmp/llm_game.csv}"
rm -rf "$SPOOL"; mkdir -p "$SPOOL"
echo ">> verifying LLM endpoint..."; python3 llm_sidecar.py --selftest
echo ">> starting sidecar on $SPOOL"; python3 llm_sidecar.py --spool "$SPOOL" & SIDE=$!
trap 'kill $SIDE 2>/dev/null || true; rm -rf "$SPOOL"' EXIT
echo ">> running game (BLOCK_MS=$BLOCK_MS, maxGF=$MAXGF)"
RTTR_LLM_SPOOL="$SPOOL" RTTR_LLM_BLOCK_MS="$BLOCK_MS" \
  ./bin/ai-battle -m "$MAP" --ai llm --ai aijh --ai llm --ai aijh \
  --teams "0,2;1,3" --inexhaustibleMines --goldDeposits 4 \
  --maxGF "$MAXGF" --stats "$STATS" --statsInterval 25000 "$@"
echo ">> done. stats: $STATS"
REMOTE_EOF
"${RSYNC[@]}" "$LAUNCH" "$HOST:$REMOTE_DIR/run_llm_game.sh"
ssh -p "$PORT" "$HOST" "chmod +x $REMOTE_DIR/run_llm_game.sh"
rm -f "$LAUNCH"

echo ">> NOT copying profile (~/.s25rttr) ..."
#"${RSYNC[@]}" "$HOME/.s25rttr" "$HOST:~/"
# The 2v2 launcher needs a 4-player map; your custom ones live in the profile.
# Uncomment to sync just your maps (incl. Macro144.SWD) without the savegames:
#"${RSYNC[@]}" "$HOME/.s25rttr/MAPS/" "$HOST:$REMOTE_DIR/maps/"

cat <<EOF

Done.

Run the GUI game (NOT via start.sh, which would try to 'make'):
    ssh -p $PORT $HOST
    cd ${REMOTE_DIR/#\~/\$HOME} && ./bin/s25client

Test the LLM AI opponent headlessly (from ${REMOTE_DIR/#\~/\$HOME}):
    python3 llm_sidecar.py --selftest                 # check endpoint (never prints the key)
    # quick 1v1 on a shipped map:
    ./bin/ai-battle -m share/s25rttr/RTTR/MAPS/NEW/WAGE_0_3.SWD --ai llm --ai aijh --maxGF 5000
    # full 2v2 LLM-in-the-loop on your ruleset (needs a 4-player map, e.g. maps/Macro144.SWD):
    ./run_llm_game.sh maps/Macro144.SWD
  Heuristic-only (no model, no network, deterministic): just omit the sidecar / RTTR_LLM_SPOOL.

Troubleshooting:
  - binary won't start:   ldd ./bin/s25client            (any 'not found' -> apt install / rebuild)
  - black window / GLXBadContextTag (as on Qubes): prepend SDL_VIDEO_X11_FORCE_EGL=1
    (add LIBGL_ALWAYS_SOFTWARE=1 only if the machine has no GPU)
  - sidecar selftest fails on SSL: apt install ca-certificates ; needs network to LLM_URL
EOF
