// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Strategist.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "ai/AIInterface.h"
#include "ai/random.h"
#include "gameData/BuildingProperties.h"
#include "gameTypes/StatisticTypes.h"
#include "buildings/noBuildingSite.h"
#include "world/GameWorldBase.h"
#include <algorithm>

namespace AIllm {

EconStats gatherEconStats(const AIContext& ctx, const unsigned gf)
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
    return s;
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
    if(enMil > myMil + myMil / 4) // clearly behind militarily -> dig in HARD on weapons + defenders
    {
        s.militaryFocus = std::min(10, s.militaryFocus + 4);
        s.recruitRatio = std::min(10, s.recruitRatio + 2);
        s.frontierFill = 8;
        // Don't go fully passive: keep some counterattacks to relieve pressure on the front.
        s.attackAggression = std::max(3, s.attackAggression - 1);
    } else if(myMil > enMil + enMil / 3 && enMil > 0) // clearly ahead -> press
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
}

} // namespace AIllm
