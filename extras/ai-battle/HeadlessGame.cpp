// Copyright (C) 2005 - 2024 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "HeadlessGame.h"
#include "BuildingRegister.h"
#include "Cheats.h"
#include "EventManager.h"
#include "GameCommand.h"
#include "GameInterface.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "PlayerInfo.h"
#include "Savegame.h"
#include "factories/AIFactory.h"
#include "factories/GameCommandFactory.h"
#include "network/PlayerGameCommands.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/JobTypes.h"
#include "gameTypes/MapInfo.h"
#include "gameTypes/StatisticTypes.h"
#include "gameData/GameConsts.h"
#include "gameData/MaxPlayers.h"
#include <boost/nowide/iostream.hpp>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#ifdef WIN32
#    include "Windows.h"
#endif

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais, const std::vector<Team>& teams,
                                           const std::vector<unsigned>& positions);
std::string ToString(const std::chrono::milliseconds& time);
std::string HumanReadableNumber(unsigned num);
const char* AITypeName(AI::Type type);

namespace {
/// Swallows any game command the engine itself issues (cheats etc.) - none are needed headless.
struct NoopGcFactory : GameCommandFactory
{
    bool AddGC(gc::GameCommandPtr /*gc*/) override { return false; }
};
/// No-op GameInterface: the engine's world-callbacks (minimap, road-build UI, winner, cheats) are
/// client/UI concerns that don't affect the simulation, so headless can safely ignore them. Having a
/// non-null interface avoids a null-deref when the winner/defeat callbacks fire.
struct HeadlessGameInterface : GameInterface
{
    explicit HeadlessGameInterface(GameWorldBase& world) : cheats_(world, gcf_) {}
    void GI_PlayerDefeated(unsigned) override {}
    void GI_UpdateMinimap(MapPoint) override {}
    void GI_FlagDestroyed(MapPoint) override {}
    void GI_TreatyOfAllianceChanged(unsigned) override {}
    void GI_Winner(unsigned) override {}
    void GI_TeamWinner(unsigned) override {}
    void GI_StartRoadBuilding(MapPoint, bool) override {}
    void GI_CancelRoadBuilding() override {}
    void GI_BuildRoad() override {}
    Cheats& GI_GetCheats() override { return cheats_; }

    NoopGcFactory gcf_;
    Cheats cheats_;
};

/// Total number of soldiers (all ranks) currently held by the player.
unsigned CountSoldiers(const Inventory& inv)
{
    return inv.people[Job::Private] + inv.people[Job::PrivateFirstClass] + inv.people[Job::Sergeant]
           + inv.people[Job::Officer] + inv.people[Job::General];
}
} // namespace

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
                           const std::vector<Team>& teams, const std::vector<unsigned>& positions)
    : map_(map), game_(ggs, std::make_unique<EventManager>(0), GeneratePlayerInfo(ais, teams, positions)),
      world_(game_.world_), em_(*static_cast<EventManager*>(game_.em_.get()))
{
    // Mirror GameClient's fresh-map start sequence so the headless simulation (and any recorded replay)
    // matches a real game: start pacts -> load map -> set up resources -> init.
    for(unsigned i = 0; i < world_.GetNumPlayers(); ++i)
    {
        if(world_.GetPlayer(i).isUsed())
            world_.GetPlayer(i).MakeStartPacts();
    }

    MapLoader loader(world_);
    if(!loader.Load(map))
        throw std::runtime_error("Could not load " + map.string());
    MapLoader::SetupResources(world_, true);

    // One slot per map player; empty/unused slots get a null AI so indices stay aligned with world_.
    players_.clear();
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
    {
        const GamePlayer& pl = world_.GetPlayer(playerId);
        if(pl.isUsed())
            players_.push_back(AIFactory::Create(pl.aiInfo, playerId, world_));
        else
            players_.push_back(nullptr);
    }

    world_.InitAfterLoad();
    gameInterface_ = std::make_unique<HeadlessGameInterface>(world_);
    world_.SetGameInterface(gameInterface_.get());
}

HeadlessGame::~HeadlessGame()
{
    Close();
    // world_ holds a raw pointer to gameInterface_, which is destroyed before game_/world_; clear it
    // so nothing dereferences a freed interface during teardown.
    world_.SetGameInterface(nullptr);
}

void HeadlessGame::EnableStats(const bfs::path& path, unsigned interval)
{
    statsPath_ = path;
    statsInterval_ = interval;
}

void HeadlessGame::EnableDominanceAbort(unsigned minGF, double factor)
{
    dominanceMinGf_ = minGF;
    dominanceFactor_ = factor;
}

bool HeadlessGame::DominanceReached() const
{
    if(dominanceFactor_ <= 0.0 || em_.GetCurrentGF() < dominanceMinGf_)
        return false;

    unsigned numAlive = 0;
    unsigned leaderLand = 0, runnerUpLand = 0;
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        if(!pl.isUsed() || pl.IsDefeated())
            continue;
        ++numAlive;
        const unsigned land = pl.GetStatisticCurrentValue(StatisticType::Country);
        if(land > leaderLand)
        {
            runnerUpLand = leaderLand;
            leaderLand = land;
        } else if(land > runnerUpLand)
            runnerUpLand = land;
    }
    if(numAlive <= 1)
        return false; // a single survivor ends the game via the normal domination check anyway
    if(runnerUpLand == 0)
        return leaderLand > 0; // everyone else has no land left
    return static_cast<double>(leaderLand) >= dominanceFactor_ * static_cast<double>(runnerUpLand);
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

    const char* endReason = "maxGF";
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
                AIPlayer* player = players_[playerId].get();
                if(!player)
                    continue;
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
        {
            if(player)
                player->RunGF(em_.GetCurrentGF(), isnfw);
        }

        game_.RunGF();

        if(replay_.IsRecording())
            replay_.UpdateLastGF(em_.GetCurrentGF());

        if(statsFile_ && statsInterval_ > 0 && em_.GetCurrentGF() % statsInterval_ == 0)
            WriteStatsRow();

        if(DominanceReached())
        {
            endReason = "dominance";
            break;
        }

        if(std::chrono::steady_clock::now() > nextReport)
        {
            nextReport += std::chrono::seconds(1);
            PrintState();
        }
    }
    if(game_.IsGameFinished())
        endReason = "domination";
    PrintState();
    if(statsFile_)
    {
        WriteStatsRow();
        std::fclose(statsFile_);
        statsFile_ = nullptr;
    }
    PrintResult(endReason);
}

void HeadlessGame::WriteStatsHeader()
{
    std::fprintf(statsFile_, "gf,player,name,type,defeated,country,buildings,inhabitants,merchandise,military,gold,"
                             "productivity,vanquished,milblds,storehouses,soldiers,generals,helpers,boards,stones,"
                             "coins,swords,shields,beer,sawmills,foresters,farms,ironmines,coalmines,goldmines,"
                             "smelters,armories,metalworks,mints,catapults,quarries,granitemines,sites\n");
}

void HeadlessGame::WriteStatsRow()
{
    const unsigned gf = em_.GetCurrentGF();
    if(gf == lastStatsGf_) // avoid duplicate final row when maxGF is a multiple of the interval
        return;
    lastStatsGf_ = gf;
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        if(!pl.isUsed())
            continue;
        const Inventory& inv = pl.GetInventory();
        const BuildingRegister& br = pl.GetBuildingRegister();
        const auto nb = [&](BuildingType bt) { return br.GetBuildings(bt).size(); };
        std::fprintf(statsFile_,
                     "%u,%u,%s,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%zu,%zu,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                     "%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",
                     gf, p, pl.name.c_str(), static_cast<unsigned>(pl.aiInfo.type), pl.IsDefeated() ? 1 : 0,
                     pl.GetStatisticCurrentValue(StatisticType::Country),
                     pl.GetStatisticCurrentValue(StatisticType::Buildings),
                     pl.GetStatisticCurrentValue(StatisticType::Inhabitants),
                     pl.GetStatisticCurrentValue(StatisticType::Merchandise),
                     pl.GetStatisticCurrentValue(StatisticType::Military),
                     pl.GetStatisticCurrentValue(StatisticType::Gold),
                     pl.GetStatisticCurrentValue(StatisticType::Productivity),
                     pl.GetStatisticCurrentValue(StatisticType::Vanquished), br.GetMilitaryBuildings().size(),
                     br.GetStorehouses().size(), CountSoldiers(inv), inv.people[Job::General], inv.people[Job::Helper],
                     inv.goods[GoodType::Boards], inv.goods[GoodType::Stones], inv.goods[GoodType::Coins],
                     inv.goods[GoodType::Sword], inv.goods[GoodType::ShieldRomans], inv.goods[GoodType::Beer],
                     nb(BuildingType::Sawmill), nb(BuildingType::Forester), nb(BuildingType::Farm),
                     nb(BuildingType::IronMine), nb(BuildingType::CoalMine), nb(BuildingType::GoldMine),
                     nb(BuildingType::Ironsmelter), nb(BuildingType::Armory), nb(BuildingType::Metalworks),
                     nb(BuildingType::Mint), nb(BuildingType::Catapult), nb(BuildingType::Quarry),
                     nb(BuildingType::GraniteMine), br.GetBuildingSites().size());
    }
    std::fflush(statsFile_);
}

void HeadlessGame::PrintResult(const char* reason)
{
    // Determine the winner: the still-alive player holding the most populated land (the game's own
    // victory metric). Falls back to overall land leader if everyone is defeated (shouldn't happen).
    int winner = -1;
    unsigned bestLand = 0;
    unsigned numUsed = 0;
    bool anyAlive = false;
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        if(!pl.isUsed())
            continue;
        ++numUsed;
        if(pl.IsDefeated())
            continue;
        anyAlive = true;
        const unsigned land = pl.GetStatisticCurrentValue(StatisticType::Country);
        if(winner < 0 || land > bestLand)
        {
            bestLand = land;
            winner = static_cast<int>(p);
        }
    }
    if(!anyAlive)
    {
        for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
        {
            if(!world_.GetPlayer(p).isUsed())
                continue;
            const unsigned land = world_.GetPlayer(p).GetStatisticCurrentValue(StatisticType::Country);
            if(winner < 0 || land > bestLand)
            {
                bestLand = land;
                winner = static_cast<int>(p);
            }
        }
    }

    bnw::cout << "\n=== AI-BATTLE RESULT ===\n";
    bnw::cout << "RESULT reason=" << reason << " gf=" << em_.GetCurrentGF()
              << " finished=" << (game_.IsGameFinished() ? 1 : 0) << " numPlayers=" << numUsed
              << " winner=" << winner
              << " winnerType=" << (winner >= 0 ? AITypeName(world_.GetPlayer(winner).aiInfo.type) : "none") << '\n';
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        if(!pl.isUsed())
            continue;
        const Inventory& inv = pl.GetInventory();
        const BuildingRegister& br = pl.GetBuildingRegister();
        bnw::cout << "RESULT_PLAYER idx=" << p << " type=" << static_cast<unsigned>(pl.aiInfo.type)
                  << " typeName=" << AITypeName(pl.aiInfo.type) << " defeated=" << (pl.IsDefeated() ? 1 : 0)
                  << " country=" << pl.GetStatisticCurrentValue(StatisticType::Country)
                  << " military=" << pl.GetStatisticCurrentValue(StatisticType::Military)
                  << " buildings=" << pl.GetStatisticCurrentValue(StatisticType::Buildings)
                  << " inhabitants=" << pl.GetStatisticCurrentValue(StatisticType::Inhabitants)
                  << " productivity=" << pl.GetStatisticCurrentValue(StatisticType::Productivity)
                  << " soldiers=" << CountSoldiers(inv) << " milblds=" << br.GetMilitaryBuildings().size() << '\n';
    }
    bnw::cout << "=== END RESULT ===\n";
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

const char* AITypeName(AI::Type type)
{
    switch(type)
    {
        case AI::Type::Dummy: return "dummy";
        case AI::Type::Default: return "aijh";
    }
    return "unknown";
}

void HeadlessGame::PrintState()
{
    unsigned numUsed = 0;
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        if(world_.GetPlayer(p).isUsed())
            ++numUsed;
    }

    static bool first_run = true;
    if(first_run)
        first_run = false;
    else
        printConsole("\x1b[%dA", 8 + numUsed); // Move cursor back up

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
        if(!player.isUsed())
            continue;
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

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais, const std::vector<Team>& teams,
                                           const std::vector<unsigned>& positions)
{
    // Resolve the seating: positions[k] is the map start-slot for ais[k]. Default = first N slots.
    std::vector<unsigned> seats = positions;
    if(seats.empty())
    {
        for(unsigned i = 0; i < ais.size(); ++i)
            seats.push_back(i);
    }
    if(seats.size() != ais.size())
        throw std::invalid_argument("Number of --positions must match number of --ai players");

    unsigned numSlots = 0;
    for(const unsigned s : seats)
    {
        if(s >= MAX_PLAYERS)
            throw std::invalid_argument("Start position " + std::to_string(s) + " out of range (max "
                                        + std::to_string(MAX_PLAYERS - 1) + ")");
        numSlots = std::max(numSlots, s + 1);
    }

    // Empty slots are PlayerState::Free (the default PlayerInfo). Used slots get the AI + team + name.
    std::vector<PlayerInfo> ret(numSlots);
    for(unsigned k = 0; k < ais.size(); ++k)
    {
        const unsigned slot = seats[k];
        PlayerInfo& pi = ret[slot];
        if(pi.ps != PlayerState::Free)
            throw std::invalid_argument("Duplicate --positions entry: slot already taken");
        pi.ps = PlayerState::Occupied;
        pi.aiInfo = ais[k];
        switch(ais[k].type)
        {
            case AI::Type::Default: pi.name = "AIJH " + std::to_string(slot); break;
            case AI::Type::Dummy:
            default: pi.name = "Dummy " + std::to_string(slot); break;
        }
        pi.nation = Nation::Romans;
        pi.team = (k < teams.size()) ? teams[k] : Team::None;
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
