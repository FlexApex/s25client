# replay-verify

A **headless replay checker** for Return To The Roots. It plays a recorded
replay (`.rpl`) back through the real game engine — exactly the way the GUI
client does in replay mode, but with no window, no rendering and no input — and
reports whether the replay stays *in sync* with the match it was recorded from.

Its main use is catching **replay desyncs** (and, by extension, simulation
non-determinism) in continuous, scriptable runs: regression tests, CI, and
debugging AI-battle recordings without launching `s25client`.

## What "in sync" means

While recording, the engine periodically stores an `AsyncChecksum` — a small
fingerprint of the simulation state (the RNG checksum plus live-object,
object-id and event counters) — alongside each batch of game commands. On
playback the engine re-executes those exact commands and recomputes the
checksum at the same game frame (GF). If the recomputed checksum ever differs
from the stored one, the replay has *desynced*: the simulation no longer
reproduces the original match. `replay-verify` performs that comparison and
tells you the first GF where it breaks, plus a per-field breakdown.

This mirrors what `GameClient` does in replay mode
(`GameClientGF_Replay.cpp`), so a desync seen here is the same one a user sees
in the GUI ("The played replay is not in sync with the original match").

## Building

It is built as part of the normal CMake build (it lives under `extras/`):

```sh
cmake -S . -B build
cmake --build build --target replay-verify
```

The binary lands at `build/bin/replay-verify`.

## Running

The engine needs to find its game data (`RTTR`) and a writable user-data
directory (for logs). When running from a build tree that has not been
`install`ed, point both at real locations via environment variables:

```sh
RTTR_RTTR_DIR="$PWD/data/RTTR" \
RTTR_USERDATA_DIR="$HOME/.s25rttr" \
  build/bin/replay-verify path/to/game.rpl
```

(If you run an installed build, the defaults already resolve and no env vars
are needed.)

Exit code: `0` if in sync, `2` if a desync was detected, `1` on usage/load
errors.

### Example output

In sync:

```
Replay: "game.rpl" seed=42 players=4 lastGF=100000 minorVer=3 savegame=0
IN SYNC: replay verified to lastGF=100000
```

Desync:

```
Replay: "game.rpl" seed=42 players=4 lastGF=100000 minorVer=3 savegame=0
FIRST obj divergence at GF 88640 (player 2): objCt 10160:10154 objIdCt 39846:39840 rand 3488482749:3488482749 (match=0)
ASYNC at GF 88640 (player 2): orig:replay
  rand        3488482749:3488482749
  objCt       10160:10154
  objIdCt     39846:39840
  eventCt     2732:2732
  evInstanceCt 1441827:1441827
DESYNC: firstAsyncGF=88640 totalAsyncFrames=724
```

All paired numbers are printed as **`original:replay`** — i.e. the value stored
while recording vs. the value recomputed during this playback.

## Reading the checksum signature

The *which field diverged* tells you *what kind* of non-determinism you are
dealing with:

| Field that first differs | Meaning |
| --- | --- |
| `rand` (randChecksum) | An RNG-consuming code path ran a different number of times / in a different order. The whole simulation has diverged. |
| `objCt` / `objIdCt` only (rand matches) | A non-RNG divergence: different game objects were created/destroyed even though every random draw matched. Usually a *setup* mismatch between the recorder and the replay engine (see below). |
| `eventCt` / `evInstanceCt` | The scheduled-event population differs. |

`objCt` is the number of live objects; `objIdCt` is the running total of objects
ever created (monotonic). Note that `AsyncChecksum` is a small aggregate, so the
*first detected* divergence can be a few GFs after the *actual* root cause —
the real cause lies between the previous in-sync command-GF and the reported
one.

## Recorder vs. replay setup (a common desync cause)

A recording and its playback must initialise the world **identically**.
`GameClient` (and this tool) build a fresh map game as:

```
MapLoader::Load → MapLoader::SetupResources → GamePlayer::MakeStartPacts → InitAfterLoad
```

If the program that *recorded* the replay skipped one of these steps, playback
diverges even though the replay file is perfectly valid. Two historically real
cases:

- **`SetupResources` skipped** → mine/water/fish resources differ → shows up as
  a `rand` divergence.
- **`MakeStartPacts` skipped** → team members are not allied in the recording
  but are during playback → combat/territory plays out differently → shows up as
  an `objCt`/`objIdCt` divergence with `rand` still matching.

**Always check the replay's embedded build revision first.** A desync may simply
mean the replay is *stale* — recorded by an older binary before such a fix
landed. The revision is stored in the file header right after the `RTTRRP2`
signature and the two version bytes:

```sh
od -c game.rpl | head -1
```

If that revision predates the fix, re-record with the current binary rather
than hunting for a non-determinism that no longer exists.

## Diagnostic environment switches

These exist to *localise* a desync (they are off by default):

| Variable | Effect |
| --- | --- |
| `RTTR_VERIFY_TRACE` | Print every checksum comparison (one line per command-GF), marking the ones that differ. Useful to bound exactly when divergence starts. |
| `RTTR_VERIFY_RUN_AI` | Also construct and run the AI players each GF (their generated commands are discarded — the replay still drives the world). Tests whether merely *running the AI* during playback changes the result, i.e. whether the recorder's AI presence is the cause. |
| `RTTR_VERIFY_NO_SETUPRES` | Skip `MapLoader::SetupResources`, to mimic a recorder that didn't call it. |
| `RTTR_VERIFY_NO_PACTS` | Skip `GamePlayer::MakeStartPacts`, to mimic a recorder that didn't call it. |

Typical workflow: if a replay desyncs with the default (GameClient-equivalent)
setup, re-run with `RTTR_VERIFY_NO_PACTS=1` and/or `RTTR_VERIFY_NO_SETUPRES=1`.
If one of those makes it go in sync, the desync is purely that setup mismatch
in the recorder — fix the recorder (or re-record) rather than the engine.

## Relationship to the autoplay test

`tests/s25Main/autoplay` already replays bundled `.rpl` files and asserts they
stay in sync, as a determinism regression test. `replay-verify` is the
standalone, ad-hoc counterpart: point it at *any* replay on disk, get a precise
desync report, and toggle the setup switches above — without writing a test
case.
