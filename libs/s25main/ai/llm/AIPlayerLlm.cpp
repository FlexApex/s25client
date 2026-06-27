// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AIPlayerLlm.h"
#include "LlmStrategist.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "ai/AIInterface.h"
#include "ai/random.h"
#include "buildings/nobBaseMilitary.h"
#include "buildings/nobBaseWarehouse.h"
#include "buildings/nobHQ.h"
#include "buildings/nobMilitary.h"
#include "gameData/BuildingConsts.h"
#include "gameData/BuildingProperties.h"
#include "gameData/MilitaryConsts.h"
#include "gameTypes/BuildingQuality.h"
#include "gameTypes/GO_Type.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/SettingsTypes.h"
#include "gameTypes/StatisticTypes.h"
#include "helpers/EnumRange.h"
#include "buildings/noBuildingSite.h"
#include "nodeObjs/noFlag.h"
#include "world/GameWorldBase.h"
#include "world/MilitarySquares.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace AIllm {

namespace {
    unsigned R(double x) { return static_cast<unsigned>(std::max(0L, std::lround(x))); }
    int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    AISubSurfaceResource mineResource(BuildingType bt)
    {
        switch(bt)
        {
            case BuildingType::CoalMine: return AISubSurfaceResource::Coal;
            case BuildingType::IronMine: return AISubSurfaceResource::Ironore;
            case BuildingType::GoldMine: return AISubSurfaceResource::Gold;
            case BuildingType::GraniteMine: return AISubSurfaceResource::Granite;
            default: return AISubSurfaceResource::Nothing;
        }
    }

    // (resource to score the site by, whether to use resource scoring at all)
    std::pair<AIResource, bool> siteResource(BuildingType bt)
    {
        switch(bt)
        {
            case BuildingType::Woodcutter: return {AIResource::Wood, true};
            case BuildingType::Forester:
            case BuildingType::Farm:
            case BuildingType::Hunter: return {AIResource::Plantspace, true};
            case BuildingType::Quarry: return {AIResource::Stones, true};
            case BuildingType::Fishery: return {AIResource::Fish, true};
            case BuildingType::Barracks:
            case BuildingType::Guardhouse:
            case BuildingType::Watchtower:
            case BuildingType::Fortress: return {AIResource::Borderland, true};
            default: return {AIResource::Wood, false};
        }
    }
} // namespace

AIPlayerLlm::AIPlayerLlm(const unsigned char playerId, const GameWorldBase& gwb, const AI::Level level)
    : AIPlayer(playerId, gwb, level)
{}

AIContext AIPlayerLlm::ctx() const
{
    return AIContext{aii, player, gwb, ggs, playerId};
}

bool AIPlayerLlm::HasGold() const
{
    return ggs.getSelection(AddonId::CHANGE_GOLD_DEPOSITS) == 0;
}

void AIPlayerLlm::RunGF(const unsigned gf, bool gfisnwf)
{
    if(aii.IsDefeated())
        return;
    if(!initDone_)
    {
        InitOnce();
        initDone_ = true;
    }
    if(initGfDelay_ < 10)
    {
        ++initGfDelay_;
        return;
    }
    if(gfisnwf)
        orderedThisNwf_.clear();

    if(gf % strategyInterval_ == 0)
    {
        contained_ = containedTicks_ >= 3;
        const EconStats s = gatherEconStats(ctx(), gf);
        strategist_->Update(gf, ctx(), s, contained_, strategy_);
        if(gf - lastChatGf_ >= 8000)
        {
            aii.Chat(strategist_->lastRationale());
            lastChatGf_ = gf;
        }
    }
    if((gf + playerId * 7u) % buildInterval_ == 0)
        PlanEconomy(gf);
    if((gf + playerId * 11u) % expandInterval_ == 0)
        TryExpand(gf);
    if((gf + playerId * 13u) % settingsInterval_ == 0)
        AdjustSettings();
    if((gf + playerId * 17u) % attackInterval_ == 0)
        TryAttack();
    if((gf + playerId * 5u) % 50u == 0)
        ConnectUnconnectedSites();
    if((gf + playerId * 3u) % 2000u == 0)
        GarbageCollectStuckSites(gf);

    if(std::getenv("RTTR_LLM_DEBUG") && gf % 25000 == 0)
    {
        const MapPoint hq = aii.GetHeadquarter() ? aii.GetHeadquarter()->GetPos() : MapPoint(0, 0);
        const MapPoint smeltSpot = FindSite(BuildingType::Ironsmelter, hq, 11);
        std::fprintf(stderr,
                     "[llm p%u gf%u] sites=%zu milblds=%zu mil=%u coal=%zu iron=%zu smelt=%zu(want=%u) "
                     "armory=%zu contained=%d tooMany=%d smeltSpotValid=%d\n",
                     playerId, gf, aii.GetBuildingSites().size(), aii.GetMilitaryBuildings().size(),
                     player.GetStatisticCurrentValue(StatisticType::Military),
                     aii.GetBuildings(BuildingType::CoalMine).size(), aii.GetBuildings(BuildingType::IronMine).size(),
                     aii.GetBuildings(BuildingType::Ironsmelter).size(),
                     ComputeWanted(gatherEconStats(ctx(), gf))[BuildingType::Ironsmelter],
                     aii.GetBuildings(BuildingType::Armory).size(), contained_ ? 1 : 0, TooManyOpenSites() ? 1 : 0,
                     smeltSpot.isValid() ? 1 : 0);
    }
}

void AIPlayerLlm::OnChatMessage(unsigned, ChatDestination, const std::string&) {}

void AIPlayerLlm::InitOnce()
{
    const Persona persona = pickRandomPersona();
    // Use the LLM strategist when a spool dir is configured (RTTR_LLM_SPOOL); otherwise the built-in
    // heuristic. RTTR_LLM_BLOCK_MS>0 switches to synchronous, reproducible LLM-in-the-loop mode.
    if(const char* spool = std::getenv("RTTR_LLM_SPOOL"); spool && *spool)
    {
        unsigned blockMs = 0;
        if(const char* b = std::getenv("RTTR_LLM_BLOCK_MS"))
            blockMs = static_cast<unsigned>(std::atoi(b));
        strategist_ = std::make_unique<LlmStrategist>(playerId, spool, blockMs, persona);
    } else
        strategist_ = std::make_unique<HeuristicStrategist>(persona);

    const EconStats s = gatherEconStats(ctx(), 0);
    strategist_->Update(0, ctx(), s, false, strategy_);
    SetupDistribution();
    AdjustSettings();
}

void AIPlayerLlm::SetupDistribution()
{
    // Sensible good->consumer split (esp. food to mines, beer chain to armories). Mirrors the values
    // the heuristic AI uses; this is mechanics, not strategy.
    Distributions d;
    d[0] = 10;  // food -> GraniteMine
    d[1] = 10;  // food -> CoalMine
    d[2] = 10;  // food -> IronMine
    d[3] = 10;  // food -> GoldMine
    d[4] = 2;   // food -> Temple
    d[5] = 10;  // grain -> Mill
    d[6] = 10;  // grain -> PigFarm
    d[7] = 6;   // grain -> DonkeyBreeder
    d[8] = 10;  // grain -> Brewery
    d[9] = 6;   // grain -> Charburner
    d[10] = 10; // iron -> Armory
    d[11] = 5;  // iron -> Metalworks
    d[12] = 10; // coal -> Armory
    d[13] = 10; // coal -> Ironsmelter
    d[14] = 10; // coal -> Mint
    d[15] = 10; // wood -> Sawmill
    d[16] = 5;  // wood -> Charburner
    d[17] = 2;  // wood -> Vineyard
    d[18] = 10; // boards -> build sites
    d[19] = 4;  // boards -> Metalworks
    d[20] = 2;  // boards -> Shipyard
    d[21] = 1;  // boards -> Tannery
    d[22] = 10; // water -> Bakery
    d[23] = 10; // water -> Brewery
    d[24] = 10; // water -> PigFarm
    d[25] = 6;  // water -> DonkeyBreeder
    d[26] = 2;  // water -> Vineyard
    d[27] = 8;  // ham -> Slaughterhouse
    d[28] = 3;  // ham -> Skinner
    aii.ChangeDistribution(d);
}

helpers::EnumArray<unsigned, BuildingType> AIPlayerLlm::ComputeWanted(const EconStats& s) const
{
    helpers::EnumArray<unsigned, BuildingType> w{};
    const double terr = std::max(1u, s.nMil);
    const double eW = 0.5 + strategy_.economyFocus * 0.1;  // 0.5..1.5
    const double mW = 0.7 + strategy_.militaryFocus * 0.08; // 0.7..1.5 (army is never neglected)

    // Resource extractors. These are gated by *terrain* (FindSite returns nothing where the resource
    // isn't), so over-wanting is harmless: the map limits them, not an artificial staff cap. (The
    // old tool-based cap deadlocked the weapons chain - quarries ate the pickaxes miners needed.)
    // Boards fuel the whole build rate, so keep the wood chain generous (trimming it badly throttles
    // construction of everything, including the army).
    w[BuildingType::Woodcutter] = R(3 + terr * 0.6 * eW);
    w[BuildingType::Forester] = std::max(1u, R(2 + terr * 0.3 * eW));
    w[BuildingType::Quarry] = std::max(2u, R(2 + terr * 0.16));
    // Cap mines: coal only needs to feed the smelters/armories/mint, not blanket every ore node.
    // Uncapped coal floods the build queue and starves the rest of the weapons chain.
    w[BuildingType::CoalMine] = std::min(12u, std::max(2u, R(terr * 0.3 * mW)));
    w[BuildingType::IronMine] = std::min(14u, std::max(2u, R(terr * 0.28 * mW)));
    w[BuildingType::GraniteMine] = contained_ ? 2u : (s.stones < 30 ? 1u : 0u);
    // Food must keep the mines running, so scale it with both territory and the weapons chain.
    w[BuildingType::Farm] = std::max(2u, R(terr * 0.55 * eW));
    w[BuildingType::Fishery] = std::max(1u, R(terr * 0.25));
    w[BuildingType::Hunter] = 3u;
    if(s.hasGold)
        w[BuildingType::GoldMine] = static_cast<unsigned>(std::min(2.0, terr * 0.12));

    // Processors. Capped by upstream supply (finished + sites) so we never build a chain link with no
    // input, but always allow one of each foundational type to bootstrap.
    const unsigned nWoodc = NumBuildings(BuildingType::Woodcutter);
    const unsigned nIron = NumBuildings(BuildingType::IronMine);
    const unsigned nSmelter = NumBuildings(BuildingType::Ironsmelter);
    const unsigned nFarm = NumBuildings(BuildingType::Farm);
    const unsigned nPig = NumBuildings(BuildingType::PigFarm);
    const unsigned nGold = NumBuildings(BuildingType::GoldMine);

    w[BuildingType::Sawmill] = std::max(1u, (nWoodc + 1) / 2);
    // Toolmakers convert iron into the tools that STAFF mines/smelters/armories. Too few -> the whole
    // specialist economy (especially the weapons chain) sits unstaffed. Scale with the empire.
    w[BuildingType::Metalworks] = std::min(6u, std::max(2u, R(terr * 0.12)));
    w[BuildingType::Ironsmelter] = nIron > 0 ? std::max(1u, nIron) : 0u;
    w[BuildingType::Armory] = nSmelter > 0 ? std::max(1u, std::min(R(terr * 0.6 * mW), nSmelter * 3u)) : 0u;
    w[BuildingType::Brewery] = std::max(1u, R(terr * 0.18));
    w[BuildingType::Mill] = nFarm > 0 ? std::max(1u, (nFarm + 1) / 3) : 0u;
    w[BuildingType::Bakery] = nFarm > 0 ? std::max(1u, (nFarm + 1) / 3) : 0u;
    w[BuildingType::PigFarm] = nFarm > 1 ? std::max(1u, (nFarm + 1) / 4) : 0u;
    w[BuildingType::Slaughterhouse] = nPig > 0 ? std::max(1u, nPig) : 0u;
    w[BuildingType::Well] = std::max(1u, R(terr * 0.3));
    if(s.hasGold)
        w[BuildingType::Mint] = nGold > 0 ? std::max(1u, nGold) : 0u;

    // Logistics reach for large empires
    w[BuildingType::Storehouse] = static_cast<unsigned>(terr / 7);

    // Contained: pour idle surplus into more weapons production
    if(contained_)
    {
        w[BuildingType::Armory] += 2;
        w[BuildingType::CoalMine] += 2;
        w[BuildingType::IronMine] += 2;
        w[BuildingType::Ironsmelter] += 1;
        w[BuildingType::Brewery] += 1;
    }
    return w;
}

unsigned AIPlayerLlm::CountFinished(BuildingType type) const
{
    if(BuildingProperties::IsMilitary(type))
    {
        unsigned c = 0;
        for(const nobMilitary* m : aii.GetMilitaryBuildings())
            if(m->GetBuildingType() == type)
                ++c;
        return c;
    }
    if(BuildingProperties::IsWareHouse(type))
    {
        unsigned c = 0;
        for(const nobBaseWarehouse* wh : aii.GetStorehouses())
            if(wh->GetBuildingType() == type)
                ++c;
        return c;
    }
    return static_cast<unsigned>(aii.GetBuildings(type).size());
}

unsigned AIPlayerLlm::CountSites(BuildingType type) const
{
    unsigned c = 0;
    for(const noBuildingSite* bs : aii.GetBuildingSites())
        if(bs->GetBuildingType() == type)
            ++c;
    return c;
}

bool AIPlayerLlm::TooManyOpenSites() const
{
    return aii.GetBuildingSites().size() > 50;
}

MapPoint AIPlayerLlm::FindSite(BuildingType bt, MapPoint around, unsigned radius)
{
    const BuildingQuality need = BUILDING_SIZE[bt];
    const bool isMine = BuildingProperties::IsMine(bt);
    const auto [res, useRes] = siteResource(bt);

    MapPoint best = MapPoint::Invalid();
    int bestScore = INT_MIN;
    unsigned evaluated = 0;
    constexpr unsigned evalCap = 90;

    for(const MapPoint pt : gwb.GetPointsInRadius(around, radius))
    {
        if(!aii.IsOwnTerritory(pt))
            continue;
        bool collide = false;
        for(const MapPoint o : orderedThisNwf_)
        {
            if(gwb.CalcDistance(o, pt) < 5)
            {
                collide = true;
                break;
            }
        }
        if(collide)
            continue;
        if(!canUseBq(aii.GetBuildingQuality(pt), need))
            continue;
        if(need != BuildingQuality::Harbor && aii.isHarborPosClose(pt, 2, true))
            continue;

        if(isMine)
        {
            if(aii.GetSubsurfaceResource(pt) != mineResource(bt))
                continue;
            return pt; // first (closest) matching mine spot
        }
        if(useRes)
        {
            if(++evaluated > evalCap)
                break;
            const int v = aii.CalcResourceValue(pt, res);
            if(v <= 0)
                continue;
            if(v > bestScore)
            {
                bestScore = v;
                best = pt;
            }
        } else
            return pt; // generic: closest valid spot (points come in increasing distance)
    }
    return best;
}

bool AIPlayerLlm::PlaceBuilding(BuildingType bt, unsigned radius)
{
    if(!aii.CanBuildBuildingtype(bt))
        return false;

    std::vector<MapPoint> centers;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        centers.push_back(wh->GetPos());
    for(const nobMilitary* m : aii.GetMilitaryBuildings())
        centers.push_back(m->GetPos());
    if(centers.empty())
        return false;

    const unsigned n = static_cast<unsigned>(centers.size());
    const unsigned start = AI::randomValue(0u, n - 1);
    const unsigned tryCap = std::min(n, 16u);
    for(unsigned k = 0; k < tryCap; ++k)
    {
        const MapPoint pos = FindSite(bt, centers[(start + k) % n], radius);
        if(pos.isValid() && aii.SetBuildingSite(pos, bt))
        {
            orderedThisNwf_.push_back(pos);
            return true;
        }
    }
    return false;
}

void AIPlayerLlm::PlanEconomy(unsigned gf)
{
    if(TooManyOpenSites())
        return;
    contained_ = containedTicks_ >= 3;
    const EconStats s = gatherEconStats(ctx(), gf);
    const auto w = ComputeWanted(s);

    // Interleave the chains so a single pass never spends its whole budget on boards before reaching
    // the weapons chain. Boards -> base food -> WEAPONS -> more food -> rest.
    static const BuildingType order[] = {
      BuildingType::Sawmill,    BuildingType::Woodcutter,    BuildingType::Forester,
      BuildingType::Well,       BuildingType::Farm,          BuildingType::Mill,
      BuildingType::Bakery,     BuildingType::CoalMine,      BuildingType::IronMine,
      BuildingType::Ironsmelter, BuildingType::Armory,       BuildingType::Brewery,
      BuildingType::Fishery,    BuildingType::Hunter,        BuildingType::PigFarm,
      BuildingType::Slaughterhouse, BuildingType::Quarry,    BuildingType::Metalworks,
      BuildingType::GraniteMine, BuildingType::GoldMine,     BuildingType::Mint,
      BuildingType::Storehouse};

    unsigned budget = 12;
    for(const BuildingType bt : order)
    {
        if(budget == 0)
            break;
        int need = static_cast<int>(w[bt]) - static_cast<int>(NumBuildings(bt));
        // Cap placements per type per pass so a high-want type (e.g. mines) can't consume the whole
        // budget and starve the rest of the chain (smelters/armories) of build slots.
        unsigned perType = 0;
        while(need > 0 && budget > 0 && perType < 3)
        {
            if(!PlaceBuilding(bt, 11))
                break;
            --need;
            --budget;
            ++perType;
        }
    }
}

BuildingType AIPlayerLlm::ChooseMilitaryType(const EconStats& s) const
{
    if(s.bestEnemyMilitary > s.myMilitary && s.stones > 40 && aii.CanBuildBuildingtype(BuildingType::Fortress))
        return BuildingType::Fortress;
    if(strategy_.expansionAggression >= 7 && s.stones > 40 && aii.CanBuildBuildingtype(BuildingType::Watchtower))
        return BuildingType::Watchtower;
    if(s.stones > 20 && aii.CanBuildBuildingtype(BuildingType::Guardhouse))
        return BuildingType::Guardhouse;
    return BuildingType::Barracks;
}

void AIPlayerLlm::TryExpand(unsigned gf)
{
    if(!strategy_.wantExpand)
        return;
    // Don't let expansion outrun construction, but keep the ceiling high: territory tempo is what
    // decides the game, and the stuck-site GC now prevents far-flung claims from clogging the queue.
    if(aii.GetBuildingSites().size() > 40)
        return;
    unsigned milSites = 0;
    for(const noBuildingSite* bs : aii.GetBuildingSites())
        if(BuildingProperties::IsMilitary(bs->GetBuildingType()))
            ++milSites;
    const unsigned maxPending = 4 + strategy_.expansionAggression; // 4..14
    if(milSites >= maxPending)
        return;

    // Territory is the master growth lever (it gates farms -> food -> mines -> weapons), so push
    // several claims per pass.
    const EconStats s = gatherEconStats(ctx(), gf);
    const unsigned toPlace = std::min(5u, maxPending - milSites);
    bool anyPlaced = false;
    for(unsigned i = 0; i < toPlace; ++i)
    {
        const BuildingType bt = ChooseMilitaryType(s);
        if(PlaceBuilding(bt, 11) || (bt != BuildingType::Barracks && PlaceBuilding(BuildingType::Barracks, 11)))
            anyPlaced = true;
        else
            break;
    }
    if(anyPlaced)
        containedTicks_ = 0;
    else
        ++containedTicks_;
}

void AIPlayerLlm::AdjustSettings()
{
    bool hasFrontier = false;
    for(const nobMilitary* b : aii.GetMilitaryBuildings())
    {
        if(b->GetFrontierDistance() != FrontierDistance::Far)
        {
            hasFrontier = true;
            break;
        }
    }

    // Occupation policy balances two opposite needs:
    //  - SAFE + soldier-poor  -> spread THIN (1/building) to claim the most land, reach ore, bootstrap.
    //  - THREATENED (stronger enemy / under attack) -> CONCENTRATE at the front to actually hold it;
    //    spreading thin while being attacked just feeds buildings to the enemy (the late-game collapse).
    const unsigned mil = player.GetStatisticCurrentValue(StatisticType::Military);
    const unsigned numMil = static_cast<unsigned>(aii.GetMilitaryBuildings().size());
    const double perBld = numMil ? static_cast<double>(mil) / numMil : 99.0;

    unsigned enemyMil = 0;
    for(unsigned i = 0; i < gwb.GetNumPlayers(); ++i)
        if(i != playerId && aii.IsPlayerAttackable(static_cast<unsigned char>(i)))
            enemyMil = std::max(enemyMil, gwb.GetPlayer(i).GetStatisticCurrentValue(StatisticType::Military));
    bool underAttack = false;
    for(const nobMilitary* b : aii.GetMilitaryBuildings())
        if(b->IsUnderAttack())
        {
            underAttack = true;
            break;
        }
    const bool threatened = underAttack || enemyMil > mil + mil / 3;

    uint8_t front, mid, inland;
    if(threatened) // hold the line: fill the front, keep depth
    {
        front = 8;
        mid = 7;
        inland = 3;
    } else if(perBld < 2.5) // safe + soldier-poor -> grab land
    {
        front = 5;
        mid = 3;
        inland = 1;
    } else if(perBld < 5.0)
    {
        front = 7;
        mid = 6;
        inland = 2;
    } else // soldier-rich -> dig in
    {
        front = 8;
        mid = 8;
        inland = 4;
    }

    MilitarySettings m{};
    m[0] = static_cast<uint8_t>(clampi(strategy_.recruitRatio, 0, 10));
    m[1] = hasFrontier ? 5 : 0; // send strong soldiers to the front
    m[2] = 4;
    m[3] = 6;                                          // commit most available attackers
    m[4] = inland;
    m[5] = mid;
    m[6] = ggs.isEnabled(AddonId::SEA_ATTACK) ? 8 : 0;
    m[7] = std::min<uint8_t>(front, static_cast<uint8_t>(clampi(strategy_.frontierFill, 0, 8)));
    aii.ChangeMilitary(m);

    if(HasGold())
    {
        for(const nobMilitary* b : aii.GetMilitaryBuildings())
            aii.SetCoinsAllowed(b->GetPos(), b->GetFrontierDistance() != FrontierDistance::Far);
    }
}

void AIPlayerLlm::TryAttack()
{
    if(strategy_.attackAggression <= 0)
        return;

    const auto& militaryBuildings = aii.GetMilitaryBuildings();
    const unsigned numMilBlds = static_cast<unsigned>(militaryBuildings.size());
    if(numMilBlds == 0)
        return;
    const unsigned limit = 20 + strategy_.attackAggression * 4u; // check more buildings when aggressive

    // Collect candidate enemy targets seen from our frontier buildings (undefended HQs/harbors first).
    unsigned undefendedFirst = 0;
    std::vector<const nobBaseMilitary*> targets;
    for(const nobMilitary* milBld : militaryBuildings)
    {
        if(!AI::randomChance(numMilBlds, limit) || milBld->GetFrontierDistance() == FrontierDistance::Far)
            continue;
        const MapPoint src = milBld->GetPos();
        for(const nobBaseMilitary* target : gwb.LookForMilitaryBuildings(src, 2))
        {
            if(std::find(targets.begin(), targets.end(), target) != targets.end())
                continue;
            if(target->GetGOT() == GO_Type::NobMilitary && static_cast<const nobMilitary*>(target)->IsNewBuilt())
                continue;
            const MapPoint dest = target->GetPos();
            if(gwb.CalcDistance(src, dest) >= BASE_ATTACKING_DISTANCE || target->GetPlayer() == playerId
               || !aii.IsPlayerAttackable(target->GetPlayer()) || !aii.IsVisible(dest))
                continue;
            if(target->GetGOT() != GO_Type::NobMilitary && !target->DefendersAvailable())
            {
                targets.insert(targets.begin(), target); // undefended HQ/harbor: capture it
                ++undefendedFirst;
            } else
                targets.push_back(target);
        }
    }
    if(targets.empty())
        return;
    std::shuffle(targets.begin() + undefendedFirst, targets.end(), AI::getRandomGenerator());

    // How much advantage we demand before committing (more aggressive -> attack at parity).
    const double reqRatio = std::max(0.7, 1.5 - strategy_.attackAggression * 0.09);

    for(const nobBaseMilitary* target : targets)
    {
        const MapPoint dest = target->GetPos();
        unsigned attackersCount = 0;
        unsigned attackersStrength = 0;
        for(const nobBaseMilitary* mine : gwb.LookForMilitaryBuildings(dest, 2))
        {
            if(mine->GetPlayer() != playerId)
                continue;
            const auto* myMil = dynamic_cast<const nobMilitary*>(mine);
            if(!myMil || myMil->IsUnderAttack())
                continue;
            unsigned n = 0;
            attackersStrength += myMil->GetSoldiersStrengthForAttack(dest, n);
            attackersCount += n;
        }
        if(attackersCount == 0)
            continue;
        if(target->GetGOT() == GO_Type::NobMilitary)
        {
            const auto* enemy = static_cast<const nobMilitary*>(target);
            if(enemy->GetNumTroops() > 0 && attackersStrength <= enemy->GetSoldiersStrength() * reqRatio)
                continue;
        }
        aii.Attack(dest, attackersCount, true);
        ++numAttacksLaunched_;
        return; // one commitment per call
    }
}

bool AIPlayerLlm::FlagConnected(MapPoint flagPos) const
{
    const noFlag* flag = gwb.GetSpecObj<noFlag>(flagPos);
    if(!flag)
        return true; // site not materialised yet -> nothing to do
    for(const auto dir : helpers::enumRange<Direction>())
    {
        if(dir == Direction::NorthWest) // NW is the building<->flag link, not a road
            continue;
        if(flag->GetRoute(dir))
            return true;
    }
    return false;
}

bool AIPlayerLlm::ConnectFlag(MapPoint flagPos)
{
    const noFlag* srcFlag = gwb.GetSpecObj<noFlag>(flagPos);
    if(!srcFlag)
        return false;

    // Reference flag: nearest warehouse flag. Only connect to flags that already reach it, so we
    // join the main network instead of forming an isolated cluster.
    const noFlag* whFlag = nullptr;
    unsigned bestWhDist = UINT_MAX;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        const MapPoint wf = gwb.GetNeighbour(wh->GetPos(), Direction::SouthEast);
        const noFlag* f = gwb.GetSpecObj<noFlag>(wf);
        if(!f)
            continue;
        const unsigned d = gwb.CalcDistance(flagPos, wf);
        if(d < bestWhDist)
        {
            bestWhDist = d;
            whFlag = f;
        }
    }
    if(!whFlag)
        return false;

    std::vector<Direction> bestRoute;
    unsigned bestLen = UINT_MAX;
    unsigned considered = 0;
    for(const MapPoint pt : gwb.GetPointsInRadius(flagPos, 14))
    {
        const noFlag* cf = gwb.GetSpecObj<noFlag>(pt);
        if(!cf || cf->GetPlayer() != playerId || cf == srcFlag)
            continue;
        if(cf != whFlag && !aii.FindPathOnRoads(*cf, *whFlag))
            continue; // not part of the warehouse network
        if(aii.FindPathOnRoads(*srcFlag, *cf))
            continue; // already connected
        if(++considered > 24)
            break;
        std::vector<Direction> route;
        unsigned len = 0;
        if(!aii.FindFreePathForNewRoad(flagPos, pt, &route, &len))
            continue;
        if(len < bestLen)
        {
            bestLen = len;
            bestRoute = route;
        }
    }
    if(bestRoute.empty())
    {
        // Fallback: connect straight to the nearest warehouse flag, even if it's far. Without this,
        // frontier buildings beyond road range never join the network, never build, and pile up as
        // stuck sites that eventually freeze all construction.
        std::vector<Direction> route;
        unsigned len = 0;
        if(whFlag != srcFlag && aii.FindFreePathForNewRoad(flagPos, whFlag->GetPos(), &route, &len) && !route.empty())
            bestRoute = route;
    }
    if(bestRoute.empty())
        return false;
    aii.BuildRoad(flagPos, false, bestRoute);
    // Segment the road with flags so several carriers share it. A single long flagless road is a
    // severe throughput bottleneck that starves the weapons/food chains (one carrier per segment).
    MapPoint cur = flagPos;
    for(size_t i = 0; i < bestRoute.size(); ++i)
    {
        cur = gwb.GetNeighbour(cur, bestRoute[i]);
        if(i + 1 < bestRoute.size() && (i % 2) == 1)
            aii.SetFlag(cur);
    }
    return true;
}

void AIPlayerLlm::GarbageCollectStuckSites(unsigned gf)
{
    // Abandon building sites that never progress (unreachable, or starved of builders/materials).
    // Left alone they pile up, trip the open-sites ceiling, and freeze ALL construction - the exact
    // cause of the mid-game plateau. Normal builds finish in well under this window.
    constexpr unsigned stuckLimit = 12000; // ~10 min game time
    std::map<unsigned, unsigned> seen;
    for(const noBuildingSite* bs : aii.GetBuildingSites())
    {
        const unsigned idx = gwb.GetIdx(bs->GetPos());
        const auto it = siteFirstSeen_.find(idx);
        const unsigned first = (it != siteFirstSeen_.end()) ? it->second : gf;
        if(gf - first > stuckLimit)
            aii.DestroyBuilding(bs->GetPos()); // give up; the slot reopens for a reachable spot later
        else
            seen[idx] = first;
    }
    siteFirstSeen_.swap(seen);
}

void AIPlayerLlm::ConnectUnconnectedSites()
{
    unsigned budget = 6;
    for(const noBuildingSite* bs : aii.GetBuildingSites())
    {
        const MapPoint flagPos = gwb.GetNeighbour(bs->GetPos(), Direction::SouthEast);
        if(FlagConnected(flagPos))
            continue;
        ConnectFlag(flagPos);
        if(--budget == 0)
            break;
    }
}

} // namespace AIllm
