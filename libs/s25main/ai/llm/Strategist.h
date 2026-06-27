// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Strategy.h"
#include <string>

namespace AIllm {

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

private:
    Persona persona_;
    std::string rationale_;
};

/// Apply a persona's opening preset to a Strategy.
void applyPersona(Persona persona, Strategy& strategy);

/// Pick a persona from the shared AI RNG (seed-dependent -> varies between games, reproducible).
Persona pickRandomPersona();

} // namespace AIllm
