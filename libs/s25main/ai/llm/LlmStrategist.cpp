// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "LlmStrategist.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "ai/AIInterface.h"
#include "ai/AIResource.h"
#include "buildings/nobBaseWarehouse.h"
#include "buildings/nobHQ.h"
#include "buildings/nobMilitary.h"
#include "enum_cast.hpp"
#include "gameTypes/BuildingQuality.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/Direction.h"
#include "gameTypes/GameSettingTypes.h"
#include "gameTypes/StatisticTypes.h"
#include "world/GameWorldBase.h"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace AIllm {

namespace {
    const char* personaName(Persona p)
    {
        switch(p)
        {
            case Persona::Rusher: return "Rusher";
            case Persona::Boomer: return "Boomer";
            case Persona::Turtle: return "Turtle";
            case Persona::Expander: return "Expander";
            case Persona::Balanced:
            default: return "Balanced";
        }
    }
    Persona parsePersona(const std::string& s)
    {
        if(s == "Rusher")
            return Persona::Rusher;
        if(s == "Boomer")
            return Persona::Boomer;
        if(s == "Turtle")
            return Persona::Turtle;
        if(s == "Expander")
            return Persona::Expander;
        return Persona::Balanced;
    }
    std::string trim(const std::string& s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if(b == std::string::npos)
            return "";
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }
    const char* boolStr(bool b) { return b ? "true" : "false"; }
    unsigned countBld(const AIInterface& aii, BuildingType bt)
    {
        return static_cast<unsigned>(aii.GetBuildings(bt).size());
    }

    // Wire token for a Direction (the 6 HQ sectors). Used for mapDigest sector dirs + dirFromHq.
    const char* dirName(Direction d)
    {
        switch(d)
        {
            case Direction::West: return "West";
            case Direction::NorthWest: return "NorthWest";
            case Direction::NorthEast: return "NorthEast";
            case Direction::East: return "East";
            case Direction::SouthEast: return "SouthEast";
            case Direction::SouthWest: return "SouthWest";
        }
        return "West";
    }

    // Coarse HQ-centered sector for `pt` using integer wrap-delta bucketing (SPEC §5.2; D5). No trig
    // and no GetShortestVector (which MapBase lacks) -> deterministic / replay-safe. Roles are biases,
    // never hard filters, so the coarse split near the axes is acceptable.
    Direction sectorOf(const GameWorldBase& gwb, MapPoint hq, MapPoint pt)
    {
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
    bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "True" || v == "yes"; }

    const char* objectiveName(GameObjective o)
    {
        switch(o)
        {
            case GameObjective::Conquer3_4: return "Conquer3_4";
            case GameObjective::TotalDomination: return "TotalDomination";
            case GameObjective::EconomyMode: return "EconomyMode";
            case GameObjective::Tournament1:
            case GameObjective::Tournament2:
            case GameObjective::Tournament3:
            case GameObjective::Tournament4:
            case GameObjective::Tournament5: return "Tournament";
            case GameObjective::None:
            default: return "None";
        }
    }

    // FULL ruleset block (expensive/plan requests): the complete human-like picture (SPEC §2.1.1). All
    // addon IDs confirmed in const_addons.h; hasGold mirrors gatherEconStats (selection==0 -> gold).
    void appendRulesFull(std::ostringstream& o, const AIContext& ctx, const EconStats& s)
    {
        const GlobalGameSettings& g = ctx.ggs;
        o << "  \"rules\": {\"objective\": \"" << objectiveName(g.objective) << "\""
          << ", \"inexhaustibleMines\": " << boolStr(g.isEnabled(AddonId::INEXHAUSTIBLE_MINES))
          << ", \"inexhaustibleGraniteMines\": " << boolStr(g.isEnabled(AddonId::INEXHAUSTIBLE_GRANITEMINES))
          << ", \"inexhaustibleFish\": " << boolStr(g.isEnabled(AddonId::INEXHAUSTIBLE_FISH))
          << ", \"changeGoldDeposits\": " << g.getSelection(AddonId::CHANGE_GOLD_DEPOSITS)
          << ", \"hasGold\": " << boolStr(s.hasGold) << ", \"maxRank\": " << g.getSelection(AddonId::MAX_RANK)
          << ", \"halfCostMilEquip\": " << boolStr(g.isEnabled(AddonId::HALF_COST_MIL_EQUIP))
          << ", \"adjustMilitaryStrength\": " << boolStr(g.isEnabled(AddonId::ADJUST_MILITARY_STRENGTH))
          << ", \"militaryHitpoints\": " << boolStr(g.isEnabled(AddonId::MILITARY_HITPOINTS))
          << ", \"seaAttack\": " << boolStr(g.isEnabled(AddonId::SEA_ATTACK))
          << ", \"charburner\": " << boolStr(g.isEnabled(AddonId::CHARBURNER))
          << ", \"wine\": " << boolStr(g.isEnabled(AddonId::WINE))
          << ", \"leather\": " << boolStr(g.isEnabled(AddonId::LEATHER))
          << ", \"noArmorDefault\": " << boolStr(g.isEnabled(AddonId::NO_ARMOR_DEFAULT))
          << ", \"trade\": " << boolStr(g.isEnabled(AddonId::TRADE))
          << ", \"peacefulMode\": " << boolStr(g.isEnabled(AddonId::PEACEFULMODE)) << "},\n";
    }

    // SUBSET ruleset block (cheap/tick requests): only what the tactician needs (SPEC §2.1.1).
    void appendRulesLite(std::ostringstream& o, const AIContext& ctx, const EconStats& s)
    {
        const GlobalGameSettings& g = ctx.ggs;
        o << "  \"rules\": {\"objective\": \"" << objectiveName(g.objective) << "\""
          << ", \"inexhaustibleMines\": " << boolStr(g.isEnabled(AddonId::INEXHAUSTIBLE_MINES))
          << ", \"hasGold\": " << boolStr(s.hasGold) << ", \"maxRank\": " << g.getSelection(AddonId::MAX_RANK)
          << ", \"seaAttack\": " << boolStr(g.isEnabled(AddonId::SEA_ATTACK)) << "},\n";
    }

    // Wire token for a Phase (Auto is internal-only -> echoed as "Auto" for the model's reference).
    const char* phaseName(Phase p)
    {
        switch(p)
        {
            case Phase::Open: return "Open";
            case Phase::Expand: return "Expand";
            case Phase::Consolidate: return "Consolidate";
            case Phase::Push: return "Push";
            case Phase::Defend: return "Defend";
            case Phase::Auto:
            default: return "Auto";
        }
    }

    // --- Overlay enum parsers. Each returns `prior` on an unknown token, so a missing/garbled key
    //     keeps the previous value (the floor). Wire tokens are PINNED in SPEC §1.1. ---
    Phase parsePhase(const std::string& s, Phase prior)
    {
        if(s == "Open")
            return Phase::Open;
        if(s == "Expand")
            return Phase::Expand;
        if(s == "Consolidate")
            return Phase::Consolidate;
        if(s == "Push")
            return Phase::Push;
        if(s == "Defend")
            return Phase::Defend;
        return prior;
    }
    Focus parseFocus(const std::string& s, Focus prior)
    {
        if(s == "SecureIron")
            return Focus::SecureIron;
        if(s == "SecureCoal")
            return Focus::SecureCoal;
        if(s == "SecureStone")
            return Focus::SecureStone;
        if(s == "ExpandFront")
            return Focus::ExpandFront;
        if(s == "BoomEconomy")
            return Focus::BoomEconomy;
        if(s == "AttackEnemy")
            return Focus::AttackEnemy;
        if(s == "Defend")
            return Focus::Defend;
        if(s == "Raid")
            return Focus::Raid;
        if(s == "None")
            return Focus::None;
        return prior;
    }
    AttackIntent parseAttackIntent(const std::string& s, AttackIntent prior)
    {
        if(s == "Hold")
            return AttackIntent::Hold;
        if(s == "Probe")
            return AttackIntent::Probe;
        if(s == "Commit")
            return AttackIntent::Commit;
        if(s == "AllIn")
            return AttackIntent::AllIn;
        return prior;
    }
    SectorRole parseSectorRole(const std::string& s, SectorRole prior)
    {
        if(s == "MilitaryPush")
            return SectorRole::MilitaryPush;
        if(s == "ExpandEconomy")
            return SectorRole::ExpandEconomy;
        if(s == "FarmBelt")
            return SectorRole::FarmBelt;
        if(s == "MiningOutpost")
            return SectorRole::MiningOutpost;
        if(s == "Hold")
            return SectorRole::Hold;
        if(s == "Ignore")
            return SectorRole::Ignore;
        if(s == "Auto")
            return SectorRole::Auto;
        return prior;
    }
    DefensePosture bucketDefense(int v)
    {
        // The model emits a 0..10 int; bucket it (SPEC §1.1): 0..2 Loose, 3..6 Firm, 7..10 Fortress.
        if(v <= 2)
            return DefensePosture::Loose;
        if(v <= 6)
            return DefensePosture::Firm;
        return DefensePosture::Fortress;
    }
    // The model emits a bool-ish `expandIntent`. Map conservatively (SPEC §2.2): true keeps a strong
    // prior (Hard) else Steady; false sets Halt ONLY when the same tick is clearly defensive
    // (defense>=Fortress or attackIntent==Hold), otherwise Auto — never silently halt expansion.
    ExpandIntent parseExpandIntentBool(const std::string& v, const TickStrategy& tick)
    {
        if(parseBool(v))
            return (tick.expandIntent == ExpandIntent::Hard) ? ExpandIntent::Hard : ExpandIntent::Steady;
        const bool defensive = tick.defense == DefensePosture::Fortress || tick.attackIntent == AttackIntent::Hold;
        return defensive ? ExpandIntent::Halt : ExpandIntent::Auto;
    }
    // Validate a target player id: -1 means none; otherwise must be in [0, nPlayers) and NOT ourselves
    // (a self-target would point FindSite's primary-mass at our own HQ). Anything else -> -1 (B7).
    int clampPlayer(int v, unsigned nPlayers, unsigned self)
    {
        if(v < 0 || static_cast<unsigned>(v) >= nPlayers || static_cast<unsigned>(v) == self)
            return -1;
        return v;
    }
    // Escape a free-text string for embedding in JSON: escape " and \, strip control chars (D7/§6.4).
    std::string jsonEsc(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for(const char c : in)
        {
            if(c == '"' || c == '\\')
            {
                out += '\\';
                out += c;
            } else if(static_cast<unsigned char>(c) >= 0x20)
                out += c;
            // control chars (< 0x20) are dropped
        }
        return out;
    }
} // namespace

LlmStrategist::LlmStrategist(unsigned char playerId, std::string spoolDir, unsigned blockMs, Persona persona)
    : playerId_(playerId), spoolDir_(std::move(spoolDir)), blockMs_(blockMs), fallback_(persona)
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(spoolDir_, ec);
}

std::string LlmStrategist::requestPath(unsigned gf) const
{
    return spoolDir_ + "/req_p" + std::to_string(playerId_) + "_" + std::to_string(gf) + ".json";
}
std::string LlmStrategist::responsePath(unsigned gf) const
{
    return spoolDir_ + "/resp_p" + std::to_string(playerId_) + "_" + std::to_string(gf) + ".txt";
}

void LlmStrategist::appendOpponents(std::ostream& o, const OpponentDigest& od, bool full)
{
    auto emit = [&](const OpponentInfo& oi, bool last) {
        o << "{\"id\": " << oi.id << ", \"military\": " << oi.military << ", \"buildings\": " << oi.buildings;
        if(full)
            o << ", \"country\": " << oi.country;
        o << ", \"trend\": \"" << (oi.trend > 0 ? "growing" : (oi.trend < 0 ? "shrinking" : "steady")) << "\""
          << ", \"attackingUs\": " << boolStr(oi.attackingUs);
        if(full)
        {
            o << ", \"dirFromHq\": \"" << dirName(static_cast<Direction>(oi.dirFromHq >= 0 ? oi.dirFromHq : 0)) << "\""
              << ", \"distFromHq\": " << oi.distFromHq;
        }
        o << "}" << (last ? "" : ", ");
    };

    o << "  \"opponents\": [";
    if(full)
    {
        for(size_t i = 0; i < od.enemies.size(); ++i)
            emit(od.enemies[i], i + 1 == od.enemies.size());
    } else
    {
        // Condensed top-2 by threat (military*2 + attackingUs*1000 - distFromHq), dropping the spatials.
        std::vector<const OpponentInfo*> ranked;
        ranked.reserve(od.enemies.size());
        for(const OpponentInfo& oi : od.enemies)
            ranked.push_back(&oi);
        auto threat = [](const OpponentInfo* a) {
            return static_cast<int>(a->military) * 2 + (a->attackingUs ? 1000 : 0) - a->distFromHq;
        };
        std::stable_sort(ranked.begin(), ranked.end(),
                         [&](const OpponentInfo* a, const OpponentInfo* b) { return threat(a) > threat(b); });
        const size_t n = std::min<size_t>(2, ranked.size());
        for(size_t i = 0; i < n; ++i)
            emit(*ranked[i], i + 1 == n);
    }
    o << "],\n";
}

void LlmStrategist::appendMapDigest(std::ostream& o, const MapDigest& md)
{
    int openest = -1, openestRoom = -1, enemySec = -1, enemyDist = 1 << 30;
    o << "  \"mapDigest\": {\"hqValid\": " << boolStr(md.valid) << ", \"sectors\": [";
    for(int i = 0; i < 6; ++i)
    {
        const SectorDigest& sd = md.sectors[i];
        o << "{\"dir\": \"" << dirName(static_cast<Direction>(i)) << "\", \"index\": " << i
          << ", \"room\": " << sd.room << ", \"iron\": " << (sd.nearestIron < 0 ? 255 : sd.nearestIron)
          << ", \"coal\": " << (sd.nearestCoal < 0 ? 255 : sd.nearestCoal)
          << ", \"granite\": " << (sd.nearestGranite < 0 ? 255 : sd.nearestGranite)
          << ", \"stone\": " << (sd.nearestStone < 0 ? 255 : sd.nearestStone)
          << ", \"water\": " << boolStr(sd.hasWater) << ", \"enemyDir\": " << sd.enemyDir
          << ", \"enemyDist\": " << (sd.enemyDist < 0 ? 255 : sd.enemyDist)
          << ", \"chokepoint\": " << boolStr(sd.chokepoint) << "}" << (i == 5 ? "" : ", ");
        if(sd.room > openestRoom)
        {
            openestRoom = sd.room;
            openest = i;
        }
        if(sd.enemyDir >= 0 && sd.enemyDist >= 0 && sd.enemyDist < enemyDist)
        {
            enemyDist = sd.enemyDist;
            enemySec = i;
        }
    }
    o << "], \"mapHints\": {\"openestSector\": \"" << dirName(static_cast<Direction>(openest >= 0 ? openest : 0))
      << "\", \"enemySector\": \"" << dirName(static_cast<Direction>(enemySec >= 0 ? enemySec : 0)) << "\"}},\n";
}

void LlmStrategist::writeRequest(unsigned gf, const AIContext& ctx, const EconStats& s, bool contained,
                                 const Strategy& cur, Tier tier, RequestKind kind) const
{
    const AIInterface& aii = ctx.aii;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": 2,\n";
    o << "  \"tier\": \"" << (tier == Tier::Expensive ? "expensive" : "cheap") << "\",\n";
    o << "  \"requestKind\": \"" << (kind == RequestKind::Plan ? "plan" : "tick") << "\",\n";
    o << "  \"player\": " << static_cast<unsigned>(playerId_) << ",\n";
    o << "  \"gf\": " << gf << ",\n";
    o << "  \"minutes\": " << gf / 1200 << ",\n";
    o << "  \"persona\": \"" << personaName(cur.persona) << "\",\n";
    o << "  \"contained\": " << boolStr(contained) << ",\n";
    // FULL ruleset on an expensive plan, the lite subset on a cheap tick (SPEC §2.1.1).
    if(kind == RequestKind::Plan)
        appendRulesFull(o, ctx, s);
    else
        appendRulesLite(o, ctx, s);
    // weaponsThroughput: bottleneck stage of the weapons chain (no new bookkeeping; SPEC §2.1.2).
    const unsigned weaponsThroughput = std::min({s.coalMines, s.ironMines, s.ironsmelters});
    o << "  \"self\": {"
      << "\"militaryBuildings\": " << s.nMil << ", \"militarySites\": " << s.nMilSites
      << ", \"storehouses\": " << s.nStore << ", \"buildings\": " << s.myBuildings
      << ", \"militaryStrength\": " << s.myMilitary << ", \"reserveSoldiers\": " << s.soldiers
      << ", \"boards\": " << s.boards << ", \"stones\": " << s.stones << ", \"swords\": " << s.swords
      << ", \"shields\": " << s.shields << ", \"beer\": " << s.beer << ", \"helpers\": " << s.helpers
      << ", \"woodcutters\": " << countBld(aii, BuildingType::Woodcutter)
      << ", \"sawmills\": " << countBld(aii, BuildingType::Sawmill)
      << ", \"foresters\": " << countBld(aii, BuildingType::Forester)
      << ", \"quarries\": " << countBld(aii, BuildingType::Quarry)
      << ", \"farms\": " << countBld(aii, BuildingType::Farm) << ", \"coalMines\": " << s.coalMines
      << ", \"ironMines\": " << s.ironMines << ", \"smelters\": " << s.ironsmelters
      << ", \"armories\": " << s.armories << ", \"breweries\": " << countBld(aii, BuildingType::Brewery)
      << ", \"nearestIronDist\": " << s.nearestIronDist << ", \"nearestCoalDist\": " << s.nearestCoalDist
      << ", \"nearestStoneDist\": " << s.nearestStoneDist << ", \"nearestGraniteDist\": " << s.nearestGraniteDist
      << ", \"stoneStarved\": " << boolStr(s.stoneStarved) << ", \"ironChainBroken\": " << boolStr(s.ironChainBroken)
      << ", \"foodSecure\": " << boolStr(s.foodSecure) << ", \"weaponsThroughput\": " << weaponsThroughput << "},\n";
    o << "  \"enemy\": {\"bestMilitary\": " << s.bestEnemyMilitary << ", \"bestBuildings\": " << s.bestEnemyBuildings
      << "},\n";
    // opponents[]: FULL per-enemy on a plan; condensed top-2 by threat on a cheap tick (SPEC §2.1.3).
    appendOpponents(o, oppDigest_, kind == RequestKind::Plan);
    // mapDigest: expensive/plan ONLY, and only once it has been built (cached). Omitted on cheap ticks.
    if(tier == Tier::Expensive && mapDigest_.valid)
        appendMapDigest(o, mapDigest_);
    o << "  \"currentStrategy\": {"
      << "\"persona\": \"" << personaName(cur.persona) << "\""
      << ", \"expansionAggression\": " << cur.expansionAggression << ", \"economyFocus\": " << cur.economyFocus
      << ", \"militaryFocus\": " << cur.militaryFocus << ", \"attackAggression\": " << cur.attackAggression
      << ", \"recruitRatio\": " << cur.recruitRatio << ", \"frontierFill\": " << cur.frontierFill
      << ", \"wantExpand\": " << boolStr(cur.wantExpand) << "},\n";
    // Echo the live GamePlan (or null) so the model can adapt its own prior plan (continuity).
    if(plan_.valid)
    {
        o << "  \"currentPlan\": {"
          << "\"name\": \"" << jsonEsc(plan_.strategyName) << "\""
          << ", \"phase\": \"" << phaseName(plan_.phase) << "\""
          << ", \"primaryTarget\": " << plan_.primaryTargetEnemy
          << ", \"feintTarget\": " << plan_.feintTargetEnemy << ", \"timingArmy\": " << plan_.timingArmy
          << ", \"timingMinute\": " << plan_.timingMinute
          << ", \"economicGambit\": " << boolStr(plan_.economicGambit)
          << ", \"ageGf\": " << (gf >= plan_.planGf ? gf - plan_.planGf : 0u) << "}\n";
    } else
        o << "  \"currentPlan\": null\n";
    o << "}\n";

    const std::string path = requestPath(gf);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if(!f)
            return;
        f << o.str();
    }
    std::rename(tmp.c_str(), path.c_str());
}

bool LlmStrategist::tryReadResponse(unsigned gf, Strategy& out, GamePlan& plan, TickStrategy& tick,
                                    std::string& chat)
{
    const std::string path = responsePath(gf);
    std::ifstream f(path);
    if(!f)
        return false;

    auto toInt = [](const std::string& v, int def) {
        try
        {
            return std::stoi(v);
        } catch(...)
        {
            return def;
        }
    };
    auto cl = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

    // requestReplan never keeps a prior value (D10): reset before parsing so it can only latch true
    // if this very response asks for it.
    tick.requestReplan = false;

    bool parsedAny = false;     // any non-empty k=v line (file is complete, not mid-write)
    bool parsedKnown = false;   // at least one RECOGNISED key (B3: don't latch haveLlm_ on foreign junk)
    bool parsedPlanKey = false;
    // A4: stash expandIntent raw and resolve it AFTER the loop, once defense/attackIntent are final
    // (the sidecar may emit expandIntent before them; in-file-order parsing would otherwise read stale ones).
    std::string rawExpand;
    bool haveRawExpand = false;
    std::string line;
    while(std::getline(f, line))
    {
        const auto eq = line.find('=');
        if(eq == std::string::npos)
            continue;
        const std::string k = trim(line.substr(0, eq));
        const std::string v = trim(line.substr(eq + 1));
        if(k.empty())
            continue;
        parsedAny = true;
        // --- Shared knobs (valid in plan and tick) ---
        if(k == "persona")
            out.persona = parsePersona(v);
        else if(k == "expansionAggression")
            out.expansionAggression = cl(toInt(v, out.expansionAggression), 0, 10);
        else if(k == "economyFocus")
            out.economyFocus = cl(toInt(v, out.economyFocus), 0, 10);
        else if(k == "militaryFocus")
            out.militaryFocus = cl(toInt(v, out.militaryFocus), 0, 10);
        else if(k == "attackAggression")
            out.attackAggression = cl(toInt(v, out.attackAggression), 0, 10);
        else if(k == "recruitRatio")
            out.recruitRatio = cl(toInt(v, out.recruitRatio), 0, 10);
        else if(k == "frontierFill")
            out.frontierFill = cl(toInt(v, out.frontierFill), 0, 8);
        else if(k == "wantExpand")
            out.wantExpand = parseBool(v);
        else if(k == "chat")
            chat = v;
        // --- GamePlan keys ---
        else if(k == "phase")
        {
            plan.phase = parsePhase(v, plan.phase);
            parsedPlanKey = true;
        } else if(k == "primaryTargetEnemy")
        {
            plan.primaryTargetEnemy =
              clampPlayer(toInt(v, plan.primaryTargetEnemy), numPlayers_, static_cast<unsigned>(playerId_));
            parsedPlanKey = true;
        } else if(k == "feintTargetEnemy")
        {
            plan.feintTargetEnemy =
              clampPlayer(toInt(v, plan.feintTargetEnemy), numPlayers_, static_cast<unsigned>(playerId_));
            parsedPlanKey = true;
        } else if(k == "timingTriggerArmy")
        {
            plan.timingArmy = static_cast<unsigned>(cl(toInt(v, static_cast<int>(plan.timingArmy)), 0, 5000));
            parsedPlanKey = true;
        } else if(k == "timingTriggerMinute")
        {
            plan.timingMinute = static_cast<unsigned>(cl(toInt(v, static_cast<int>(plan.timingMinute)), 0, 600));
            parsedPlanKey = true;
        } else if(k == "economicGambit")
        {
            plan.economicGambit = parseBool(v);
            parsedPlanKey = true;
        } else if(k == "strategyName")
        {
            plan.strategyName = v;
            parsedPlanKey = true;
        } else if(k == "sectorRoles")
        {
            // One CSV[6] line; up to 6 tokens, each parsed (junk -> Auto). Short -> remaining stay Auto.
            std::stringstream ss(v);
            std::string tok;
            unsigned idx = 0;
            while(idx < plan.sectorRoles.size() && std::getline(ss, tok, ','))
            {
                plan.sectorRoles.data()[idx] = parseSectorRole(trim(tok), SectorRole::Auto);
                ++idx;
            }
            parsedPlanKey = true;
        }
        // --- TickStrategy keys ---
        else if(k == "diagnosis")
            tick.diagnosis = v;
        else if(k == "focusPrimary")
            tick.focusPrimary = parseFocus(v, tick.focusPrimary);
        else if(k == "focusSecondary")
            tick.focusSecondary = parseFocus(v, tick.focusSecondary);
        else if(k == "attackIntent")
            tick.attackIntent = parseAttackIntent(v, tick.attackIntent);
        else if(k == "defense")
            tick.defense = bucketDefense(cl(toInt(v, 5), 0, 10));
        else if(k == "expandIntent")
        {
            rawExpand = v; // A4: defer resolution until after the loop
            haveRawExpand = true;
        } else if(k == "requestReplan")
            tick.requestReplan = parseBool(v);
        else if(k == "notice")  // sidecar system event (tier fallback/recovery) -> surface in-game
            pendingNotice_ = v;
        else
            continue;        // unknown key -> ignore, do NOT count as a known/usable plan (B3)
        parsedKnown = true;  // reached only when a recognised key matched
    }
    f.close();
    if(!parsedAny)
        return false; // empty/whitespace-only: likely mid-write, retry next tick (do NOT consume)

    // A4: now that defense/attackIntent are final, resolve the deferred expandIntent bool.
    if(haveRawExpand)
        tick.expandIntent = parseExpandIntentBool(rawExpand, tick);

    // The file had content: consume it so we don't re-read it forever (even if it was all junk, which
    // would otherwise wedge pending_). Then decide usability.
    std::remove(path.c_str());
    std::remove(requestPath(gf).c_str());
    if(!parsedKnown)
        return false; // only unknown keys -> treat as no usable response (don't latch haveLlm_)

    // Mark the plan live once the expensive tier ever produced any plan key.
    if(parsedPlanKey)
    {
        plan.valid = true;
        plan.planGf = gf;
        // Announce a genuinely NEW strategic plan in-game (unless a system notice already claimed this
        // tick's message). Important AI event the player should see.
        if(!plan.strategyName.empty() && plan.strategyName != lastAnnouncedPlan_)
        {
            lastAnnouncedPlan_ = plan.strategyName;
            if(pendingNotice_.empty())
                pendingNotice_ = "New strategy: " + plan.strategyName + (chat.empty() ? "" : " - " + chat);
        }
    }
    return true;
}

bool LlmStrategist::consume(unsigned gf, Strategy& base)
{
    // Apply a previously-requested answer onto the held contracts. The knob plan starts from the last
    // good LLM knobs (or the heuristic baseline if we never had one); plan_/tick_ are updated in place
    // (plan_ is retained across ticks, overwritten only by plan keys; tick_ is per-tick).
    Strategy knobs = haveLlm_ ? llmStrategy_ : base;
    std::string chat;
    if(!tryReadResponse(pendingGf_, knobs, plan_, tick_, chat))
        return false;
    llmStrategy_ = knobs;
    haveLlm_ = true;
    lastLlmGf_ = gf;
    pending_ = false;
    if(pendingKind_ == RequestKind::Plan)
    {
        lastPlanGf_ = gf;
        escalatePending_ = false;
        plan_.rationale = chat;
    }
    // Prefer the freshest free text for narration: tick diagnosis, then plan rationale, then chat.
    if(!tick_.diagnosis.empty())
        rationale_ = tick_.diagnosis;
    else if(!plan_.rationale.empty())
        rationale_ = plan_.rationale;
    else if(!chat.empty())
        rationale_ = chat;
    else
        rationale_ = "Plan updated.";
    return true;
}

void LlmStrategist::projectPlanAndTick(Strategy& out) const
{
    // Copy intent from the held contracts onto `out` so ApplyFocusToKnobs + the executor see them.
    out.phase = plan_.valid ? plan_.phase : Phase::Auto;
    out.focusPrimary = tick_.focusPrimary;
    out.focusSecondary = tick_.focusSecondary;
    out.expandIntent = tick_.expandIntent;
    out.attackIntent = tick_.attackIntent;
    out.defense = tick_.defense;

    out.primaryTargetEnemy = plan_.primaryTargetEnemy;
    out.feintTargetEnemy = plan_.feintTargetEnemy;
    out.timingTriggerArmy = plan_.timingArmy;
    out.timingTriggerMinute = plan_.timingMinute;
    out.economicGambit = plan_.economicGambit;
    out.sectorRoles = plan_.sectorRoles;
    out.diagnosis = tick_.diagnosis;
}

void LlmStrategist::buildMapDigest(const AIContext& ctx, unsigned gf)
{
    const AIInterface& aii = ctx.aii;
    const GameWorldBase& gwb = ctx.gwb;

    // Robust HQ-guarded center: the HQ if alive, else any storehouse (incl. a captured one). If neither
    // exists (HQ lost, no stores) we keep the previous digest rather than wiping it (SPEC §5.3).
    MapPoint hq = MapPoint::Invalid();
    if(const nobHQ* h = aii.GetHeadquarter())
        hq = h->GetPos();
    else if(!aii.GetStorehouses().empty())
        hq = aii.GetStorehouses().front()->GetPos();
    if(!hq.isValid())
    {
        mapDigest_.valid = false;
        mapDigest_.builtGf = gf; // remember we tried, so we don't rescan every tick while HQ-less
        return;
    }

    MapDigest d;
    d.valid = true;
    d.hq = hq;
    d.scanRadius = scanRadius_;
    d.builtGf = gf;

    // Single bounded pass: bucket each point into its sector, accumulate open room + nearest resources.
    for(const MapPoint pt : gwb.GetPointsInRadius(hq, scanRadius_))
    {
        const Direction sec = sectorOf(gwb, hq, pt);
        SectorDigest& sd = d.sectors[rttr::enum_cast(sec)];
        if(gwb.IsSeaPoint(pt))
            sd.hasWater = true;
        if(aii.IsOwnTerritory(pt))
            continue; // owned ground is already secured; the digest cares about reachable new land
        if(aii.GetBuildingQualityAnyOwner(pt) >= BuildingQuality::Hut)
            ++sd.room;
        const unsigned dist = gwb.CalcDistance(hq, pt);
        const int di = static_cast<int>(dist);
        switch(aii.GetSubsurfaceResource(pt))
        {
            case AISubSurfaceResource::Ironore:
                if(sd.nearestIron < 0 || di < sd.nearestIron)
                    sd.nearestIron = di;
                break;
            case AISubSurfaceResource::Coal:
                if(sd.nearestCoal < 0 || di < sd.nearestCoal)
                    sd.nearestCoal = di;
                break;
            case AISubSurfaceResource::Granite:
                if(sd.nearestGranite < 0 || di < sd.nearestGranite)
                    sd.nearestGranite = di;
                break;
            default: break;
        }
        if(aii.GetSurfaceResource(pt) == AISurfaceResource::Stones)
            if(sd.nearestStone < 0 || di < sd.nearestStone)
                sd.nearestStone = di;
    }

    // O(P) enemy-HQ pass: assign each attackable enemy's HQ to its sector (nearest one wins per sector).
    for(unsigned i = 0; i < numPlayers_; ++i)
    {
        if(i == ctx.playerId || !aii.IsPlayerAttackable(static_cast<unsigned char>(i)))
            continue;
        const MapPoint ehq = gwb.GetPlayer(i).GetHQPos();
        if(!ehq.isValid())
            continue;
        const Direction sec = sectorOf(gwb, hq, ehq);
        SectorDigest& sd = d.sectors[rttr::enum_cast(sec)];
        const int dist = static_cast<int>(gwb.CalcDistance(hq, ehq));
        if(sd.enemyDir < 0 || dist < sd.enemyDist)
        {
            sd.enemyDir = static_cast<int>(i);
            sd.enemyDist = dist;
        }
    }

    // A sector that is both tight (little open room) and faces an enemy is a defensible neck.
    for(SectorDigest& sd : d.sectors)
        sd.chokepoint = sd.room < chokeRoomThresh_ && sd.enemyDir >= 0;

    mapDigest_ = d;
}

void LlmStrategist::buildOpponentDigest(const AIContext& ctx, unsigned gf, const EconStats& /*s*/)
{
    const AIInterface& aii = ctx.aii;
    const GameWorldBase& gwb = ctx.gwb;

    // attackingUs proxy: true if any of our military buildings is currently under attack (SPEC §2.1.3).
    bool anyUnderAttack = false;
    for(const nobMilitary* b : aii.GetMilitaryBuildings())
    {
        if(b->IsUnderAttack())
        {
            anyUnderAttack = true;
            break;
        }
    }

    MapPoint hq = MapPoint::Invalid();
    if(const nobHQ* h = aii.GetHeadquarter())
        hq = h->GetPos();
    else if(!aii.GetStorehouses().empty())
        hq = aii.GetStorehouses().front()->GetPos();

    OpponentDigest od;
    od.builtGf = gf;
    unsigned strongest = 0, weakest = 0xffffffffu, closest = 0xffffffffu;
    for(unsigned i = 0; i < numPlayers_; ++i)
    {
        if(i == ctx.playerId || !aii.IsPlayerAttackable(static_cast<unsigned char>(i)))
            continue;
        const GamePlayer& op = gwb.GetPlayer(i);
        const MapPoint ehq = op.GetHQPos();
        if(!ehq.isValid())
            continue; // an eliminated/HQ-less enemy is not a live opponent for planning

        OpponentInfo oi;
        oi.id = static_cast<int>(i);
        oi.military = op.GetStatisticCurrentValue(StatisticType::Military);
        oi.buildings = op.GetStatisticCurrentValue(StatisticType::Buildings);
        oi.country = op.GetStatisticCurrentValue(StatisticType::Country);

        // Trend vs last tick, dead-banded by cur/16 (avoids flapping on tiny swaps).
        const unsigned prev = (i < lastEnemyMil_.size()) ? lastEnemyMil_[i] : 0u;
        const unsigned band = oi.military / 16;
        if(oi.military > prev + band)
            oi.trend = 1;
        else if(prev > oi.military + band)
            oi.trend = -1;
        else
            oi.trend = 0;

        // Bearing/distance from our HQ (255 / -1 if our HQ is lost).
        if(hq.isValid())
        {
            oi.dirFromHq = static_cast<int>(rttr::enum_cast(sectorOf(gwb, hq, ehq)));
            oi.distFromHq = static_cast<int>(gwb.CalcDistance(hq, ehq));
        } else
        {
            oi.dirFromHq = -1;
            oi.distFromHq = 255;
        }

        od.enemies.push_back(oi);
        if(oi.military > strongest || od.strongestId < 0)
        {
            strongest = oi.military;
            od.strongestId = oi.id;
        }
        if(oi.military < weakest)
        {
            weakest = oi.military;
            od.weakestId = oi.id;
        }
        if(static_cast<unsigned>(oi.distFromHq) < closest)
        {
            closest = static_cast<unsigned>(oi.distFromHq);
            od.closestId = oi.id;
        }
    }

    // attackingUs: attribute the proxy to the closest enemy (the most plausible aggressor).
    if(anyUnderAttack && od.closestId >= 0)
    {
        for(OpponentInfo& oi : od.enemies)
            if(oi.id == od.closestId)
                oi.attackingUs = true;
    }

    // Refresh per-player trend memory for the next tick.
    for(const OpponentInfo& oi : od.enemies)
        if(static_cast<unsigned>(oi.id) < lastEnemyMil_.size())
            lastEnemyMil_[oi.id] = oi.military;

    oppDigest_ = od;
}

bool LlmStrategist::wantExpensivePlan(unsigned gf, const EconStats& /*s*/) const
{
    // Respect the minimum gap between expensive requests (budget + reproducibility). Gate on
    // planEverRequested_ (not lastPlanReqGf_!=0) so the gf-0 opening, which sets lastPlanReqGf_=0,
    // doesn't read as "never requested" and let escalations fire back-to-back early game (A3).
    if(planEverRequested_ && gf - lastPlanReqGf_ < planMinGapGf_)
        return false;
    if(gf == 0)
        return true; // the opening always wants a full plan
    if(!plan_.valid)
        return true; // we have never had a plan
    if(escalatePending_)
        return true; // an event asked for a re-think
    return gf - lastPlanGf_ >= replanIntervalGf_; // periodic deep-think
}

void LlmStrategist::detectEvents(unsigned /*gf*/, const EconStats& s)
{
    // Compare against the prior snapshot. The first tick's snapshot is all-zero, against which none of
    // these (drop below, lose >=2, surge over) can be true, so no false event fires at the opening.
    const bool milDrop = s.myMilitary + s.myMilitary / 5 < lastSnapMil_; // > ~20% drop
    const bool blocksLost = s.nMil + 2 <= lastSnapNMil_;                 // >= 2 mil buildings lost
    const bool enemySurge = s.bestEnemyMilitary > lastSnapBestEnemyMil_ + lastSnapBestEnemyMil_ / 4;
    if(milDrop || blocksLost || enemySurge)
        escalatePending_ = true;
    if(tick_.requestReplan)
        escalatePending_ = true;

    lastSnapMil_ = s.myMilitary;
    lastSnapNMil_ = s.nMil;
    lastSnapBestEnemyMil_ = s.bestEnemyMilitary;
}

std::string LlmStrategist::takeImportantMessage()
{
    std::string m;
    m.swap(pendingNotice_); // hand it off once (cleared so we don't repeat it)
    return m;
}

void LlmStrategist::Update(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                           Strategy& strategy)
{
    // Size player-dependent memory once (clamps target ids + per-player trend memory).
    if(numPlayers_ == 0)
    {
        numPlayers_ = ctx.gwb.GetNumPlayers();
        lastEnemyMil_.assign(numPlayers_, 0u);
    }

    // Always compute a safe heuristic baseline so the AI plays well regardless of the sidecar.
    Strategy baseline = strategy;
    fallback_.Update(gf, ctx, stats, contained, baseline);

    // The opponent digest is cheap (O(P)) and feeds every request, so refresh it each tick. It also
    // advances the per-player military trend memory for next tick.
    buildOpponentDigest(ctx, gf, stats);

    // Consume a previously-requested answer if it has arrived (may set tick_.requestReplan).
    if(pending_)
        consume(gf, baseline);

    // Update trend/escalation memory AFTER consume so a requestReplan from the just-arrived response
    // escalates THIS tick (B2), not one tick late, before we choose the request tier below.
    detectEvents(gf, stats);

    // Issue a fresh request for this tick, choosing the tier/kind.
    if(!pending_)
    {
        const bool expensive = wantExpensivePlan(gf, stats);
        const Tier tier = expensive ? Tier::Expensive : Tier::Cheap;
        const RequestKind kind = expensive ? RequestKind::Plan : RequestKind::Tick;
        // The map digest is expensive (a bounded radius pass), so build it lazily: only before an
        // expensive request, and at most once per mapRefreshGf_ frames (cached otherwise) (SPEC §5.1).
        if(tier == Tier::Expensive && (!mapDigest_.valid || gf - mapDigest_.builtGf > mapRefreshGf_))
        {
            buildMapDigest(ctx, gf);
            if(std::getenv("RTTR_LLM_DEBUG"))
                std::fprintf(stderr, "[llm p%u gf%u] map digest built (refresh<=%u GF)\n",
                             static_cast<unsigned>(playerId_), gf, mapRefreshGf_);
        }
        writeRequest(gf, ctx, stats, contained, haveLlm_ ? llmStrategy_ : baseline, tier, kind);
        pending_ = true;
        pendingGf_ = gf;
        pendingKind_ = kind;
        if(expensive)
        {
            lastPlanReqGf_ = gf;
            planEverRequested_ = true;
        }

        if(blockMs_ > 0)
        {
            // Synchronous mode: block (with timeout) for a single-run, in-loop LLM convenience. NOTE:
            // this is NOT lockstep-replay-safe (it depends on out-of-process answer timing); only the
            // no-spool heuristic floor is deterministic across replays/saves/hosts.
            unsigned waited = 0;
            while(waited < blockMs_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waited += 50;
                if(consume(gf, baseline))
                    break;
            }
        }
    }

    // Choose the knob source: a fresh LLM plan, else the heuristic baseline (with a quiet-sidecar
    // warning). Either way PROJECT the held plan_/tick_ intent and run the shared overlay so the LLM
    // and heuristic paths fold intent identically (parity, idempotent).
    Strategy out;
    if(haveLlm_ && gf - lastLlmGf_ <= staleLimitGf_)
        out = llmStrategy_;
    else
    {
        out = baseline;
        if(gf > 0 && gf - lastWarnGf_ > 10000)
        {
            rationale_ = haveLlm_ ? "LLM sidecar quiet - playing on built-in heuristic."
                                  : "Waiting for LLM sidecar (heuristic meanwhile).";
            lastWarnGf_ = gf;
        }
    }
    projectPlanAndTick(out); // copy plan_/tick_ intent onto `out` (no-op fields stay no-op)
    ApplyFocusToKnobs(out);  // identical overlay step as the heuristic path (raise-only, idempotent)
    strategy = out;
}

} // namespace AIllm
