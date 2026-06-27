// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "LlmStrategist.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "addons/const_addons.h"
#include "ai/AIInterface.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/StatisticTypes.h"
#include "world/GameWorldBase.h"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
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

void LlmStrategist::writeRequest(unsigned gf, const AIContext& ctx, const EconStats& s, bool contained,
                                 const Strategy& cur) const
{
    const AIInterface& aii = ctx.aii;
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": 1,\n";
    o << "  \"player\": " << static_cast<unsigned>(playerId_) << ",\n";
    o << "  \"gf\": " << gf << ",\n";
    o << "  \"minutes\": " << gf / 1200 << ",\n";
    o << "  \"persona\": \"" << personaName(cur.persona) << "\",\n";
    o << "  \"contained\": " << boolStr(contained) << ",\n";
    o << "  \"rules\": {\"inexhaustibleMines\": " << boolStr(ctx.ggs.isEnabled(AddonId::INEXHAUSTIBLE_MINES))
      << ", \"hasGold\": " << boolStr(s.hasGold)
      << ", \"seaAttack\": " << boolStr(ctx.ggs.isEnabled(AddonId::SEA_ATTACK)) << "},\n";
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
      << ", \"armories\": " << s.armories << ", \"breweries\": " << countBld(aii, BuildingType::Brewery) << "},\n";
    o << "  \"enemy\": {\"bestMilitary\": " << s.bestEnemyMilitary << ", \"bestBuildings\": " << s.bestEnemyBuildings
      << "},\n";
    o << "  \"currentStrategy\": {"
      << "\"persona\": \"" << personaName(cur.persona) << "\""
      << ", \"expansionAggression\": " << cur.expansionAggression << ", \"economyFocus\": " << cur.economyFocus
      << ", \"militaryFocus\": " << cur.militaryFocus << ", \"attackAggression\": " << cur.attackAggression
      << ", \"recruitRatio\": " << cur.recruitRatio << ", \"frontierFill\": " << cur.frontierFill
      << ", \"wantExpand\": " << boolStr(cur.wantExpand) << "}\n";
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

bool LlmStrategist::tryReadResponse(unsigned gf, Strategy& out, std::string& chat)
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

    bool parsedAny = false;
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
            out.wantExpand = (v == "1" || v == "true" || v == "True" || v == "yes");
        else if(k == "chat")
            chat = v;
    }
    f.close();
    if(!parsedAny)
        return false; // file present but not yet complete/usable

    std::remove(path.c_str());
    std::remove(requestPath(gf).c_str());
    return true;
}

void LlmStrategist::Update(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                           Strategy& strategy)
{
    // Always compute a safe heuristic baseline so the AI plays well regardless of the sidecar.
    Strategy baseline = strategy;
    fallback_.Update(gf, ctx, stats, contained, baseline);

    // Consume a previously-requested plan if it has arrived.
    if(pending_)
    {
        Strategy plan = haveLlm_ ? llmStrategy_ : baseline;
        std::string chat;
        if(tryReadResponse(pendingGf_, plan, chat))
        {
            llmStrategy_ = plan;
            haveLlm_ = true;
            lastLlmGf_ = gf;
            pending_ = false;
            rationale_ = chat.empty() ? "Plan updated." : chat;
        }
    }

    // Issue a fresh request for this tick.
    if(!pending_)
    {
        writeRequest(gf, ctx, stats, contained, haveLlm_ ? llmStrategy_ : baseline);
        pending_ = true;
        pendingGf_ = gf;

        if(blockMs_ > 0)
        {
            // Synchronous mode: block (with timeout) for a reproducible LLM-in-the-loop run.
            Strategy plan = haveLlm_ ? llmStrategy_ : baseline;
            std::string chat;
            unsigned waited = 0;
            while(waited < blockMs_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waited += 50;
                if(tryReadResponse(pendingGf_, plan, chat))
                {
                    llmStrategy_ = plan;
                    haveLlm_ = true;
                    lastLlmGf_ = gf;
                    pending_ = false;
                    rationale_ = chat.empty() ? "Plan updated." : chat;
                    break;
                }
            }
        }
    }

    // Use the freshest available plan; otherwise fall back to the heuristic and warn if a model was
    // expected but went quiet.
    if(haveLlm_ && gf - lastLlmGf_ <= staleLimitGf_)
    {
        strategy = llmStrategy_;
    } else
    {
        strategy = baseline;
        if(gf > 0 && gf - lastWarnGf_ > 10000)
        {
            rationale_ = haveLlm_ ? "LLM sidecar quiet - playing on built-in heuristic."
                                  : "Waiting for LLM sidecar (heuristic meanwhile).";
            lastWarnGf_ = gf;
        }
    }
}

} // namespace AIllm
