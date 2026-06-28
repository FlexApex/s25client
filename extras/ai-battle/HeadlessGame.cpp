// Copyright (C) 2005 - 2024 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "HeadlessGame.h"
#include "BuildingRegister.h"
#include "Cheats.h"
#include "addons/const_addons.h"
#include "EventManager.h"
#include "GameCommand.h"
#include "GameInterface.h"
#include "GamePlayer.h"
#include "GlobalGameSettings.h"
#include "ILocalGameState.h"
#include "PlayerInfo.h"
#include "Savegame.h"
#include "SerializedGameData.h"
#include "factories/GameCommandFactory.h"
#include "ai/aijh/AIPlayerJH.h"
#include "ai/llm/AIPlayerLlm.h"
#include "buildings/nobUsual.h"
#include "factories/AIFactory.h"
#include "network/PlayerGameCommands.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "gameTypes/BuildingType.h"
#include "gameTypes/Inventory.h"
#include "gameTypes/JobTypes.h"
#include "gameTypes/MapInfo.h"
#include "gameTypes/StatisticTypes.h"
#include "s25util/colors.h"
#include "gameData/GameConsts.h"
#include "helpers/containerUtils.h"
#include <boost/nowide/iostream.hpp>
#include <chrono>
#include <cstdio>
#include <sstream>
#ifdef WIN32
#    include "Windows.h"
#endif

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais, const std::vector<Team>& teams);
std::string ToString(const std::chrono::milliseconds& time);
std::string HumanReadableNumber(unsigned num);

namespace {
/// Swallows any game command the engine itself issues (cheats etc.) - none are needed headless.
struct NoopGcFactory : GameCommandFactory
{
    bool AddGC(gc::GameCommandPtr /*gc*/) override { return false; }
};
/// No-op GameInterface: the engine's world-callbacks (minimap, road-build UI, winner, cheats) are
/// client/UI concerns that don't affect the simulation, so headless can safely ignore them. Having a
/// non-null interface avoids the null-deref crash when a savegame's human-player slot triggers one.
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
                           const std::vector<unsigned>& baselinePlayers, const std::vector<Team>& teams)
    : map_(map), game_(ggs, std::make_unique<EventManager>(0), GeneratePlayerInfo(ais, teams)), world_(game_.world_),
      em_(*static_cast<EventManager*>(game_.em_.get()))
{
    MapLoader loader(world_);
    if(!loader.Load(map))
        throw std::runtime_error("Could not load " + map.string());

    players_.clear();
    improved_.clear();
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
    {
        AI::Info aiInfo = world_.GetPlayer(playerId).aiInfo;
        // --baseline forces an ApexAI slot back to the original AIJH, so the improved (ApexAI) and the
        // baseline (AIJH) strategies can be A/B-compared in the same game.
        if(helpers::contains(baselinePlayers, playerId) && aiInfo.type == AI::Type::ApexAI)
            aiInfo.type = AI::Type::Default;
        improved_.push_back(aiInfo.type == AI::Type::ApexAI);
        players_.push_back(AIFactory::Create(aiInfo, playerId, world_));
    }

    world_.InitAfterLoad();
    gameInterface_ = std::make_unique<HeadlessGameInterface>(world_);
    world_.SetGameInterface(gameInterface_.get());
}

namespace {
/// Minimal ILocalGameState for restoring a snapshot outside the real client.
struct HeadlessLocalState : ILocalGameState
{
    unsigned GetPlayerId() const override { return 0; }
    bool IsHost() const override { return true; }
    std::string FormatGFTime(unsigned numGFs) const override { return std::to_string(numGFs); }
    void SystemChat(const std::string&) override {}
};

std::unique_ptr<Savegame> loadSavegameOrThrow(const bfs::path& path)
{
    auto save = std::make_unique<Savegame>();
    if(!save->Load(path, SaveGameDataToLoad::All))
        throw std::runtime_error("Could not load savegame " + path.string());
    // The engine's "auto-demolish a building that ran out of resources" feature calls into the
    // hosting GameClient (nobUsual::OnOutOfResources -> GAMECLIENT.DestroyBuilding). There is no
    // hosting client in this headless runner, so disable that client-coupled QoL addon for the
    // continuation - it doesn't affect the economy/AI behaviour we run a save forward to observe.
    save->ggs.setSelection(AddonId::DEMOLISH_BLD_WO_RES, 0);
    return save;
}

std::vector<PlayerInfo> playersFromSave(Savegame& save)
{
    std::vector<PlayerInfo> players;
    for(unsigned i = 0; i < save.GetNumPlayers(); ++i)
        players.emplace_back(save.GetPlayer(i));
    return players;
}
} // namespace

HeadlessGame::HeadlessGame(const bfs::path& savegamePath) : HeadlessGame(loadSavegameOrThrow(savegamePath)) {}

HeadlessGame::HeadlessGame(std::unique_ptr<Savegame> save)
    : game_(save->ggs, save->start_gf, playersFromSave(*save)), world_(game_.world_),
      em_(*static_cast<EventManager*>(game_.em_.get())), fromSave_(true)
{
    HeadlessLocalState local;
    save->sgd.ReadSnapshot(game_, local); // restores the full world + player economies from the snapshot

    gameInterface_ = std::make_unique<HeadlessGameInterface>(world_);
    world_.SetGameInterface(gameInterface_.get());

    players_.clear();
    improved_.clear();
    for(unsigned playerId = 0; playerId < world_.GetNumPlayers(); ++playerId)
    {
        const AI::Info& aiInfo = world_.GetPlayer(playerId).aiInfo;
        improved_.push_back(aiInfo.type == AI::Type::ApexAI);
        players_.push_back(AIFactory::Create(aiInfo, playerId, world_));
        const GamePlayer& pl = world_.GetPlayer(playerId);
        const MapPoint hq = pl.GetHQPos();
        bnw::cout << "  player " << playerId << ": '" << pl.name << "' aiType=" << static_cast<unsigned>(aiInfo.type)
                  << " nation=" << static_cast<unsigned>(pl.nation) << " team=" << static_cast<unsigned>(pl.team)
                  << " HQ=(" << hq.x << "," << hq.y << ")" << (pl.IsDefeated() ? " [defeated]" : "") << '\n';
    }
    // No InitAfterLoad(): the snapshot is already a fully-initialised, post-load world.
}

void HeadlessGame::EnableStats(const bfs::path& path, unsigned interval)
{
    statsPath_ = path;
    statsInterval_ = interval;
}

HeadlessGame::~HeadlessGame()
{
    Close();
    // world_ holds a raw pointer to gameInterface_, which is destroyed before game_/world_; clear it
    // so nothing dereferences a freed interface during teardown.
    world_.SetGameInterface(nullptr);
}

void HeadlessGame::Run(unsigned maxGF)
{
    AsyncChecksum checksum;
    gameStartTime_ = std::chrono::steady_clock::now();
    auto nextReport = gameStartTime_ + std::chrono::seconds(1);

    game_.Start(fromSave_);

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
                 "metalworks,mints,catapults,attacks,quarries,granitemines,sites\n");
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
                     "%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%u,%zu,%zu,%zu\n",
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
                     nb(BuildingType::Metalworks), nb(BuildingType::Mint), nb(BuildingType::Catapult), attacks,
                     nb(BuildingType::Quarry), nb(BuildingType::GraniteMine),
                     pl.GetBuildingRegister().GetBuildingSites().size());
    }
    std::fflush(statsFile_);
}

void HeadlessGame::AnalyzeEconomy() const
{
    // Per-building-type roll-up: count, how many are idle (productivity 0), average productivity,
    // and (for consumers like mines) how much input food they currently hold.
    const auto roll = [&](const GamePlayer& pl, BuildingType bt) {
        const auto& blds = pl.GetBuildingRegister().GetBuildings(bt);
        unsigned n = 0, idle = 0, noWorker = 0, prodSum = 0, foodOnHand = 0;
        for(const nobUsual* b : blds)
        {
            ++n;
            const unsigned p = b->GetProductivity();
            prodSum += p;
            if(p == 0)
                ++idle;
            if(!b->HasWorker())
                ++noWorker;
            foodOnHand += b->GetNumWares(0) + b->GetNumWares(1) + b->GetNumWares(2);
        }
        return std::array<unsigned, 5>{n, idle, noWorker, n ? prodSum / n : 0, foodOnHand};
    };
    const auto line = [&](const GamePlayer& pl, const char* label, BuildingType bt, bool showFood) {
        const auto r = roll(pl, bt);
        if(r[0] == 0)
            return;
        bnw::cout << "    " << label << ": " << r[0] << "  idle=" << r[1] << " noWorker=" << r[2]
                  << " avgProd=" << r[3];
        if(showFood)
            bnw::cout << " foodOnHand=" << r[4];
        bnw::cout << '\n';
    };

    bnw::cout << "\n==================== ECONOMY ANALYSIS (loaded state, GF=" << em_.GetCurrentGF()
              << ") ====================\n";
    for(unsigned p = 0; p < world_.GetNumPlayers(); ++p)
    {
        const GamePlayer& pl = world_.GetPlayer(p);
        const AI::Info& aiInfo = pl.aiInfo;
        if(aiInfo.type != AI::Type::Default && aiInfo.type != AI::Type::ApexAI && aiInfo.type != AI::Type::Llm)
            continue; // skip humans / dummies
        const Inventory& inv = pl.GetInventory();
        const MapPoint hq = pl.GetHQPos();
        bnw::cout << "\n-- player " << p << " '" << pl.name << "' aiType=" << static_cast<unsigned>(aiInfo.type)
                  << " HQ=(" << hq.x << "," << hq.y << ")" << (pl.IsDefeated() ? " [defeated]" : "") << " --\n";

        bnw::cout << "  FOOD PRODUCTION:\n";
        line(pl, "Farm        ", BuildingType::Farm, false);
        line(pl, "Mill        ", BuildingType::Mill, false);
        line(pl, "Bakery      ", BuildingType::Bakery, false);
        line(pl, "PigFarm     ", BuildingType::PigFarm, false);
        line(pl, "Slaughterhse", BuildingType::Slaughterhouse, false);
        line(pl, "Fishery     ", BuildingType::Fishery, false);
        line(pl, "Hunter      ", BuildingType::Hunter, false);
        line(pl, "Well        ", BuildingType::Well, false);
        line(pl, "Brewery     ", BuildingType::Brewery, false);
        line(pl, "DonkeyBreedr", BuildingType::DonkeyBreeder, false);
        line(pl, "Charburner  ", BuildingType::Charburner, false);

        bnw::cout << "  MINES (consume food):\n";
        line(pl, "CoalMine    ", BuildingType::CoalMine, true);
        line(pl, "IronMine    ", BuildingType::IronMine, true);
        line(pl, "GoldMine    ", BuildingType::GoldMine, true);
        line(pl, "GraniteMine ", BuildingType::GraniteMine, true);

        bnw::cout << "  WARE STOCKS:  grain=" << inv.goods[GoodType::Grain] << " flour=" << inv.goods[GoodType::Flour]
                  << " bread=" << inv.goods[GoodType::Bread] << " meat=" << inv.goods[GoodType::Meat]
                  << " ham=" << inv.goods[GoodType::Ham] << " fish=" << inv.goods[GoodType::Fish]
                  << " water=" << inv.goods[GoodType::Water] << " beer=" << inv.goods[GoodType::Beer] << '\n';
        bnw::cout << "  WORKERS/TOOLS: farmer=" << inv.people[Job::Farmer] << " scythe=" << inv.goods[GoodType::Scythe]
                  << " | miner=" << inv.people[Job::Miner] << " pickaxe=" << inv.goods[GoodType::PickAxe]
                  << " | baker=" << inv.people[Job::Baker] << " butcher=" << inv.people[Job::Butcher]
                  << " miller=" << inv.people[Job::Miller] << " fisher=" << inv.people[Job::Fisher]
                  << " helpers=" << inv.people[Job::Helper] << '\n';
    }
    bnw::cout << "\n=================================================================================\n\n";
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

std::vector<PlayerInfo> GeneratePlayerInfo(const std::vector<AI::Info>& ais, const std::vector<Team>& teams)
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
            case AI::Type::ApexAI: pi.name = "ApexAI " + std::to_string(ret.size()); break;
            case AI::Type::Llm: pi.name = "LLM " + std::to_string(ret.size()); break;
            case AI::Type::Dummy:
            default: pi.name = "Dummy " + std::to_string(ret.size()); break;
        }
        pi.nation = Nation::Romans;
        pi.color = PLAYER_COLORS[ret.size() % PLAYER_COLORS.size()];
        pi.team = (ret.size() < teams.size()) ? teams[ret.size()] : Team::None;
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
