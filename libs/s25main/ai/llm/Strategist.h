// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Strategy.h"
#include "ai/AIResource.h"
#include "gameTypes/MapCoordinates.h"
#include <string>

namespace AIllm {

/// Nearest UNCLAIMED tile carrying subsurface ore of type `r` within `radius` of `center`, or an
/// invalid MapPoint if none. Pure: reads only the engine's resource/ownership queries, no state change.
/// The single shared radius-scan primitive reused by the executor's ore-march (NearestNeededOre) and
/// the strategist's map/econ digests so both see the exact same geometry.
MapPoint nearestUnclaimedResource(const AIContext& ctx, MapPoint center, AISubSurfaceResource r, unsigned radius);
/// Nearest UNCLAIMED surface-stone (Stones) tile within `radius` of `center`, or an invalid MapPoint.
MapPoint nearestUnclaimedSurfaceStone(const AIContext& ctx, MapPoint center, unsigned radius);

/// Distance (in map tiles) to the nearest UNCLAIMED subsurface ore of type `r` within `radius` of
/// `center`, or 255 if none is found (or `center` is invalid). Thin distance wrapper over the
/// point-returning primitive above, used by the digests (which want the 255 sentinel, not a tile).
unsigned nearestUnclaimedResourceDist(const AIContext& ctx, MapPoint center, AISubSurfaceResource r,
                                      unsigned radius);
/// Distance to the nearest UNCLAIMED surface stone (Stones) within `radius` of `center`, or 255.
unsigned nearestUnclaimedSurfaceStoneDist(const AIContext& ctx, MapPoint center, unsigned radius);

/// The strategist updates the Strategy on a coarse cadence (and on significant events). The default
/// is a deterministic heuristic; later phases add an LLM-backed implementation behind this same
/// interface (file-oracle transport), so the executor never changes.
class IStrategist
{
public:
    virtual ~IStrategist() = default;

    /// Update `strategy` in place. `contained` is true when the executor could not find any new
    /// place to expand (boxed in) — a key trigger for shifting from land-grab to production/attack.
    virtual void Update(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                        Strategy& strategy) = 0;

    /// Optional human-readable note about the last decision (for chat narration / debugging).
    virtual const std::string& lastRationale() const = 0;

    /// The long-lived GamePlan currently in effect (empty/no-op for the heuristic). The executor reads
    /// the projected fields off `Strategy`, so this is a uniform accessor for diagnostics/future use.
    virtual const GamePlan& gamePlan() const = 0;
};

/// Deterministic strategist. Picks a persona once (for between-game variety) and then adapts the
/// strategy to the economy + the strongest enemy each tick. Needs no external service.
class HeuristicStrategist final : public IStrategist
{
public:
    explicit HeuristicStrategist(Persona persona);

    void Update(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                Strategy& strategy) override;
    const std::string& lastRationale() const override { return rationale_; }
    const GamePlan& gamePlan() const override
    {
        static const GamePlan kEmpty{};
        return kEmpty;
    }

private:
    Persona persona_;
    std::string rationale_;
};

/// Fold the overlay intent (phase/focus/intents on `s`) into the 7 legacy knobs the executor reads.
/// Pure function of `s`, identical for the heuristic and LLM paths. Raise-only (with a few caps) and
/// idempotent, so it can never push a knob below the heuristic floor and is safe to call twice.
void ApplyFocusToKnobs(Strategy& s);

/// Apply a persona's opening preset to a Strategy.
void applyPersona(Persona persona, Strategy& strategy);

/// Pick a persona from the shared AI RNG (seed-dependent -> varies between games, reproducible).
Persona pickRandomPersona();

} // namespace AIllm
