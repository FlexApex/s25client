// Copyright (C) 2005 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "ai/aijh/AIPlayerJH.h"

namespace AIJH {
class BuildingPlanner;
}

namespace AIApex {
/// "ApexAI" - the improved economy/military strategy, built as a subclass of the classic AIJH.
/// The base AIJH stays the original baseline behaviour; everything that makes the AI stronger lives
/// here, hooked in through the small virtual seams AIPlayerJH exposes:
///  - a gentler early game (slower opening attacks + softer early recruiting), and
///  - an economy that scales with the empire instead of plateauing: stone supply on stone-poor maps,
///    food supply so mines don't idle for lack of food, and a lifted late-game production ceiling.
class AIPlayerApex : public AIJH::AIPlayerJH
{
public:
    AIPlayerApex(unsigned char playerId, const GameWorldBase& gwb, AI::Level level);

protected:
    /// Apply the economy refinements (stone/scaling/food) on top of the baseline planner's wants.
    void RefineBuildingsWanted() override;
    /// Stretch the cadence between outgoing attacks while the military is still young.
    unsigned GetEffectiveAttackInterval() const override;
    /// Recruit fewer soldiers from the population during the opening, ramping back to full as we grow.
    unsigned GetRecruitingRatio() const override;

private:
    void ApplyStoneSupply(AIJH::BuildingPlanner& bp, unsigned numMilitaryBlds);
    void ApplyScaling(AIJH::BuildingPlanner& bp, unsigned numMilitaryBlds, unsigned foodusers);
    void ApplyFoodSupply(AIJH::BuildingPlanner& bp, unsigned numMilitaryBlds);
};
} // namespace AIApex
