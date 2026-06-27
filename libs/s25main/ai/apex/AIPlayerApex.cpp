// Copyright (C) 2005 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AIPlayerApex.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "BuildingRegister.h"
#include "addons/const_addons.h"
#include "ai/aijh/BuildingPlanner.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/GoodTypes.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/JobTypes.h"
#include <algorithm>

namespace AIApex {

namespace {
// Early-aggression ramp ("too strong in the beginning"): while the AI has fewer than
// EARLY_AGGRO_MIL_THRESHOLD military buildings it stretches the cadence between outgoing attacks by
// EARLY_ATTACK_SLOWDOWN frames for each building it is still missing, then converges to the configured
// cadence. This only delays offensives; garrisoning/defence/expansion are unchanged.
constexpr unsigned EARLY_AGGRO_MIL_THRESHOLD = 30;
constexpr unsigned EARLY_ATTACK_SLOWDOWN = 90;
} // namespace

AIPlayerApex::AIPlayerApex(const unsigned char playerId, const GameWorldBase& gwb, const AI::Level level)
    : AIJH::AIPlayerJH(playerId, gwb, level)
{
    // The base planner is built (and its wants computed) inside AIPlayerJH's ctor with the vanilla
    // strategy. Apply the improved refinements once now so the very first planning tick already reflects
    // them (matching how the improvements used to run at planner construction).
    RefineBuildingsWanted();
}

unsigned AIPlayerApex::GetEffectiveAttackInterval() const
{
    const unsigned base = AIJH::AIPlayerJH::GetEffectiveAttackInterval();
    const unsigned milBlds = static_cast<unsigned>(player.GetBuildingRegister().GetMilitaryBuildings().size());
    if(milBlds < EARLY_AGGRO_MIL_THRESHOLD)
        return base + (EARLY_AGGRO_MIL_THRESHOLD - milBlds) * EARLY_ATTACK_SLOWDOWN;
    return base;
}

unsigned AIPlayerApex::GetRecruitingRatio() const
{
    // Recruit fewer soldiers from the population during the opening, ramping quickly back to full
    // recruiting once the base is established (~10 military buildings). Softens the early military
    // spike a human faces and leaves more carriers/workers for the economy early, WITHOUT permanently
    // weakening the late-game army (full recruiting resumes, now fed by a larger economy).
    const unsigned milBlds = static_cast<unsigned>(player.GetBuildingRegister().GetMilitaryBuildings().size());
    return std::min<unsigned>(10u, 5u + milBlds / 2u);
}

void AIPlayerApex::RefineBuildingsWanted()
{
    // === lift the self-imposed economic ceiling ===
    // The baseline AIJH plateaus (and then shrinks) its economy while sitting on large idle stockpiles
    // of boards/stones and a big unused pool of carriers: its build wants are capped by small constants
    // and by the (often plateauing) military-building count. Scale the production chain with the actual
    // economy so it keeps converting idle resources into a bigger economy and a growing army. The
    // dependent buildings (wells/mills/bakeries/breweries) are derived from *actual* building counts, so
    // raising these primary drivers converges safely over the next planner ticks.
    AIJH::BuildingPlanner& bp = GetBldPlanner();
    const unsigned numMilitaryBlds = static_cast<unsigned>(player.GetBuildingRegister().GetMilitaryBuildings().size());
    const unsigned foodusers = bp.GetNumBuildings(BuildingType::Charburner) + bp.GetNumBuildings(BuildingType::Mill)
                               + bp.GetNumBuildings(BuildingType::Brewery) + bp.GetNumBuildings(BuildingType::PigFarm)
                               + bp.GetNumBuildings(BuildingType::DonkeyBreeder);
    ApplyStoneSupply(bp, numMilitaryBlds);
    ApplyScaling(bp, numMilitaryBlds, foodusers);
    ApplyFoodSupply(bp, numMilitaryBlds);
}

void AIPlayerApex::ApplyScaling(AIJH::BuildingPlanner& bp, const unsigned numMilitaryBlds, const unsigned foodusers)
{
    const Inventory& inventory = player.GetInventory();

    // Only intensify the economy when we are a MATURE base sitting on a LARGE idle stockpile of building
    // materials - i.e. territorial expansion has effectively saturated and the surplus would otherwise
    // just pile up unused. Testing showed that triggering any earlier diverts resources away from (far
    // more valuable) territorial expansion and makes the AI markedly weaker, so the gate is kept strict.
    if(numMilitaryBlds < 25 || inventory.goods[GoodType::Boards] < 200 || inventory.goods[GoodType::Stones] < 120)
        return;

    // Actual food-production buildings currently standing (gates how many mines can be fed).
    const unsigned numFoodProducers =
      bp.GetNumBuildings(BuildingType::Bakery) + bp.GetNumBuildings(BuildingType::Slaughterhouse)
      + bp.GetNumBuildings(BuildingType::Hunter) + bp.GetNumBuildings(BuildingType::Fishery);
    const auto raise = [&](BuildingType bld, unsigned to) {
        bp.SetBuildingsWanted(bld, std::max(bp.GetBuildingsWanted(bld), to));
    };

    // Keystone: more toolmakers so tool supply (hence new specialist workers) scales with the empire.
    const unsigned numSmelters = bp.GetNumBuildings(BuildingType::Ironsmelter);
    if(numSmelters > 0)
        raise(BuildingType::Metalworks, std::min<unsigned>(1 + numSmelters / 3, 3));

    // Grow farms ahead of their consumers so they can feed more mines (food gates mining).
    raise(BuildingType::Farm, std::min<unsigned>(inventory.goods[GoodType::Scythe] + inventory.people[Job::Farmer],
                                                 foodusers + 3 + numMilitaryBlds / 4));

    // Scale the iron/coal/gold chain with food + smelters so weapon (and tool) output keeps rising ->
    // the army keeps growing instead of capping. Each mine still needs a pickaxe+miner and is bounded by
    // the food producers that can sustain it.
    const unsigned numFarms = bp.GetNumBuildings(BuildingType::Farm);
    if(numFarms >= 4 && numFoodProducers >= 3)
    {
        const unsigned wantIron =
          std::min<unsigned>({numFarms * 2 / 3 + 1, numSmelters + 2, std::max(numFoodProducers, 2u)});
        raise(BuildingType::IronMine, wantIron);

        raise(BuildingType::Ironsmelter,
              std::min<unsigned>(inventory.goods[GoodType::Crucible] + inventory.people[Job::IronFounder],
                                 bp.GetNumBuildings(BuildingType::IronMine)));

        const unsigned smelters = bp.GetNumBuildings(BuildingType::Ironsmelter);
        const unsigned toolmakers = bp.GetBuildingsWanted(BuildingType::Metalworks);
        unsigned wantArmory = smelters > toolmakers ? smelters - toolmakers : 0u;
        if(ggs.isEnabled(AddonId::HALF_COST_MIL_EQUIP))
            wantArmory *= 2;
        raise(BuildingType::Armory, wantArmory);

        raise(BuildingType::CoalMine,
              std::min<unsigned>(bp.GetNumBuildings(BuildingType::IronMine) * 2
                                   + bp.GetNumBuildings(BuildingType::GoldMine),
                                 numFarms + numFoodProducers));

        if(ggs.GetMaxMilitaryRank() > 0)
            raise(BuildingType::GoldMine, std::min<unsigned>(smelters / 2 + 1, 5));
    }
}

void AIPlayerApex::ApplyStoneSupply(AIJH::BuildingPlanner& bp, const unsigned numMilitaryBlds)
{
    // Stone gates ALL construction: every building needs stone, big ones a lot. The baseline caps
    // quarries at ~6 and treats granite mining as a rare fallback, which starves the whole economy on
    // stone-poor maps (sparse/spread surface stone, and quarries DEPLETE). Scale stone production with
    // construction demand and lean on granite mines, which don't run dry (never with INEXHAUSTIBLE_MINES).
    const Inventory& inventory = player.GetInventory();
    const unsigned stoneStock = inventory.goods[GoodType::Stones];
    const unsigned numSites = static_cast<unsigned>(player.GetBuildingRegister().GetBuildingSites().size());

    // Target number of stone producers (quarries + granite mines), scaled to the empire and ramped when
    // the stockpile is low. When stone is plentiful we leave the baseline wants untouched.
    unsigned stoneTarget;
    if(stoneStock < 40)
        stoneTarget = numMilitaryBlds / 5 + 4;
    else if(stoneStock < 120)
        stoneTarget = numMilitaryBlds / 8 + 3;
    else if(stoneStock < 300)
        stoneTarget = numMilitaryBlds / 12 + 2;
    else
        return; // plenty of stone; baseline quarry/granite wants are fine
    if(numSites > 12 && stoneStock < 150)
        stoneTarget += 2; // construction is backing up on stone -> push harder

    // Quarries first (cheap surface stone, no miner/food cost), bounded by available stonemasons.
    // Over-wanting is harmless: placement only succeeds where surface stone actually exists.
    const unsigned quarryStaff = inventory.people[Job::Stonemason] + inventory.goods[GoodType::PickAxe];
    bp.SetBuildingsWanted(BuildingType::Quarry,
                          std::max(bp.GetBuildingsWanted(BuildingType::Quarry), std::min(stoneTarget, quarryStaff)));

    // Granite mines are the sustainable backbone (they don't deplete with inexhaustible mines). Cover
    // whatever the quarries don't, bounded by the shared miner pool so we don't starve coal/iron.
    const unsigned haveQuarry = bp.GetNumBuildings(BuildingType::Quarry);
    const unsigned mineStaff = inventory.people[Job::Miner] + inventory.goods[GoodType::PickAxe];
    const unsigned graniteNeed = (stoneTarget > haveQuarry) ? stoneTarget - haveQuarry : 1u;
    bp.SetBuildingsWanted(BuildingType::GraniteMine,
                          std::max(bp.GetBuildingsWanted(BuildingType::GraniteMine), std::min(graniteNeed, mineStaff)));
}

void AIPlayerApex::ApplyFoodSupply(AIJH::BuildingPlanner& bp, const unsigned numMilitaryBlds)
{
    // Food gates mining: a miner eats bread/meat/fish, and when it runs out the mine idles while the AI
    // sits on a big IDLE farmer reserve. Two baseline defects cause it:
    //  (1) Farm want = min(scythe + people[Farmer], foodusers+3); people[Farmer] counts only the IDLE
    //      (warehoused) farmers, so deploying a farm consumes one and the fixed point is built==idle ->
    //      it deploys ~half its farmers and parks the rest forever.
    //  (2) Charburners consume GRAIN (Wood+Grain->Coal); a coal shortage makes the baseline build many,
    //      and they then steal the grain the bread chain needs -> the mine starvation feeds itself.
    // Raise the food chain toward what the mines need, deploying the parked farmers. Only ever raises
    // wants (except clamping charburner growth) and is bounded so it stays safe on cramped maps.
    const Inventory& inventory = player.GetInventory();

    // The food consumers we must sustain: every mine eats table food.
    const unsigned numMines = bp.GetNumBuildings(BuildingType::CoalMine) + bp.GetNumBuildings(BuildingType::IronMine)
                              + bp.GetNumBuildings(BuildingType::GoldMine)
                              + bp.GetNumBuildings(BuildingType::GraniteMine);
    if(numMines == 0)
        return; // nothing to feed -> baseline food wants are fine

    // Are the mines actually under-fed? Enough table-food producers AND a stock buffer -> leave baseline
    // alone (keeps already-healthy economies, and water/fish-fed maps, unchanged - fish counts here).
    const unsigned numFoodProducers =
      bp.GetNumBuildings(BuildingType::Bakery) + bp.GetNumBuildings(BuildingType::Slaughterhouse)
      + bp.GetNumBuildings(BuildingType::Hunter) + bp.GetNumBuildings(BuildingType::Fishery);
    const unsigned tableFood = inventory.goods[GoodType::Bread] + inventory.goods[GoodType::Meat]
                               + inventory.goods[GoodType::Ham] + inventory.goods[GoodType::Fish];
    if(numFoodProducers * 2 >= numMines && tableFood >= numMines)
        return;

    const auto raise = [&](BuildingType bld, unsigned to) {
        bp.SetBuildingsWanted(bld, std::max(bp.GetBuildingsWanted(bld), to));
    };

    // --- Farms: deploy the parked farmer reserve the baseline leaves idle. Count the farmers ALREADY
    // working in farms (built) on top of the idle+scythe pool so the want can actually exceed today's
    // farm count. Bound by mine demand and, for cramped/boxed-in maps, by empire size; where there is no
    // spare farmland the placement just fails, so this never over-builds there.
    const unsigned builtFarms = bp.GetNumBuildings(BuildingType::Farm);
    const unsigned farmerCapacity = builtFarms + inventory.people[Job::Farmer] + inventory.goods[GoodType::Scythe];
    const unsigned empireCeil = numMilitaryBlds / 2 + 4; // space-aware ceiling
    raise(BuildingType::Farm, std::min<unsigned>({numMines, farmerCapacity, empireCeil}));

    // --- Mills + bakeries: turn the extra grain into bread (the miners' staple) so it reaches the mines.
    // Bounded by their own workers/tools; over-wanting is harmless (placement/staffing gate it).
    raise(BuildingType::Mill,
          std::min<unsigned>(numMines * 2 / 3 + 1,
                             bp.GetNumBuildings(BuildingType::Mill) + inventory.people[Job::Miller] + 1));
    raise(BuildingType::Bakery,
          std::min<unsigned>(bp.GetBuildingsWanted(BuildingType::Mill),
                             bp.GetNumBuildings(BuildingType::Bakery) + inventory.people[Job::Baker]
                               + inventory.goods[GoodType::Rollingpin] + 1));

    // --- Fishery: alternative table food that needs no farmland - the answer for water-rich / farmland-
    // poor maps. Placement gates it to the coast, so it only takes where the map supports it.
    raise(BuildingType::Fishery,
          std::min<unsigned>(numMines, bp.GetNumBuildings(BuildingType::Fishery) + inventory.people[Job::Fisher]
                                         + inventory.goods[GoodType::RodAndLine]));

    // --- Wells: bread/pig/brewery all need water; keep it ahead of the bakeries we just asked for.
    raise(BuildingType::Well, bp.GetBuildingsWanted(BuildingType::Bakery) + bp.GetNumBuildings(BuildingType::PigFarm)
                                + bp.GetNumBuildings(BuildingType::Brewery));

    // --- Charburners: they burn grain. While food is short, stop adding them so grain flows to the bread
    // chain instead of into a coal substitute that starves the mines it is meant to help.
    bp.SetBuildingsWanted(BuildingType::Charburner,
                          std::min(bp.GetBuildingsWanted(BuildingType::Charburner),
                                   bp.GetNumBuildings(BuildingType::Charburner)));
}

} // namespace AIApex
