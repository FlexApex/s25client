# LLM-driven AI (`AI::Type::Llm`)

A new computer player for Return To The Roots whose **strategy** is driven by a language model,
while all **execution** stays fast, deterministic C++. Selectable as `--ai llm` in `ai-battle` and as
`llm` wherever AIs are chosen (it is a first-class `AI::Type`, distinct from the heuristic `aijh`).

## Why two layers

The engine is a lockstep sim at ~20 frames/s; `RunGF` is called every frame and an LLM cannot run
per-frame. So the AI is split:

```
            every game frame (deterministic, replay-safe)        every ~1000 GF (~50s game time)
   ┌─────────────────────────────────────────────┐        ┌──────────────────────────────────┐
   │  Executor  (AIPlayerLlm)                     │ reads  │  Strategist                       │
   │  build sites · roads · queue · military ·    │◀──────▶│  Heuristic (default)  OR          │
   │  attacks · settings                          │ writes │  LLM via file-oracle transport    │
   └─────────────────────────────────────────────┘ Strategy└──────────────────────────────────┘
```

- **Executor** (`AIPlayerLlm`) turns a small `Strategy` struct into concrete `GameCommand`s every
  frame, using the engine's own building-quality / pathfinding queries (no persistent shadow map).
  It can play a complete, competent game on its own with a default `Strategy`, so the AI degrades
  gracefully when no model is attached and bulk A/B testing needs no LLM cost.
- **Strategist** updates the `Strategy` on a coarse cadence (and on containment). Two impls behind
  one interface (`IStrategist`):
  - `HeuristicStrategist` — deterministic; picks a **persona** per game (from the AI RNG, so seeds
    vary) and adapts to the economy + strongest enemy. No external service.
  - `LlmStrategist` — writes a JSON world snapshot to a spool dir and reads back a plan produced by
    the Python sidecar, which calls any OpenAI-compatible endpoint. Async by default (never blocks);
    synchronous (reproducible) when `RTTR_LLM_BLOCK_MS>0`. Always keeps a heuristic baseline, so a
    missing/slow/garbled model never breaks play — it just falls back and chats a warning.

`Strategy` knobs (all coarse 0–10, the contract the model speaks): `persona`, `expansionAggression`,
`economyFocus`, `militaryFocus`, `attackAggression`, `recruitRatio`, `frontierFill`, `wantExpand`.

## Files

| File | Role |
|------|------|
| `AIPlayerLlm.{h,cpp}` | the executor (economy, expansion, roads, military, attacks) |
| `Strategy.h` | `Strategy`, `EconStats`, `AIContext`, `gatherEconStats` |
| `Strategist.{h,cpp}` | `IStrategist`, `HeuristicStrategist`, personas |
| `LlmStrategist.{h,cpp}` | file-oracle transport (snapshot out, plan in) |
| `../../../extras/ai-battle/llm_sidecar.py` | watches the spool dir, calls the LLM, writes plans |

## Running it

Heuristic (no model, deterministic — use this for bulk eval):
```
ai-battle -m MAP --ai llm --ai aijh ...
```

LLM-driven. Configure the endpoint in `.env` at the repo root (OpenAI-compatible):
```
LLM_URL="https://.../v1"
LLM_APIKEY="..."
LLM_MODEL="z-ai/glm-5.1"
# optional: LLM_TEMPERATURE=0.7  LLM_MAX_TOKENS=700  LLM_TIMEOUT=60
```
Verify the endpoint (never prints the key):
```
python3 extras/ai-battle/llm_sidecar.py --selftest
```
Run the sidecar, then the game pointed at the same spool dir:
```
python3 extras/ai-battle/llm_sidecar.py --spool /tmp/rttr_llm &
RTTR_LLM_SPOOL=/tmp/rttr_llm RTTR_LLM_BLOCK_MS=25000 \
  ai-battle -m MAP --ai llm --ai aijh ...
```
- `RTTR_LLM_SPOOL` set → the `llm` AI uses the model; unset → heuristic.
- `RTTR_LLM_BLOCK_MS>0` → synchronous (reproducible) LLM-in-the-loop; `0`/unset → async (for live
  GUI play: the plan is applied a tick after it is requested, so the game never stalls).

## Protocol (file oracle)

Per decision the executor writes `req_p<player>_<gf>.json` (atomic) and reads `resp_p<player>_<gf>.txt`
(`key=value` lines). The sidecar normalises the model's JSON into that clean, clamped key=value form,
so the C++ parser is trivial and robust. The sidecar caches by situation hash, so reruns are cheap.
