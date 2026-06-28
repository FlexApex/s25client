# LLM-driven building placement — design v1

How the LLM AI should achieve **smart, whole-territory- and strategy-aware building placement**
(see the goals in [ai-dev-goals-v1.md](ai-dev-goals-v1.md), "Execution & building placement")
**without calling the LLM any more often than it does today.**

This is a design note, not yet implemented. It records the agreed method and the integration points
in the current code.

## Where placement stands today (the problem)

- Placement is **100 % deterministic C++** in `AIPlayerLlm::FindSite`
  (`libs/s25main/ai/llm/AIPlayerLlm.cpp:452`). The LLM only sets coarse 0–10 knobs plus six
  `sectorRoles`; it never decides individual placements.
- The spatial logic that exists (farm↔forester repulsion, sector-role bias) is **double-gated**
  behind `layoutEnabled_ && hasAnyNonAutoRole()` (`FindSite:465`). Unless the rare LLM plan sets a
  non-Auto role, it is inert — most of the game runs a bare greedy "closest in-territory node with
  positive resource value" search.
- Foresters, farms and hunters all score on the **same** `AIResource::Plantspace` metric
  (`AIPlayerLlm.cpp:62`), so they are mutually *attracted* to the same tiles and pile together.
- Roads take the **shortest** free path, never the straightest (`ConnectFlag`,
  `AIPlayerLlm.cpp:1038`).

Observed in replay `260628041345_llm-vs-apexai-vs-aijh-1v1v1`: foresters dumped in open grass far
from any woodcutter, woodcutters in treeless grass, idle `BAKERY (0)` huts scattered everywhere, no
districts, no grid, sprawling roads. Only hard resource-gated buildings (quarries on stone, mines on
mountains) land correctly.

The AIJH floor already solved the spacing pieces (forester: no other forester in r12 **and** a
plantspace-density gate, `libs/s25main/ai/aijh/AIPlayerJH.cpp:944-945`; farm: radius-3 exclusion via
`SetFarmedNodes`, `AIPlayerJH.cpp:662`). The LLM executor reimplemented `FindSite` and dropped them;
those constants are the reuse baseline.

## The constraint and the key idea

Placement happens every ~120–200 GF — hundreds of times per game. The LLM is called only ~1 / 1000
GF (cheap tactical tick) and ~3–5 times per game (expensive strategic plan), for ~21k tokens over a
40-minute game. **The LLM therefore cannot decide per building.**

> **Key idea — plan-as-blueprint.** The LLM decides the *layout* rarely; deterministic C++ *renders*
> that layout faithfully over the following hundreds of frames. One LLM layout decision amortizes
> over hundreds of free placements.

This is already the shape of the codebase — a `mapDigest` goes in, `sectorRoles` come out, and
`FindSite` renders them. The method below **deepens each of those three stages**, and all new I/O
rides the **existing** expensive/plan call, so **LLM call frequency is unchanged.**

## Stage 1 — Input: let the model *see* the map

Today `buildMapDigest` (`libs/s25main/ai/llm/LlmStrategist.cpp:661`) does a single bounded r=40 pass
around the HQ, bucketing points into **6 sectors** (open room, nearest ore/stone, water, enemy,
chokepoint). The model sees almost no geometry.

Add, in that *same* pass, a coarse **ASCII minimap**: one character per grid cell from a small
legend:

```
.  open / buildable      T  trees        S  surface stone
o  subsurface ore        ~  water         #  mountain / blocked
=  my building           E  enemy         H  my HQ
```

**Resolution vs extent — scale the extent, cap the cells.** Do *not* hardcode a fixed radius: a
fixed scan is mostly-blank (and low-resolution near the HQ) early, and *clips* the empire late, when
layout matters most. Instead:

- **Cell count is a fixed cap** (~12×12, up to ~16×16) → token cost stays flat and bounded
  (~150 chars ≈ ~100–150 tokens). **Expensive tier only**; the SHA1 situation-cache dedupes
  identical snapshots.
- **World extent scales with territory** — cover the owned bounding box + a frontier margin (or
  `r = max(R₀, k·territoryRadius)`). The map auto-zooms: tight early, wide late, constant tokens.
- Quantize `(pt − hq)` onto that grid; reuse the `sectorOf` quantization (`LlmStrategist.cpp:89`) so
  the digest and the executor's `SectorOf` stay consistent.

**Critical caveat — ship the grid frame with the plan.** The executor renders `district@col,row`
against this grid, so if the extent rescales between plans a district anchor's world meaning
*drifts*. Send the grid **origin + scale** alongside the plan and have the renderer use the grid the
plan was authored against until the next plan supersedes it (or only rescale in discrete doublings).

Keep the numeric 6-sector digest as well (cheap, already consumed). An LLM reasons well over a small
ASCII map — this is the cheapest way to give it genuine spatial understanding.

## Stage 2 — Output: a compact district blueprint

Today the plan response carries `sectorRoles[6]` as a CSV line (parsed at
`LlmStrategist.cpp:543`, serialized in the sidecar's `plan_to_kv`,
`extras/ai-battle/llm_sidecar.py:443`).

Add a compact blueprint to the *same* plan response:

- `districts=` — a short CSV, each entry `role@col,row[,priority]` on the minimap grid, e.g.
  `grove@3,4 farmbelt@8,9 mining@11,2 milfront@10,6 core@6,6`. ~4–8 districts ≈ ~60–100 tokens.
  Roles reuse the `SectorRole` vocabulary plus a couple of layout roles (`Grove`, `Core`).
- `layoutStyle=grid|organic` (~3 tokens) — expresses the **grid preference**.
- Reuse the existing `expandIntent` / `wantExpand` for consolidate-vs-sprawl tempo (no new tokens).

**Back-compat & safety:** keep `sectorRoles` as the coarse fallback; every field defaults to no-op
(`Auto`), so a model that emits nothing keeps today's behavior. Clamp coordinates to the grid. The
token delta lands only on the rare expensive call — negligible against the ~21k/game budget.

## Stage 3 — Render: deterministic C++ grid renderer (every frame, zero tokens)

All geometry stays in the executor (`AIPlayerLlm`) — the part an LLM can't do per-frame and C++ does
well:

- **(a) Ungate the geometry.** Move the co-location / spacing rules **out** of the
  `layoutEnabled_ && hasAnyNonAutoRole()` gate — they are geometry, not sector intent — so placement
  is good even with no or weak LLM plan. (Biggest standalone win; LLM-independent.)
- **(b) Grove + belt spacing in `FindSite`** (`AIPlayerLlm.cpp:452`): forester = plantspace-density
  gate + inter-forester spacing ~r12 + a woodcutter→forester attraction; farm = radius-3
  self-exclusion + the existing forester repulsion. Reuse `aii.isBuildingNearby` /
  `aii.CalcResourceValue` and the AIJH constants above. Keep gates **soft** (fall back to
  best-available) so cramped maps don't starve.
- **(c) District-aware scoring.** Generalize today's 6-sector `RoleAt` bias (`FindSite:555`) to the
  finer district grid: convert `(col,row)` → world anchor, pull each building type toward its
  district and penalize it outside.
- **(d) Town lattice → grid-like ways.** Define a coarse lattice anchored at the HQ (spacing ≈
  building + flag footprint). Snap candidate positions / flags toward lattice nodes, and in
  `ConnectFlag` (`AIPlayerLlm.cpp:1038`) prefer the **straightest** among near-shortest routes
  (count direction changes in the returned `std::vector<Direction>` as a tie-break — no new API;
  reuse `FindFreePathForNewRoad`).
- **(e) Wait-for-room** in `PlaceBuilding` (`AIPlayerLlm.cpp:616`): for a layout-sensitive building
  with no spaced slot in its district while expansion is pending (`wantExpand` / `milSites > 0`),
  skip this lap (bounded skip-count fallback) so the grove / belt is deferred until new land opens.
  This realizes overarching goal 2 ("think ahead before clumping").

## Cadence guarantee

The minimap and blueprint ride the **existing** expensive/plan request (gf 0 + the ~20k-GF cadence +
escalation events); the cheap tick is unchanged; the renderer is pure C++. **No new LLM calls and no
higher frequency.** Re-plan triggers reuse the existing ones (contained, territory grew, ground
lost).

## Why this is genuinely LLM-driven

The model, seeing the real minimap, decides the empire's spatial structure — where the grove goes,
where the farm belt sits, where to push military, where the mining outpost is, the overall town
shape. C++ only executes geometry. That is exactly the north-star intent: placement expressing real
spatial intent (districts, frontiers, supply lines), not greedy local guesses.

## Implementation phasing (later effort)

1. Ungate + deepen the deterministic geometry — steps (a), (b), (d-roads), (e). Immediate visual
   win, no LLM dependency, no protocol change.
2. Add the minimap to the digest + `districts` / `layoutStyle` to the blueprint + the district-aware
   renderer — Stages 1–2 and step (c), plus lattice snapping.
3. Tune constants; A/B against the current sector-only layout (the `RTTR_LLM_NO_LAYOUT` arm already
   exists).

## Risks / mitigations

- *Minimap token bloat* → cap grid size, expensive tier only, rely on the SHA1 cache.
- *Grid vs `sectorOf` mismatch* → reuse `sectorOf` quantization so the digest and the executor
  (`SectorOf` / `RoleAt`, `AIPlayerLlm.cpp:99/132`) agree.
- *Over-tight spacing starving food on cramped maps* → soft gates; fall back to best-available after
  N skipped laps (mirrors the existing `v<=0` "open ground wins" pattern in `FindSite`).
- *Garbled districts from the model* → default `Auto` / no-op preserves current behavior; clamp
  coordinates.

## Integration points (current code)

| File | Symbol | Change |
|------|--------|--------|
| `libs/s25main/ai/llm/LlmStrategist.cpp` | `buildMapDigest:661` | add the ASCII minimap to the digest |
| `libs/s25main/ai/llm/LlmStrategist.cpp` | plan parse `sectorRoles:543` | parse `districts` / `layoutStyle` |
| `libs/s25main/ai/llm/Strategy.h` | `GamePlan` / `Strategy` | add `districts[]`, `layoutStyle` (no-op defaults) |
| `libs/s25main/ai/llm/AIPlayerLlm.cpp` | `FindSite:452` | ungate geometry; grove/belt spacing; district-aware scoring |
| `libs/s25main/ai/llm/AIPlayerLlm.cpp` | `PlaceBuilding:616` | wait-for-room gate |
| `libs/s25main/ai/llm/AIPlayerLlm.cpp` | `ConnectFlag:1038` | straight / lattice-aligned roads |
| `extras/ai-battle/llm_sidecar.py` | `plan_to_kv:443`, `_sector_roles_csv:422`, system-prompt schema (~`:249`), stub planner (`:553`) | serialize districts; document the minimap + blueprint contract |

Reuse from the engine / AIJH floor: `AIInterface::isBuildingNearby` / `CalcResourceValue` /
`GetBuildingQuality` / `FindFreePathForNewRoad` / `IsOwnTerritory`; AIJH constants (forester r12 +
density gate, farm r3 exclusion, `GetDensity`).
