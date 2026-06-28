# ai-eval — AI evaluation harness

Tooling to measure how strong a Return-to-the-Roots AI is by playing many headless `ai-battle` games
against a baseline and reporting a statistically meaningful win record. It is **AI-agnostic**: it
evaluates whatever AIs `ai-battle --ai` accepts (today `aijh` and `dummy`; register your own to test it).

## Quick start

```sh
# build the headless runner
cd build && make ai-battle -j

# from the repo root: sanity check (identical AIs -> ~50%)
tools/ai-eval/eval.py

# evaluate your AI against AIJH over 8 seeds (48 games)
tools/ai-eval/eval.py --challenger myai --baseline aijh --num-seeds 8
```

## How it scores fairly

The eval maps are **symmetric** and each 1v1 uses two **fixed start positions**. Every `(map, seed)` is
played in **both orientations** — the challenger seated at the first position in one game and at the
second in the other — so any residual start-position or seating advantage cancels out. Consequently
**two equally-strong AIs score an exact 50%**, and a win share whose **95% confidence-interval lower
bound is above 50%** (Wilson interval, shown in the summary) is real evidence the challenger is stronger
— not lucky seeds.

Because per-`(map, seed)` outcomes swing 2-0 / 1-1 / 0-2, **use ≥ 8 seeds** (`--num-seeds 8`, 48 games)
for any verdict; the harness marks fewer than 30 decisive games `INCONCLUSIVE`.

## Maps & ruleset

Default maps (under `build/share/s25rttr/RTTR/MAPS/NEW/`), all symmetric so AI skill, not position,
decides games:

| map             | size      | players | notes                          |
|-----------------|-----------|---------|--------------------------------|
| `TueranTuer.SWD`| 32 × 48   | 2       | small, fast, close-quarters    |
| `dreamland.swd` | 80 × 80   | 4       | medium                         |
| `Landstr.swd`   | 160 × 160 | 4       | large                          |

A 1v1 uses only the first two start positions (`--positions 0,1` by default). On maps with more slots
than players the unused slots are left empty, so the **same two start positions are always used**.

Default ruleset matches the common AI-test setup: **inexhaustible mines + gold→granite** (no
gold/coins, so military strength ≈ soldier count). Games run to 400k GF but abort early once one side's
populated land reaches `--dominance-factor`× the other's (default 3×, after 60k GF) to save wall-clock.

## `eval.py` options (selected)

| option | default | meaning |
|--------|---------|---------|
| `--challenger NAME` | `aijh` | AI under test (an `ai-battle --ai` name) |
| `--baseline NAME` | `aijh` | opponent |
| `--maps F...` | the 3 above | map filenames under `--mapdir` |
| `--positions "i,j"` | `0,1` | the two fixed start slots used on every map |
| `--num-seeds N` | – | use N deterministic seeds (overrides `--seeds`) |
| `--max-gf N` | 400000 | per-game frame cap |
| `--dominance-factor X` | 3.0 | early-abort land ratio (0 disables) |
| `--gold-deposits N` | 4 | `CHANGE_GOLD_DEPOSITS` (4 = gold→granite) |
| `--jobs N` | cpus-1 | games run in parallel |
| `--stats` | off | also write per-game trajectory CSVs |

Output: a per-map and overall W/L/D table with the 95% CI and a `PASS`/`FAIL`/`INCONCLUSIVE` verdict,
plus per-game logs and a `summary.json` under `tools/ai-eval/results/<timestamp>_<challenger>_vs_<baseline>/`.

## `ai-battle` additions used by the harness

The harness relies on these (general-purpose) `ai-battle` features:

- a machine-readable `RESULT` / `RESULT_PLAYER` block printed at the end of every game;
- `--positions i,j` — seat the AIs at specific, fixed map start slots (others left empty);
- `--stats FILE [--statsInterval N]` — per-player trajectory CSV;
- `--dominanceFactor X [--minDominanceGF N]` — early abort on a lopsided land lead;
- addon flags `--inexhaustibleMines`, `--goldDeposits N`, `--maxRank N`, and `--teams "0,1;2,3"`.

The runner reproduces a real game's start (start pacts + resource setup) so recorded replays are valid.

## Evaluating your own AI

1. Add your AI type and register it in `AIFactory::Create` and `ParseAIOptions`
   (`libs/s25main/QuickStartGame.cpp`).
2. `cd build && make ai-battle -j`.
3. `tools/ai-eval/eval.py --challenger <your-ai-name> --baseline aijh --num-seeds 8`.
