// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Strategist.h"
#include "Strategy.h"
#include "ai/AIPlayer.h"
#include "ai/AIResource.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/MapCoordinates.h"
#include "helpers/EnumArray.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

class GameWorldBase;

namespace AIllm {

/// Strategy-layer AI for Return To The Roots (see ai/llm — two-layer design).
///
/// This class is the deterministic *executor*: every game frame (throttled) it turns the current
/// Strategy into concrete GameCommands using the engine's own pathfinding / building-quality
/// queries — no persistent shadow map. A pluggable IStrategist updates the Strategy on a coarse
/// cadence; the default HeuristicStrategist needs no external service, so the executor always plays.
class AIPlayerLlm final : public AIPlayer
{
public:
    AIPlayerLlm(unsigned char playerId, const GameWorldBase& gwb, AI::Level level);

    void RunGF(unsigned gf, bool gfisnwf) override;
    void OnChatMessage(unsigned sendPlayerId, ChatDestination, const std::string& msg) override;

    /// Number of attacks launched so far (for stats/analysis; mirrors AIPlayerJH's counter).
    unsigned GetNumAttacksLaunched() const { return numAttacksLaunched_; }

private:
    AIContext ctx() const;

    // One-time and periodic drivers
    void InitOnce();
    void SetupDistribution();
    void PlanEconomy(unsigned gf);
    void TryExpand(unsigned gf);
    void AdjustSettings();
    void TryAttack();
    void ConnectUnconnectedSites();

    // Building / road mechanics
    helpers::EnumArray<unsigned, BuildingType> ComputeWanted(const EconStats& s) const;
    unsigned CountFinished(BuildingType type) const;
    unsigned CountSites(BuildingType type) const;
    unsigned NumBuildings(BuildingType type) const { return CountFinished(type) + CountSites(type); }
    bool TooManyOpenSites() const;
    MapPoint FindSite(BuildingType bt, MapPoint around, unsigned radius);
    bool PlaceBuilding(BuildingType bt, unsigned radius);
    bool ConnectFlag(MapPoint flagPos);
    bool FlagConnected(MapPoint flagPos) const;
    void GarbageCollectStuckSites(unsigned gf);
    BuildingType ChooseMilitaryType(const EconStats& s) const;
    bool HasGold() const;

    std::unique_ptr<IStrategist> strategist_;
    Strategy strategy_;

    unsigned initGfDelay_ = 0;
    bool initDone_ = false;
    std::vector<MapPoint> orderedThisNwf_; // collision avoidance within a network frame
    unsigned containedTicks_ = 0;          // consecutive failed expansion attempts
    bool contained_ = false;
    unsigned lastChatGf_ = 0;
    unsigned numAttacksLaunched_ = 0;
    std::map<unsigned, unsigned> siteFirstSeen_; // building-site node idx -> gf first observed (stuck detection)

    // cadence (game frames)
    unsigned buildInterval_ = 200;
    unsigned expandInterval_ = 120;
    unsigned settingsInterval_ = 150;
    unsigned attackInterval_ = 1500;
    unsigned strategyInterval_ = 1000;
};

} // namespace AIllm
