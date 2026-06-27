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

MapPoint AIPlayerLlm::AnchorPos() const
{
    if(const nobHQ* hq = aii.GetHeadquarter())
        return hq->GetPos();
    if(!aii.GetStorehouses().empty())
        return aii.GetStorehouses().front()->GetPos();
    if(!aii.GetMilitaryBuildings().empty())
        return aii.GetMilitaryBuildings().front()->GetPos();
    return MapPoint::Invalid();
}

Direction AIPlayerLlm::SectorOf(const MapPoint pt) const
{
    // Integer wrap-delta bucket (SPEC §5.2; D5). Duplicated from LlmStrategist's sectorOf so the
    // executor and the map digest agree on geometry without a shared symbol. No trig, no
    // GetShortestVector (MapBase lacks it) -> deterministic / replay-safe. Caller guarantees a valid
    // anchor, so this is only reached with a real HQ/storehouse origin.
    const MapPoint hq = AnchorPos();
    const MapExtent sz = gwb.GetSize();
    auto wrap = [](int d, int span) {
        if(d > span / 2)
            d -= span;
        if(d < -span / 2)
            d += span;
        return d;
    };
    const int dx = wrap(int(pt.x) - int(hq.x), sz.x);
    const int dy = wrap(int(pt.y) - int(hq.y), sz.y);
    const int yy = dy * 2; // hex rows are ~half-height
    if(dy < 0)             // upper half (smaller y = north)
        return (dx <= 0) ? ((-yy > -dx) ? Direction::NorthWest : Direction::West) :
                           ((-yy > dx) ? Direction::NorthEast : Direction::East);
    return (dx <= 0) ? ((yy > -dx) ? Direction::SouthWest : Direction::West) :
                       ((yy > dx) ? Direction::SouthEast : Direction::East);
}

bool AIPlayerLlm::hasAnyNonAutoRole() const
{
    for(const SectorRole r : strategy_.sectorRoles)
        if(r != SectorRole::Auto)
            return true;
    return false;
}

SectorRole AIPlayerLlm::RoleAt(const MapPoint pt) const
{
    if(!AnchorPos().isValid())
        return SectorRole::Auto;
    return strategy_.sectorRoles[SectorOf(pt)];
}

void AIPlayerLlm::RunGF(const unsigned gf, bool gfisnwf)
{
    curGf_ = gf;
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
        const EconStats s = gatherEconStats(ctx(), gf, llmDriven_);
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

    if(std::getenv("RTTR_LLM_DEBUG") && gf % 12500 == 0)
    {
        const EconStats s = gatherEconStats(ctx(), gf);
        // Iron-race diagnostics: own-territory iron/coal tiles near our centers (is the ore CLAIMED
        // yet?), mine sites in flight, and the distance from our anchor to the nearest UNCLAIMED iron
        // (how far the border still has to march). This is the binding early-tempo signal vs AIJH.
        unsigned ironTerr = 0, coalTerr = 0;
        std::vector<MapPoint> cs;
        if(aii.GetHeadquarter())
            cs.push_back(aii.GetHeadquarter()->GetPos());
        for(const nobMilitary* m : aii.GetMilitaryBuildings())
            cs.push_back(m->GetPos());
        for(const MapPoint c : cs)
            for(const MapPoint pt : gwb.GetPointsInRadius(c, 6))
                if(aii.IsOwnTerritory(pt))
                {
                    const auto r = aii.GetSubsurfaceResource(pt);
                    if(r == AISubSurfaceResource::Ironore)
                        ++ironTerr;
                    else if(r == AISubSurfaceResource::Coal)
                        ++coalTerr;
                }
        const MapPoint anchor = AnchorPos();
        const unsigned dIron =
          anchor.isValid() ? nearestUnclaimedResourceDist(ctx(), anchor, AISubSurfaceResource::Ironore, 50) : 255u;
        std::fprintf(stderr,
                     "[llm p%u gf%u] mil=%zu(blds) milSol=%u sites=%zu | ironTerr=%u coalTerr=%u distIron=%u | "
                     "ironM=%zu(s%u) coalM=%zu smelt=%zu(s%u) armory=%zu sw=%u sh=%u stone=%u | tooMany=%d\n",
                     playerId, gf, aii.GetMilitaryBuildings().size(),
                     player.GetStatisticCurrentValue(StatisticType::Military), aii.GetBuildingSites().size(), ironTerr,
                     coalTerr, dIron, aii.GetBuildings(BuildingType::IronMine).size(), CountSites(BuildingType::IronMine),
                     aii.GetBuildings(BuildingType::CoalMine).size(), aii.GetBuildings(BuildingType::Ironsmelter).size(),
                     CountSites(BuildingType::Ironsmelter), aii.GetBuildings(BuildingType::Armory).size(), s.swords,
                     s.shields, s.stones, TooManyOpenSites() ? 1 : 0);
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
        llmDriven_ = true;
        // Spatial district layout rides on the plan's sectorRoles, so it is meaningful only on the LLM
        // path. RTTR_LLM_NO_LAYOUT=1 is the A/B off-switch (floor-parity arm). Even when enabled, every
        // spatial term is additionally gated by hasAnyNonAutoRole(), so a plan that sets no roles is a
        // no-op too.
        const char* noLayout = std::getenv("RTTR_LLM_NO_LAYOUT");
        layoutEnabled_ = !(noLayout && *noLayout);
    } else
        strategist_ = std::make_unique<HeuristicStrategist>(persona);

    const EconStats s = gatherEconStats(ctx(), 0, llmDriven_);
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
    w[BuildingType::Quarry] = std::max(3u, R(2 + terr * 0.2));
    // Cap mines: coal only needs to feed the smelters/armories/mint, not blanket every ore node.
    // Uncapped coal floods the build queue and starves the rest of the weapons chain.
    w[BuildingType::CoalMine] = std::min(12u, std::max(2u, R(terr * 0.3 * mW)));
    w[BuildingType::IronMine] = std::min(14u, std::max(2u, R(terr * 0.28 * mW)));
    // STONE is the LLM's worst bottleneck: it gates military buildings (3-7 stone -> territory) AND
    // the smelter/armory chain (2 stone each -> swords -> soldiers). Quarries deplete, but under this
    // ruleset (gold->granite + inexhaustible mines) granite mines never run dry, so they are the
    // reliable stone backbone. Keep several running, scaling with the empire; ease off only when
    // sitting on a large stockpile.
    w[BuildingType::GraniteMine] = s.stones > 100 ? 1u : std::min(8u, std::max(2u, R(terr * 0.22)));
    // Food must keep the mines running, so scale it with both territory and the weapons chain.
    w[BuildingType::Farm] = std::max(2u, R(terr * 0.55 * eW));
    w[BuildingType::Fishery] = std::max(1u, R(terr * 0.25));
    w[BuildingType::Hunter] = 3u;
    if(s.hasGold)
        w[BuildingType::GoldMine] = static_cast<unsigned>(std::min(2.0, terr * 0.12));

    // Processors. CRITICAL: gate each chain link by the *finished* upstream supply (+1 buffer), NOT
    // by finished+sites. Gating off sites creates huge phantom demand (e.g. 14 iron-mine sites ->
    // 14 smelter wants -> 27 armory wants) that spawns dozens of build sites which can never finish
    // (no stone / no iron bars yet). Those stuck sites trip the open-sites ceiling and FREEZE all
    // construction and expansion - the measured mid-game plateau. Growing strictly off finished
    // upstream lets the chain bootstrap incrementally without clogging the queue.
    const unsigned nWoodc = NumBuildings(BuildingType::Woodcutter);
    const unsigned fIron = CountFinished(BuildingType::IronMine);
    const unsigned fSmelter = CountFinished(BuildingType::Ironsmelter);
    const unsigned fFarm = CountFinished(BuildingType::Farm);
    const unsigned fPig = CountFinished(BuildingType::PigFarm);
    const unsigned fGold = CountFinished(BuildingType::GoldMine);
    const bool haveIronMine = NumBuildings(BuildingType::IronMine) > 0;
    const bool haveFarm = NumBuildings(BuildingType::Farm) > 0;

    w[BuildingType::Sawmill] = std::max(1u, (nWoodc + 1) / 2);
    // Toolmakers convert iron into the tools that STAFF mines/smelters/armories. Too few -> the whole
    // specialist economy (especially the weapons chain) sits unstaffed. Scale with the empire.
    w[BuildingType::Metalworks] = std::min(5u, std::max(2u, R(terr * 0.12)));
    // Smelters track finished iron mines (one buffer site to stay slightly ahead). Armories track
    // finished smelters. This keeps the weapons chain growing without spawning unbuildable sites.
    w[BuildingType::Ironsmelter] = fIron > 0 ? std::min(fIron + 1, 10u) : (haveIronMine ? 1u : 0u);
    w[BuildingType::Armory] = fSmelter > 0 ? std::min(fSmelter * 2u, R(terr * 0.6 * mW)) : 0u;
    w[BuildingType::Brewery] = std::max(1u, R(terr * 0.18));
    w[BuildingType::Mill] = fFarm > 0 ? std::max(1u, (fFarm + 1) / 3) : (haveFarm ? 1u : 0u);
    w[BuildingType::Bakery] = fFarm > 0 ? std::max(1u, (fFarm + 1) / 3) : 0u;
    w[BuildingType::PigFarm] = fFarm > 1 ? std::max(1u, (fFarm + 1) / 4) : 0u;
    w[BuildingType::Slaughterhouse] = fPig > 0 ? std::max(1u, fPig) : 0u;
    w[BuildingType::Well] = std::max(1u, R(terr * 0.3));
    if(s.hasGold)
        w[BuildingType::Mint] = fGold > 0 ? std::max(1u, fGold) : 0u;

    // Logistics reach for large empires
    w[BuildingType::Storehouse] = static_cast<unsigned>(terr / 7);

    // Phase / economic-gambit overlay (plan-driven). Multiplies the ALREADY-GATED wants, so a gated
    // value of 0 stays 0 (no phantom smelter/armory sites can spawn -> the clog invariant holds). At
    // the default Phase::Auto (and Open) econMul=milMul=1.0, so every want is byte-identical to before.
    // Only active when an LLM plan drives the strategy (the pure heuristic floor adapts via the knobs).
    double econMul = 1.0, milMul = 1.0;
    if(llmDriven_)
    {
        switch(strategy_.phase)
        {
            case Phase::Auto:
            case Phase::Open:
            case Phase::Expand: break; // 1.0 / 1.0 (no-op)
            case Phase::Consolidate: econMul = 1.1; break;
            case Phase::Push:
                econMul = 0.95;
                milMul = 1.25;
                break;
            case Phase::Defend:
                econMul = 0.9;
                milMul = 1.2;
                break;
        }
        if(strategy_.economicGambit) // boom now, militarize once the timing trigger fires
        {
            econMul *= 1.2;
            milMul *= 0.85;
        }
    }
    if(econMul != 1.0)
    {
        w[BuildingType::Farm] = R(w[BuildingType::Farm] * econMul);
        w[BuildingType::Woodcutter] = R(w[BuildingType::Woodcutter] * econMul);
        w[BuildingType::Forester] = R(w[BuildingType::Forester] * econMul);
    }
    if(milMul != 1.0)
    {
        w[BuildingType::Armory] = R(w[BuildingType::Armory] * milMul);
        // Re-apply the existing mine caps after scaling so the coal/iron clog invariants still hold.
        w[BuildingType::CoalMine] = std::min(12u, R(w[BuildingType::CoalMine] * milMul));
        w[BuildingType::IronMine] = std::min(14u, R(w[BuildingType::IronMine] * milMul));
    }

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

    // Spatial layout active? (master gate). Precompute the feint enemy's HQ sector once (it depends on
    // our anchor + the feint enemy, never on the candidate pt), so the per-point loop stays cheap.
    const bool spatial = layoutEnabled_ && hasAnyNonAutoRole();
    Direction feintSector = Direction::West;
    bool haveFeintSector = false;
    if(spatial && strategy_.feintTargetEnemy >= 0 && AnchorPos().isValid())
    {
        const auto fid = static_cast<unsigned>(strategy_.feintTargetEnemy);
        if(fid < gwb.GetNumPlayers())
        {
            const MapPoint fhq = gwb.GetPlayer(fid).GetHQPos();
            if(fhq.isValid())
            {
                feintSector = SectorOf(fhq);
                haveFeintSector = true;
            }
        }
    }

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
            int v = aii.CalcResourceValue(pt, res);
            // March military expansion toward the nearest unclaimed ore we still need (iron is often
            // far from the start; AIJH reaches it because it expands toward resources, not just open
            // border). Crucially this bonus is added BEFORE the v<=0 gate, so spots that lean toward
            // ore but have little borderland value (into wilderness/mountains) still qualify - that is
            // what lets the border actually grow toward the iron mountain instead of only along the
            // existing front.
            // oreTarget_ is need-gated (set only while we still lack iron/coal/granite), so make the
            // pull DOMINANT over plain borderland: otherwise the border never marches to iron, it just
            // stumbles onto it after dozens of broad claims (~gf100k vs AIJH's ~gf50k). A strong, gently
            // decaying gradient turns early military expansion into a focused chain that secures the
            // weapons-chain ore first; it auto-disengages once we have enough mines (oreTarget_ invalid).
            if(oreTarget_.isValid() && BuildingProperties::IsMilitary(bt))
                v += std::max(0, 360 - static_cast<int>(gwb.CalcDistance(pt, oreTarget_)) * 6);

            // --- M5b spatial district biases (plan-driven). All composed BEFORE the v<=0 gate so they
            //     stack with the ore-march and so a strongly-penalised cramped spot drops out this pass
            //     (open ground wins; never a hard block - a later pass/anchor finds space). Master-gated
            //     on `spatial` (layoutEnabled_ && hasAnyNonAutoRole()): the heuristic never sets roles, so
            //     this whole region is inert on the floor path -> FindSite is byte-identical to M5a there.
            if(spatial)
            {
                const bool isMil = BuildingProperties::IsMilitary(bt);
                // 8.1 Farms-vs-foresters mutual repulsion + same-kind clustering. Farms and forest huts
                // fight over the SAME plantspace tiles; keeping them apart stops a farm landing in the
                // middle of the woodcutters' grove (the explicit goal). Always on (not role-gated) once
                // any role exists, since it is purely about co-location, not sector intent.
                if(bt == BuildingType::Farm)
                {
                    if(aii.isBuildingNearby(BuildingType::Forester, pt, 6))
                        v -= 120;
                    if(aii.isBuildingNearby(BuildingType::Woodcutter, pt, 5))
                        v -= 60;
                    if(aii.isBuildingNearby(BuildingType::Farm, pt, 7))
                        v += 25; // cluster the farm belt
                } else if(bt == BuildingType::Forester || bt == BuildingType::Woodcutter)
                {
                    if(aii.isBuildingNearby(BuildingType::Farm, pt, 6))
                        v -= 100;
                    if(aii.isBuildingNearby(BuildingType::Forester, pt, 4))
                        v += 30; // co-locate the forester camp
                }

                const SectorRole role = RoleAt(pt);
                if(!isMil)
                {
                    // 8.2 Sector-role term for economy/resource buildings.
                    const bool isFood = bt == BuildingType::Farm || bt == BuildingType::Fishery
                                        || bt == BuildingType::Hunter;
                    const bool isWood = bt == BuildingType::Forester || bt == BuildingType::Woodcutter;
                    switch(role)
                    {
                        case SectorRole::FarmBelt:
                            if(isFood)
                                v += 80;
                            if(isWood)
                                v -= 80;
                            break;
                        case SectorRole::ExpandEconomy:
                            if(isWood)
                                v += 40;
                            break;
                        case SectorRole::MiningOutpost: break; // smelter/armory ride the closest path (M5a)
                        case SectorRole::Ignore: v -= 200; break;
                        case SectorRole::Auto:
                        case SectorRole::MilitaryPush:
                        case SectorRole::Hold: break; // no economy bias
                    }
                } else
                {
                    // 8.3 Military sector bias, alongside the existing oreTarget_ bonus above.
                    switch(role)
                    {
                        case SectorRole::MilitaryPush: v += 120; break;
                        case SectorRole::Hold: v += 80; break;
                        case SectorRole::Ignore: v -= 200; break;
                        case SectorRole::Auto:
                        case SectorRole::ExpandEconomy:
                        case SectorRole::FarmBelt:
                        case SectorRole::MiningOutpost: break;
                    }
                    // Feint sector pull: nudge a few claims toward the feint enemy's bearing.
                    if(haveFeintSector && SectorOf(pt) == feintSector)
                        v += 70;
                    // primaryTarget_ directional mass (mirrors the ore-march). Set in TryExpand and
                    // reset there, so it only biases military FindSite calls during expansion.
                    if(primaryTarget_.isValid())
                        v += std::max(0, 130 - static_cast<int>(gwb.CalcDistance(pt, primaryTarget_)) * 3);
                }
            }

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
    // the weapons chain. Boards -> STONE -> base food -> WEAPONS -> more food -> rest. Stone is moved
    // up front because it gates military buildings and the smelter/armory chain (the LLM's chronic
    // starvation); without it the rest of the queue can't actually be built.
    static const BuildingType order[] = {
      BuildingType::Sawmill,    BuildingType::Woodcutter,    BuildingType::Forester,
      BuildingType::Quarry,     BuildingType::GraniteMine,   BuildingType::Well,
      BuildingType::Farm,       BuildingType::Mill,          BuildingType::Bakery,
      BuildingType::CoalMine,   BuildingType::IronMine,      BuildingType::Ironsmelter,
      BuildingType::Armory,     BuildingType::Brewery,       BuildingType::Fishery,
      BuildingType::Hunter,     BuildingType::PigFarm,
      BuildingType::Slaughterhouse, BuildingType::Metalworks,
      BuildingType::GoldMine,     BuildingType::Mint,
      BuildingType::Storehouse};

    // ROUND-ROBIN the chain: place at most ONE of each wanted type per outer lap, looping until the
    // budget is spent or a whole lap places nothing. The old front-loaded "up to 3 per type in one
    // pass" let the first few high-want types (woodcutter/forester/quarry want 10+) consume the whole
    // budget before reaching Farm (7th) -> farms starved (measured: 1 farm vs AIJH's 8 by gf40k) ->
    // no food -> no mines -> no weapons. Round-robin guarantees every link (esp. farms, then mines)
    // advances each plan tick, so the food->mine->weapons chain bootstraps on time.
    unsigned budget = 16;
    bool progressed = true;
    while(budget > 0 && progressed)
    {
        progressed = false;
        for(const BuildingType bt : order)
        {
            if(budget == 0)
                break;
            if(static_cast<int>(w[bt]) - static_cast<int>(NumBuildings(bt)) <= 0)
                continue;
            if(PlaceBuilding(bt, 11))
            {
                --budget;
                progressed = true;
            }
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

MapPoint AIPlayerLlm::NearestNeededOre(const EconStats& s) const
{
    const bool needIron = NumBuildings(BuildingType::IronMine) < 4;
    const bool needCoal = NumBuildings(BuildingType::CoalMine) < 5;
    const bool needGranite = NumBuildings(BuildingType::GraniteMine) < 2 && s.stones < 60;
    if(!needIron && !needCoal && !needGranite)
        return MapPoint::Invalid();

    // GetHeadquarter() is null-safe now, but storehouses (incl. HQ) are a robust center even after an
    // HQ loss, so use them.
    MapPoint center = MapPoint::Invalid();
    if(!aii.GetStorehouses().empty())
        center = aii.GetStorehouses().front()->GetPos();
    else if(!aii.GetMilitaryBuildings().empty())
        center = aii.GetMilitaryBuildings().front()->GetPos();
    if(!center.isValid())
        return MapPoint::Invalid();

    const AIContext c = ctx();
    // Iron is the long pole of the weapons chain, so prefer it; fall back to the closer of coal/granite.
    // Uses the shared radius-scan primitive (M4.1) so the ore-march and the digests agree on geometry.
    const MapPoint bestIron =
      needIron ? nearestUnclaimedResource(c, center, AISubSurfaceResource::Ironore, 42) : MapPoint::Invalid();
    if(bestIron.isValid())
        return bestIron;

    const MapPoint bestCoal =
      needCoal ? nearestUnclaimedResource(c, center, AISubSurfaceResource::Coal, 42) : MapPoint::Invalid();
    const MapPoint bestGranite =
      needGranite ? nearestUnclaimedResource(c, center, AISubSurfaceResource::Granite, 42) : MapPoint::Invalid();
    if(!bestCoal.isValid())
        return bestGranite;
    if(!bestGranite.isValid())
        return bestCoal;
    return gwb.CalcDistance(center, bestCoal) <= gwb.CalcDistance(center, bestGranite) ? bestCoal : bestGranite;
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
    // several claims per pass. Set oreTarget_ so FindSite marches the border toward the nearest
    // iron/coal/granite mountain we still need (securing the weapons chain early, like AIJH does).
    const EconStats s = gatherEconStats(ctx(), gf);
    oreTarget_ = NearestNeededOre(s);
    // Plan-driven primary-enemy directional mass: bias military FindSite toward the primary enemy's HQ
    // (mirrors the ore-march). Gated like the spatial terms (layout on + roles set) and on a valid enemy
    // HQ, so it is no-op on the floor path and when no plan names a primary target. Reset below alongside
    // oreTarget_ so it never leaks into economy/debug FindSite calls.
    primaryTarget_ = MapPoint::Invalid();
    if(layoutEnabled_ && hasAnyNonAutoRole() && strategy_.primaryTargetEnemy >= 0)
    {
        const auto pid = static_cast<unsigned>(strategy_.primaryTargetEnemy);
        if(pid < gwb.GetNumPlayers())
        {
            const MapPoint phq = gwb.GetPlayer(pid).GetHQPos();
            if(phq.isValid())
                primaryTarget_ = phq;
        }
    }
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
    oreTarget_ = MapPoint::Invalid();     // don't bias non-military FindSite calls (economy, debug)
    primaryTarget_ = MapPoint::Invalid(); // ditto: only active during military expansion
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

    // Plan-driven attack overlay (timing gate + posture). Only active when an LLM plan drives the
    // strategy; the pure heuristic floor keeps the original timing (always-on) and reqRatio. Each piece
    // is ALSO no-op at its own defaults (triggers 0, attackIntent Auto/Hold), so an LLM that emits no
    // attack intent matches the floor too.
    double reqRatioFloor = 1.50;
    unsigned intentLimitBonus = 0;
    if(llmDriven_)
    {
        // Timing gate: hold attacks until our army reaches a target size or the game reaches a target
        // minute. No-op when both triggers are 0 (the default).
        const unsigned myMil = player.GetStatisticCurrentValue(StatisticType::Military);
        if(strategy_.timingTriggerArmy > 0 || strategy_.timingTriggerMinute > 0)
        {
            const bool armyReady = strategy_.timingTriggerArmy > 0 && myMil >= strategy_.timingTriggerArmy;
            const bool timeReady =
              strategy_.timingTriggerMinute > 0 && curGf_ / 1200u >= strategy_.timingTriggerMinute;
            if(!armyReady && !timeReady)
                return;
        }

        // Posture. Auto/Hold keep the current behaviour byte-identical (reqRatioFloor 1.50, no limit
        // bonus); the stronger intents only LOOSEN (lower the demanded advantage, scan more buildings) so
        // the floor is never tightened. Hold's actual cap rides through attackAggression (capped in
        // ApplyFocusToKnobs), not here.
        switch(strategy_.attackIntent)
        {
            case AttackIntent::Probe: reqRatioFloor = 1.10; break;
            case AttackIntent::Commit:
                reqRatioFloor = 0.95;
                intentLimitBonus = 8;
                break;
            case AttackIntent::AllIn:
                reqRatioFloor = 0.75;
                intentLimitBonus = 20;
                break;
            case AttackIntent::Auto:
            case AttackIntent::Hold: break;
        }
    }

    const unsigned limit =
      20 + strategy_.attackAggression * 4u + intentLimitBonus; // check more buildings when aggressive

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

    // Plan-driven target ordering: after the shuffle (which keeps replays deterministic), bring the
    // primary enemy's buildings to the front of the defended tail and push the feint enemy's to the
    // back. stable_sort preserves the shuffled order within each tier. No-op at defaults (both targets
    // -1 => every tier()==1 => order unchanged), and the undefended-HQ prefix is never reordered. Gated
    // on llmDriven_ so the heuristic floor is byte-identical (it never sets target ids anyway).
    if(llmDriven_ && (strategy_.primaryTargetEnemy >= 0 || strategy_.feintTargetEnemy >= 0))
    {
        const int primary = strategy_.primaryTargetEnemy;
        const int feint = strategy_.feintTargetEnemy;
        const auto tier = [primary, feint](const nobBaseMilitary* t) -> int {
            const int owner = static_cast<int>(t->GetPlayer());
            if(owner == primary)
                return 0;
            if(owner == feint)
                return 2;
            return 1;
        };
        std::stable_sort(targets.begin() + undefendedFirst, targets.end(),
                         [&tier](const nobBaseMilitary* a, const nobBaseMilitary* b) { return tier(a) < tier(b); });
    }

    // How much advantage we demand before committing (more aggressive -> attack at parity). The attack
    // intent only loosens this floor; at default attackIntent=Auto, min(aggrRatio, 1.50)=aggrRatio,
    // identical to the prior single-expression form.
    const double aggrRatio = std::max(0.7, 1.5 - strategy_.attackAggression * 0.09);
    const double reqRatio = std::min(aggrRatio, reqRatioFloor);

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
