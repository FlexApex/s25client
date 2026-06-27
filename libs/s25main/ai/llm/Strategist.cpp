// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Strategist.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "ai/AIInterface.h"
#include "ai/AIResource.h"
#include "ai/random.h"
#include "gameData/BuildingProperties.h"
#include "gameTypes/StatisticTypes.h"
#include "buildings/noBuildingSite.h"
#include "buildings/nobBaseWarehouse.h"
#include "buildings/nobMilitary.h"
#include "world/GameWorldBase.h"
#include <algorithm>

namespace AIllm {

namespace {
    // raise(v,floor): v=max(v,floor) then clamp to [lo,hi]. capDown(v,ceil): v=min(v,ceil) then clamp.
    inline void raise(int& v, int floor, int lo = 0, int hi = 10)
    {
        v = std::max(v, floor);
        v = std::min(std::max(v, lo), hi);
    }
    inline void capDown(int& v, int ceil, int lo = 0, int hi = 10)
    {
        v = std::min(v, ceil);
        v = std::min(std::max(v, lo), hi);
    }
} // namespace

MapPoint nearestUnclaimedResource(const AIContext& ctx, const MapPoint center, const AISubSurfaceResource r,
                                  const unsigned radius)
{
    if(!center.isValid())
        return MapPoint::Invalid();
    const AIInterface& aii = ctx.aii;
    const GameWorldBase& gwb = ctx.gwb;
    MapPoint best = MapPoint::Invalid();
    unsigned bestDist = 999;
    for(const MapPoint pt : gwb.GetPointsInRadius(center, radius))
    {
        if(aii.IsOwnTerritory(pt))
            continue;
        if(aii.GetSubsurfaceResource(pt) != r)
            continue;
        const unsigned d = gwb.CalcDistance(center, pt);
        if(d < bestDist)
        {
            bestDist = d;
            best = pt;
        }
    }
    return best;
}

MapPoint nearestUnclaimedSurfaceStone(const AIContext& ctx, const MapPoint center, const unsigned radius)
{
    if(!center.isValid())
        return MapPoint::Invalid();
    const AIInterface& aii = ctx.aii;
    const GameWorldBase& gwb = ctx.gwb;
    MapPoint best = MapPoint::Invalid();
    unsigned bestDist = 999;
    for(const MapPoint pt : gwb.GetPointsInRadius(center, radius))
    {
        if(aii.IsOwnTerritory(pt))
            continue;
        if(aii.GetSurfaceResource(pt) != AISurfaceResource::Stones)
            continue;
        const unsigned d = gwb.CalcDistance(center, pt);
        if(d < bestDist)
        {
            bestDist = d;
            best = pt;
        }
    }
    return best;
}

unsigned nearestUnclaimedResourceDist(const AIContext& ctx, const MapPoint center, const AISubSurfaceResource r,
                                      const unsigned radius)
{
    const MapPoint pt = nearestUnclaimedResource(ctx, center, r, radius);
    return pt.isValid() ? ctx.gwb.CalcDistance(center, pt) : 255u;
}

unsigned nearestUnclaimedSurfaceStoneDist(const AIContext& ctx, const MapPoint center, const unsigned radius)
{
    const MapPoint pt = nearestUnclaimedSurfaceStone(ctx, center, radius);
    return pt.isValid() ? ctx.gwb.CalcDistance(center, pt) : 255u;
}

EconStats gatherEconStats(const AIContext& ctx, const unsigned gf, const bool withResourceScan)
{
    const AIInterface& aii = ctx.aii;
    const Inventory& inv = aii.GetInventory();

    EconStats s;
    s.gf = gf;
    s.nMil = static_cast<unsigned>(aii.GetMilitaryBuildings().size());
    s.nStore = static_cast<unsigned>(aii.GetStorehouses().size());
    for(const noBuildingSite* bs : aii.GetBuildingSites())
    {
        if(BuildingProperties::IsMilitary(bs->GetBuildingType()))
            ++s.nMilSites;
    }

    s.boards = inv[GoodType::Boards];
    s.stones = inv[GoodType::Stones];
    s.swords = inv[GoodType::Sword];
    s.shields = inv[GoodType::ShieldRomans] + inv[GoodType::ShieldVikings] + inv[GoodType::ShieldAfricans]
                + inv[GoodType::ShieldJapanese];
    s.beer = inv[GoodType::Beer];
    s.helpers = inv[Job::Helper];
    s.soldiers = inv[Job::Private] + inv[Job::PrivateFirstClass] + inv[Job::Sergeant] + inv[Job::Officer]
                 + inv[Job::General];

    s.armories = static_cast<unsigned>(aii.GetBuildings(BuildingType::Armory).size());
    s.coalMines = static_cast<unsigned>(aii.GetBuildings(BuildingType::CoalMine).size());
    s.ironMines = static_cast<unsigned>(aii.GetBuildings(BuildingType::IronMine).size());
    s.ironsmelters = static_cast<unsigned>(aii.GetBuildings(BuildingType::Ironsmelter).size());

    s.hasGold = ctx.ggs.getSelection(AddonId::CHANGE_GOLD_DEPOSITS) == 0;

    s.myMilitary = ctx.player.GetStatisticCurrentValue(StatisticType::Military);
    s.myBuildings = ctx.player.GetStatisticCurrentValue(StatisticType::Buildings);
    for(unsigned i = 0; i < ctx.gwb.GetNumPlayers(); ++i)
    {
        if(i == ctx.playerId || !aii.IsPlayerAttackable(static_cast<unsigned char>(i)))
            continue;
        const GamePlayer& op = ctx.gwb.GetPlayer(i);
        s.bestEnemyMilitary = std::max(s.bestEnemyMilitary, op.GetStatisticCurrentValue(StatisticType::Military));
        s.bestEnemyBuildings = std::max(s.bestEnemyBuildings, op.GetStatisticCurrentValue(StatisticType::Buildings));
    }

    // Resource-distance awareness: a bounded (r=40) scan around a robust center (storehouses, incl. the
    // HQ, survive an HQ loss). Counts only UNCLAIMED nodes (own territory is already secured). 255 = none
    // found in radius. Uses the shared free functions (M4.1) so the executor's ore-march and the digests
    // see the same geometry. ONLY run when an LLM will consume them (withResourceScan): the no-LLM floor
    // and the executor's hot 120/200-GF calls must NOT pay for these scans (B1 / SPEC D8 cost ceiling).
    if(withResourceScan)
    {
        MapPoint center = MapPoint::Invalid();
        if(!aii.GetStorehouses().empty())
            center = aii.GetStorehouses().front()->GetPos();
        else if(!aii.GetMilitaryBuildings().empty())
            center = aii.GetMilitaryBuildings().front()->GetPos();
        if(center.isValid())
        {
            s.nearestIronDist = nearestUnclaimedResourceDist(ctx, center, AISubSurfaceResource::Ironore, 40);
            s.nearestCoalDist = nearestUnclaimedResourceDist(ctx, center, AISubSurfaceResource::Coal, 40);
            s.nearestGraniteDist = nearestUnclaimedResourceDist(ctx, center, AISubSurfaceResource::Granite, 40);
            s.nearestStoneDist = nearestUnclaimedSurfaceStoneDist(ctx, center, 40);
        }
    }

    // Chain-health diagnostics (cheap, derived from already-gathered counts).
    s.stoneStarved = s.stones < 20;
    s.ironChainBroken = (s.nMil > 0) && (s.ironMines == 0 || s.coalMines == 0 || s.ironsmelters == 0);
    const unsigned farms = static_cast<unsigned>(aii.GetBuildings(BuildingType::Farm).size());
    const unsigned mills = static_cast<unsigned>(aii.GetBuildings(BuildingType::Mill).size());
    const unsigned fisheries = static_cast<unsigned>(aii.GetBuildings(BuildingType::Fishery).size());
    s.foodSecure = farms >= 2 && (mills > 0 || fisheries > 0);

    return s;
}

void ApplyFocusToKnobs(Strategy& s)
{
    // Order: phase -> defense -> expand intent -> attack intent -> focus primary -> focus secondary.
    // Every write clamps to the knob domain (0..10; frontierFill 0..8). Raise-only (+ a few caps),
    // so calling this twice is a no-op (idempotent) and it can never push a knob below the heuristic
    // floor. Mapping is the binding table in SPEC §4.1.
    constexpr int kFrontierHi = 8;

    switch(s.phase)
    {
        case Phase::Open:
            raise(s.expansionAggression, 5);
            raise(s.economyFocus, 6);
            break;
        case Phase::Expand:
            raise(s.expansionAggression, 7);
            s.wantExpand = true;
            break;
        case Phase::Consolidate:
            raise(s.economyFocus, 6);
            raise(s.militaryFocus, 6);
            raise(s.frontierFill, 6, 0, kFrontierHi);
            break;
        case Phase::Push:
            raise(s.militaryFocus, 7);
            raise(s.attackAggression, 7);
            raise(s.recruitRatio, 8);
            raise(s.frontierFill, 7, 0, kFrontierHi);
            break;
        case Phase::Defend:
            raise(s.economyFocus, 5);
            raise(s.militaryFocus, 8);
            capDown(s.attackAggression, 4);
            raise(s.recruitRatio, 9);
            s.frontierFill = kFrontierHi;
            break;
        case Phase::Auto: break;
    }

    switch(s.defense)
    {
        case DefensePosture::Loose: capDown(s.frontierFill, 5, 0, kFrontierHi); break;
        case DefensePosture::Firm:
            raise(s.militaryFocus, 5);
            raise(s.frontierFill, 6, 0, kFrontierHi);
            break;
        case DefensePosture::Fortress:
            raise(s.militaryFocus, 8);
            capDown(s.attackAggression, 4);
            raise(s.recruitRatio, 9);
            s.frontierFill = kFrontierHi;
            break;
        case DefensePosture::Auto: break;
    }

    switch(s.expandIntent)
    {
        case ExpandIntent::Hard:
            raise(s.expansionAggression, 9);
            s.wantExpand = true;
            break;
        case ExpandIntent::Steady:
            raise(s.expansionAggression, 5);
            s.wantExpand = true;
            break;
        case ExpandIntent::Halt: s.wantExpand = false; break;
        case ExpandIntent::Auto: break;
    }

    switch(s.attackIntent)
    {
        case AttackIntent::AllIn:
            raise(s.militaryFocus, 6);
            raise(s.attackAggression, 10);
            raise(s.recruitRatio, 8);
            break;
        case AttackIntent::Commit:
            raise(s.militaryFocus, 5);
            raise(s.attackAggression, 7);
            break;
        case AttackIntent::Probe: raise(s.attackAggression, 4); break;
        case AttackIntent::Hold: capDown(s.attackAggression, 1); break;
        case AttackIntent::Auto: break;
    }

    auto applyFocus = [&](Focus f) {
        switch(f)
        {
            case Focus::SecureIron: raise(s.militaryFocus, 7); break;
            case Focus::SecureCoal: raise(s.militaryFocus, 6); break;
            case Focus::SecureStone:
                raise(s.expansionAggression, 6);
                raise(s.economyFocus, 6);
                break;
            case Focus::ExpandFront:
                raise(s.expansionAggression, 7);
                s.wantExpand = true;
                break;
            case Focus::BoomEconomy: raise(s.economyFocus, 8); break;
            case Focus::AttackEnemy:
                raise(s.militaryFocus, 5);
                raise(s.attackAggression, 7);
                break;
            case Focus::Defend:
                raise(s.economyFocus, 5);
                raise(s.militaryFocus, 7);
                capDown(s.attackAggression, 3);
                raise(s.recruitRatio, 8);
                raise(s.frontierFill, 7, 0, kFrontierHi);
                break;
            case Focus::Raid:
                raise(s.militaryFocus, 5);
                capDown(s.frontierFill, 4, 0, kFrontierHi);
                break;
            case Focus::None: break;
        }
    };
    applyFocus(s.focusPrimary);
    applyFocus(s.focusSecondary);
}

void applyPersona(const Persona persona, Strategy& s)
{
    s.persona = persona;
    switch(persona)
    {
        case Persona::Rusher:
            s.expansionAggression = 7;
            s.economyFocus = 3;
            s.militaryFocus = 8;
            s.attackAggression = 8;
            break;
        case Persona::Boomer:
            s.expansionAggression = 5;
            s.economyFocus = 9;
            s.militaryFocus = 4;
            s.attackAggression = 2;
            break;
        case Persona::Turtle:
            s.expansionAggression = 3;
            s.economyFocus = 6;
            s.militaryFocus = 6;
            s.attackAggression = 1;
            s.frontierFill = 8;
            break;
        case Persona::Expander:
            s.expansionAggression = 9;
            s.economyFocus = 5;
            s.militaryFocus = 4;
            s.attackAggression = 3;
            break;
        case Persona::Balanced:
        default:
            s.expansionAggression = 5;
            s.economyFocus = 5;
            s.militaryFocus = 5;
            s.attackAggression = 4;
            break;
    }
}

Persona pickRandomPersona()
{
    // Slight bias toward expander/boomer: the user's complaint is plateauing, not weak rushes.
    switch(AI::randomValue(0, 5))
    {
        case 0: return Persona::Rusher;
        case 1: return Persona::Boomer;
        case 2: return Persona::Boomer;
        case 3: return Persona::Turtle;
        case 4: return Persona::Expander;
        default: return Persona::Balanced;
    }
}

HeuristicStrategist::HeuristicStrategist(const Persona persona) : persona_(persona) {}

void HeuristicStrategist::Update(const unsigned /*gf*/, const AIContext& /*ctx*/, const EconStats& stats,
                                 const bool contained, Strategy& s)
{
    // Start from the persona preset, then adapt to reality.
    applyPersona(persona_, s);

    // Gentle early recruiting that ramps with territory (fewer soldiers early -> more workers; full
    // army once the economy is large). Mirrors the validated tuning insight for the heuristic AI.
    s.recruitRatio = std::min(10, 4 + static_cast<int>(stats.nMil) / 2);

    // React to the strongest enemy.
    const unsigned myMil = stats.myMilitary;
    const unsigned enMil = stats.bestEnemyMilitary;
    const bool behind = enMil > myMil + myMil / 4; // clearly behind militarily
    const bool ahead = myMil > enMil + enMil / 3 && enMil > 0; // clearly ahead
    if(behind) // dig in HARD on weapons + defenders
    {
        s.militaryFocus = std::min(10, s.militaryFocus + 4);
        s.recruitRatio = std::min(10, s.recruitRatio + 2);
        s.frontierFill = 8;
        // Don't go fully passive: keep some counterattacks to relieve pressure on the front.
        s.attackAggression = std::max(3, s.attackAggression - 1);
    } else if(ahead) // clearly ahead -> press
    {
        s.attackAggression = std::min(10, s.attackAggression + 3);
        s.expansionAggression = std::min(10, s.expansionAggression + 1);
    }

    // Containment: boxed in -> convert idle surplus into weapons and press out with attacks. We keep
    // wantExpand ON regardless: probing for new sites is cheap and auto-no-ops, and the moment the
    // border shifts (an attack lands, an enemy weakens) we must resume expanding. Permanently
    // disabling expansion is exactly the plateau we are trying to avoid.
    s.wantExpand = true;
    if(contained)
    {
        s.militaryFocus = std::min(10, s.militaryFocus + 3);
        s.economyFocus = std::min(10, s.economyFocus + 1);
        s.attackAggression = std::min(10, s.attackAggression + 2);
        rationale_ = "Contained: converting surplus into military and pressing out.";
    } else
    {
        rationale_ = (enMil > myMil) ? "Behind on military: building up weapons and defenders."
                                     : "Expanding and growing the economy.";
    }

    // NOTE: the pure heuristic floor deliberately does NOT fill the GamePlan/TickStrategy overlay or
    // call ApplyFocusToKnobs. The executor gates every LLM-intent field (phase/focus/sectorRoles/...)
    // behind llmDriven_/layoutEnabled_, so those fields are unused on the no-LLM floor; and on the LLM
    // path LlmStrategist::Update overwrites them via projectPlanAndTick and applies the overlay itself.
    // Filling+folding them here only mutated the 7 knobs the executor always reads, diverging the floor
    // from the validated M0 baseline (raising expansion/economy, and even lowering counterattack when
    // behind). Keeping the floor == M0 makes it the known-good degradation target and a clean A/B arm.
    s.diagnosis = rationale_;
}

} // namespace AIllm
