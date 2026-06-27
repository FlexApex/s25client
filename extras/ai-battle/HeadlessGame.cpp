// Copyright (C) 2005 - 2024 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "HeadlessGame.h"
#include "BuildingRegister.h"
#include "EventManager.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "PlayerInfo.h"
#include "Savegame.h"
#include "ai/aijh/AIPlayerJH.h"
#include "ai/llm/AIPlayerLlm.h"
#include "factories/AIFactory.h"
#include "network/PlayerGameCommands.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/JobTypes.h"
#include "gameTypes/MapInfo.h"
#include "gameTypes/StatisticTypes.h"
#include "gameData/GameConsts.h"
#include "helpers/containerUtils.h"
#include <boost/nowide/iostream.hpp>
#include <chrono>
#include <cstdio>
#include <sstream>
#ifdef WIN32
#    include "Windows.h"
#endif

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais);
std::string ToString(const std::chrono::milliseconds& time);
std::string HumanReadableNumber(unsigned num);

namespace bfs = boost::filesystem;
namespace bnw = boost::nowide;
using bfs::canonical;

#ifdef WIN32
HANDLE setupStdOut();
#endif

#if defined(__MINGW32__) && !defined(__clang__)
void printConsole(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#elif defined __GNUC__
void printConsole(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void printConsole(const char* fmt, ...);
#endif

HeadlessGame::HeadlessGame(const GlobalGameSettings& ggs, const bfs::path& map, const std::vector<AI::Info>& ais,
                           const std::vector<unsigned>& baselinePlayers)
    : map_(map), game_(ggs, std::make_unique<EventManager>(0), GeneratePlayerInfo(ais)), world_(game_.world_),
      em_(*static_cast<EventManager*>(game_.em_.get()))
{
    MapLoader loader(world_);
    if(!loader.Load(map))
        throw std::runtime_error("Could not load " + map.string());

    players_.clear();
    improved_.clear();
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
    {
        const AI::Info& aiInfo = world_.GetPlayer(playerId).aiInfo;
        const bool useImproved = !helpers::contains(baselinePlayers, playerId);
        improved_.push_back(useImproved && aiInfo.type == AI::Type::Default);
        if(aiInfo.type == AI::Type::Default)
            players_.push_back(std::make_unique<AIJH::AIPlayerJH>(playerId, world_, aiInfo.level, useImproved));
        else
            players_.push_back(AIFactory::Create(aiInfo, playerId, world_));
    }

    world_.InitAfterLoad();
}

void HeadlessGame::EnableStats(const bfs::path& path, unsigned interval)
{
    statsPath_ = path;
    statsInterval_ = interval;
}

HeadlessGame::~HeadlessGame()
{
    Close();
}

void HeadlessGame::Run(unsigned maxGF)
{
    AsyncChecksum checksum;
    gameStartTime_ = std::chrono::steady_clock::now();
    auto nextReport = gameStartTime_ + std::chrono::seconds(1);

    game_.Start(false);

    if(statsInterval_ > 0)
    {
        statsFile_ = std::fopen(statsPath_.string().c_str(), "w");
        if(statsFile_)
        {
            WriteStatsHeader();
            WriteStatsRow();
        }
    }

    while(em_.GetCurrentGF() < maxGF && !game_.IsGameFinished())
    {
        // In the actual game, the network frame intervall is based on ping (highest_ping < NFW-length < 20*gf_length).
        bool isnfw = em_.GetCurrentGF() % 20 == 0;

        if(isnfw)
        {
            if(replay_.IsRecording())
                checksum = AsyncChecksum::create(game_);
            for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
            {
                world_.GetPlayer(playerId);
                AIPlayer* player = players_[playerId].get();
                PlayerGameCommands cmds;
                cmds.gcs = player->FetchGameCommands();

                if(replay_.IsRecording() && !cmds.gcs.empty())
                {
                    cmds.checksum = checksum;
                    replay_.AddGameCommand(em_.GetCurrentGF(), playerId, cmds);
                }

                for(const gc::GameCommandPtr& gc : cmds.gcs)
                    gc->Execute(world_, player->GetPlayerId());
            }
        }

        for(auto& player : players_)
            player->RunGF(em_.GetCurrentGF(), isnfw);

        game_.RunGF();

        if(replay_.IsRecording())
            replay_.UpdateLastGF(em_.GetCurrentGF());

        if(statsFile_ && statsInterval_ > 0 && em_.GetCurrentGF() % statsInterval_ == 0)
            WriteStatsRow();

        if(std::chrono::steady_clock::now() > nextReport)
        {
            nextReport += std::chrono::seconds(1);
            PrintState();
        }
    }
    PrintState();
    if(statsFile_)
    {
        WriteStatsRow();
        std::fclose(statsFile_);
        statsFile_ = nullptr;
    }
}

void HeadlessGame::WriteStatsHeader()
{
    std::fprintf(statsFile_,
                 "gf,player,name,improved,defeated,country,buildings,inhabitants,merchandise,military,gold,"
                 "productivity,vanquished,milblds,storehouses,soldiers,generals,helpers,boards,stones,coins,"
                 "swords,shields,beer,sawmills,foresters,farms,ironmines,coalmines,goldmines,smelters,armories,"
                 "metalworks,mints,catapults,attacks\n");
}

void HeadlessGame::WriteStatsRow()
{
    const unsigned gf = em_.GetCurrentGF();
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        const Inventory& inv = pl.GetInventory();
        const unsigned soldiers = inv.people[Job::Private] + inv.people[Job::PrivateFirstClass]
                                  + inv.people[Job::Sergeant] + inv.people[Job::Officer] + inv.people[Job::General];
        const BuildingRegister& br = pl.GetBuildingRegister();
        const auto nb = [&](BuildingType bt) { return br.GetBuildings(bt).size(); };
        const auto* jh = dynamic_cast<const AIJH::AIPlayerJH*>(players_[p].get());
        const auto* llm = dynamic_cast<const AIllm::AIPlayerLlm*>(players_[p].get());
        const unsigned attacks = jh ? jh->GetNumAttacksLaunched() : (llm ? llm->GetNumAttacksLaunched() : 0u);
        std::fprintf(statsFile_,
                     "%u,%u,%s,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%zu,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                     "%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%u\n",
                     gf, p, pl.name.c_str(), improved_[p] ? 1 : 0, pl.IsDefeated() ? 1 : 0,
                     pl.GetStatisticCurrentValue(StatisticType::Country),
                     pl.GetStatisticCurrentValue(StatisticType::Buildings),
                     pl.GetStatisticCurrentValue(StatisticType::Inhabitants),
                     pl.GetStatisticCurrentValue(StatisticType::Merchandise),
                     pl.GetStatisticCurrentValue(StatisticType::Military),
                     pl.GetStatisticCurrentValue(StatisticType::Gold),
                     pl.GetStatisticCurrentValue(StatisticType::Productivity),
                     pl.GetStatisticCurrentValue(StatisticType::Vanquished),
                     pl.GetBuildingRegister().GetMilitaryBuildings().size(),
                     pl.GetBuildingRegister().GetStorehouses().size(), soldiers, inv.people[Job::General],
                     inv.people[Job::Helper], inv.goods[GoodType::Boards], inv.goods[GoodType::Stones],
                     inv.goods[GoodType::Coins], inv.goods[GoodType::Sword], inv.goods[GoodType::ShieldRomans],
                     inv.goods[GoodType::Beer], nb(BuildingType::Sawmill), nb(BuildingType::Forester),
                     nb(BuildingType::Farm), nb(BuildingType::IronMine), nb(BuildingType::CoalMine),
                     nb(BuildingType::GoldMine), nb(BuildingType::Ironsmelter), nb(BuildingType::Armory),
                     nb(BuildingType::Metalworks), nb(BuildingType::Mint), nb(BuildingType::Catapult), attacks);
    }
    std::fflush(statsFile_);
}

void HeadlessGame::Close()
{
    bnw::cout << '\n';

    if(replay_.IsRecording())
    {
        replay_.StopRecording();
        bnw::cout << "Replay written to " << canonical(replayPath_) << '\n';
    }

    replay_.Close();
}

void HeadlessGame::RecordReplay(const bfs::path& path, unsigned random_init)
{
    // Remove old replay
    bfs::remove(path);

    replayPath_ = path;

    MapInfo mapInfo;
    mapInfo.filepath = map_;
    mapInfo.mapData.CompressFromFile(mapInfo.filepath, &mapInfo.mapChecksum);
    mapInfo.type = MapType::OldMap;

    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
        replay_.AddPlayer(world_.GetPlayer(playerId));
    replay_.ggs = game_.ggs_;
    if(!replay_.StartRecording(path, mapInfo, random_init))
        throw std::runtime_error("Replayfile could not be opened!");
}

void HeadlessGame::SaveGame(const bfs::path& path) const
{
    // Remove old savegame
    bfs::remove(path);

    Savegame save;
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
        save.AddPlayer(world_.GetPlayer(playerId));
    save.ggs = game_.ggs_;
    save.ggs.exploration = Exploration::Disabled; // no FOW
    save.start_gf = em_.GetCurrentGF();
    save.sgd.MakeSnapshot(game_);
    save.Save(path, "AI Battle");

    bnw::cout << "Savegame written to " << canonical(path) << '\n';
}

std::string ToString(const std::chrono::milliseconds& time)
{
    char buffer[90];
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(time);
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(time % std::chrono::hours(1));
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time % std::chrono::minutes(1));
    snprintf(buffer, std::size(buffer), "%03ld:%02ld:%02ld", static_cast<long int>(hours.count()),
             static_cast<long int>(minutes.count()), static_cast<long int>(seconds.count()));
    return std::string(buffer);
}

std::string HumanReadableNumber(unsigned num)
{
    std::stringstream ss;
    ss.imbue(std::locale(""));
    ss << std::fixed << num;
    return ss.str();
}

void HeadlessGame::PrintState()
{
    static bool first_run = true;
    if(first_run)
        first_run = false;
    else
        printConsole("\x1b[%dA", 8 + world_.GetNumPlayers()); // Move cursor back up

    printConsole("┌───────────────┬───────────────────────┬───────────────────────┬────────────────┐\n");
    printConsole(
      "│ GF %10s │ Game Clock  %s │ Wall Clock  %s │ %7s GF/sec │\n", HumanReadableNumber(em_.GetCurrentGF()).c_str(),
      ToString(SPEED_GF_LENGTHS[GameSpeed::Normal] * em_.GetCurrentGF()).c_str(), // elapsed time
      ToString(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gameStartTime_))
        .c_str(),                                                       // wall clock
      HumanReadableNumber(em_.GetCurrentGF() - lastReportGf_).c_str()); // GF per second
    printConsole("└───────────────┴───────────────────────┴───────────────────────┴────────────────┘\n");
    printConsole("\n");
    printConsole("┌────────────────────────┬─────────────────┬─────────────┬───────────┬───────────┐\n");
    printConsole("│ Player                 │ Country         │ Buildings   │ Military  │ Gold      │\n");
    printConsole("├────────────────────────┼─────────────────┼─────────────┼───────────┼───────────┤\n");
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
    {
        const GamePlayer& player = world_.GetPlayer(playerId);
        printConsole("│ %s%-22s%s │ %15s │ %11s │ %9s │ %9s │\n", player.IsDefeated() ? "\x1b[9m" : "",
                     player.name.c_str(), player.IsDefeated() ? "\x1b[29m" : "",
                     HumanReadableNumber(player.GetStatisticCurrentValue(StatisticType::Country)).c_str(),
                     HumanReadableNumber(player.GetStatisticCurrentValue(StatisticType::Buildings)).c_str(),
                     HumanReadableNumber(player.GetStatisticCurrentValue(StatisticType::Military)).c_str(),
                     HumanReadableNumber(player.GetStatisticCurrentValue(StatisticType::Gold)).c_str());
    }
    printConsole("└────────────────────────┴─────────────────┴─────────────┴───────────┴───────────┘\n");

    lastReportGf_ = em_.GetCurrentGF();
}

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais)
{
    std::vector<PlayerInfo> ret;
    for(const AI::Info& ai : ais)
    {
        PlayerInfo pi;
        pi.ps = PlayerState::Occupied;
        pi.aiInfo = ai;
        switch(ai.type)
        {
            case AI::Type::Default: pi.name = "AIJH " + std::to_string(ret.size()); break;
            case AI::Type::Llm: pi.name = "LLM " + std::to_string(ret.size()); break;
            case AI::Type::Dummy:
            default: pi.name = "Dummy " + std::to_string(ret.size()); break;
        }
        pi.nation = Nation::Romans;
        pi.team = Team::None;
        ret.push_back(pi);
    }
    return ret;
}

#ifdef WIN32
HANDLE setupStdOut()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(h, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(65001);
    return h;
}
#endif

void printConsole(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    const int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if(len > 0 && (size_t)len < sizeof(buffer))
    {
#ifdef WIN32
        static auto h = setupStdOut();
        WriteConsoleA(h, buffer, len, 0, 0);
#else
        bnw::cout << buffer;
#endif
    }
}
