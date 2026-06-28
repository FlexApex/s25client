// Copyright (C) 2005 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Headless replay player + desync verifier.
//
// Plays a recorded .rpl back through the real engine exactly the way GameClient does in replay mode
// (load map/savegame -> [load embedded Lua] -> SetupResources -> MakeStartPacts -> InitAfterLoad ->
// execute the recorded commands at their GFs, comparing the stored AsyncChecksum against the freshly
// computed one). With no window/rendering it is a pure check of simulation determinism between whatever
// recorded the replay and the engine's replay path.
//
// Two faces in one tool:
//  * Player/monitor  - live refreshing table (GF/clock/GF-per-sec + per-player stats), roster, version.
//  * Desync verifier - reports the first object-count divergence and the first full async with a
//                      per-field checksum breakdown (rand/objCt/objIdCt/eventCt/evInstanceCt), so we can
//                      tell *what* diverged. Diagnostic env switches (off by default) localise a desync:
//      RTTR_VERIFY_TRACE       print every checksum comparison (suppresses the live table)
//      RTTR_VERIFY_RUN_AI      also construct+run the AI players each GF (commands discarded) - tests
//                              whether the mere presence of the AI during playback changes the result
//      RTTR_VERIFY_NO_SETUPRES skip MapLoader::SetupResources (mimic a recorder that didn't call it)
//      RTTR_VERIFY_NO_PACTS    skip GamePlayer::MakeStartPacts (mimic a recorder that didn't call it)
//
// Exit code: 0 in sync, 2 desync detected, 1 usage/load error.

#include "AsyncChecksum.h"
#include "EventManager.h"
#include "Game.h"
#include "GamePlayer.h"
#include "HeadlessReplay.h"
#include "PlayerInfo.h"
#include "RTTR_Version.h"
#include "Replay.h"
#include "RttrConfig.h"
#include "Savegame.h"
#include "ai/AIPlayer.h"
#include "factories/AIFactory.h"
#include "ogl/glAllocator.h"
#include "random/Random.h"
#include "random/randomIO.h"
#include "variant.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "gameTypes/MapInfo.h"
#include "libsiedler2/libsiedler2.h"
#include "s25util/System.h"
#include "s25util/tmpFile.h"

#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

namespace bfs = boost::filesystem;
namespace bnw = boost::nowide;
namespace po = boost::program_options;

int main(int argc, char** argv)
{
    bnw::nowide_filesystem();
    bnw::args _(argc, argv);

    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help,h", "Show help")
        ("replay,r", po::value<std::string>()->required(), "Replay file (.rpl) to play back\n"
                        "Supports <RTTR_USERDATA> placeholder (user data dir)")
        ("verbose,V", "Print the async-log entries when the first desync is detected")
        ("version,v", "Show version information and exit")
    ;
    // clang-format on
    po::positional_options_description pos;
    pos.add("replay", 1);

    if(argc == 1)
    {
        bnw::cerr << desc << std::endl;
        return 1;
    }

    po::variables_map options;
    try
    {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(pos).run(), options);
        if(options.count("help"))
        {
            bnw::cout << desc << std::endl;
            return 0;
        }
        if(options.count("version"))
        {
            bnw::cout << rttr::version::GetTitle() << " v" << rttr::version::GetVersion() << "-"
                      << rttr::version::GetRevision() << std::endl
                      << "Compiled with " << System::getCompilerName() << " for " << System::getOSName() << std::endl;
            return 0;
        }
        po::notify(options);
    } catch(const std::exception& e)
    {
        bnw::cerr << "Error: " << e.what() << std::endl;
        bnw::cerr << desc << std::endl;
        return 1;
    }

    for(int i = 0; i < argc; ++i)
        bnw::cout << argv[i] << " ";
    bnw::cout << "\n\n";

    try
    {
        RTTRCONFIG.Init();
        libsiedler2::setAllocator(new GlAllocator);

        const bfs::path replayPath = RTTRCONFIG.ExpandPath(options["replay"].as<std::string>());
        const bool verbose = options.count("verbose") > 0;
        // Diagnostic switches (off by default) - see file header.
        const bool trace = std::getenv("RTTR_VERIFY_TRACE") != nullptr;
        const bool runAI = std::getenv("RTTR_VERIFY_RUN_AI") != nullptr;

        bnw::cout << "Loading: " << replayPath << "\n";

        Replay replay;
        if(!replay.LoadHeader(replayPath))
        {
            bnw::cerr << "Failed to load replay header: " << replay.GetLastErrorMsg() << "\n";
            return 1;
        }
        MapInfo mapInfo;
        if(!replay.LoadGameData(mapInfo))
        {
            bnw::cerr << "Failed to load replay game data: " << replay.GetLastErrorMsg() << "\n";
            return 1;
        }

        std::vector<PlayerInfo> players;
        for(unsigned i = 0; i < replay.GetNumPlayers(); i++)
            players.emplace_back(replay.GetPlayer(i));

        Game game(replay.ggs, /*startGF*/ 0, players);
        RANDOM.Init(replay.getSeed());
        GameWorld& gameWorld = game.world_;

        const bool isSavegame = (mapInfo.savegame != nullptr);
        if(isSavegame)
        {
            HeadlessGameState gs;
            mapInfo.savegame->sgd.ReadSnapshot(game, gs);
        } else
        {
            TmpFile mapfile;
            mapfile.close();
            if(!mapInfo.mapData.DecompressToFile(mapfile.filePath))
            {
                bnw::cerr << "Failed to decompress embedded map data\n";
                return 1;
            }
            MapLoader loader(gameWorld);
            if(!loader.Load(mapfile.filePath))
            {
                bnw::cerr << "Failed to load map\n";
                return 1;
            }
            if(mapInfo.luaData.uncompressedLength > 0)
            {
                TmpFile luaFile(".lua");
                luaFile.close();
                if(!mapInfo.luaData.DecompressToFile(luaFile.filePath))
                {
                    bnw::cerr << "Failed to decompress embedded Lua script\n";
                    return 1;
                }
                HeadlessGameState gs;
                if(!loader.LoadLuaScript(game, gs, luaFile.filePath))
                {
                    bnw::cerr << "Failed to load embedded Lua script\n";
                    return 1;
                }
                gameWorld.GetLua().setSuppressStdout(true);
                bnw::cout << "Lua script loaded from replay.\n";
            }
            // Match GameClient::StartGame: fix-fish for >= 8.3 replays, then team pacts.
            // The env switches let us replicate an OLD recorder that skipped these (to localise a desync).
            const bool fixFish = !(replay.GetMajorVersion() == 8 && replay.GetMinorVersion() < 3);
            if(!std::getenv("RTTR_VERIFY_NO_SETUPRES"))
            {
                MapLoader::SetupResources(gameWorld, fixFish);
                if(!fixFish)
                    bnw::cout << "Note: fish fix skipped (replay version "
                              << static_cast<unsigned>(replay.GetMajorVersion()) << "."
                              << static_cast<unsigned>(replay.GetMinorVersion()) << " predates 8.3)\n";
            }
            if(!std::getenv("RTTR_VERIFY_NO_PACTS"))
                for(unsigned i = 0; i < gameWorld.GetNumPlayers(); ++i)
                    gameWorld.GetPlayer(i).MakeStartPacts();
        }

        gameWorld.InitAfterLoad();

        const unsigned startGF = game.em_->GetCurrentGF();
        printInitialInfo(replay, gameWorld, isSavegame, startGF);

        // RTTR_VERIFY_RUN_AI: construct the AI players and run them each GF. Their generated commands are
        // discarded - the replay still drives the world via the recorded commands. This tests whether the
        // mere presence of the AI during simulation (notification subscriptions, lazy world reads) is what
        // makes a recording diverge from a no-AI replay.
        std::vector<std::unique_ptr<AIPlayer>> aiPlayers;
        if(runAI)
        {
            for(unsigned i = 0; i < gameWorld.GetNumPlayers(); ++i)
            {
                const GamePlayer& pl = gameWorld.GetPlayer(i);
                aiPlayers.push_back(pl.ps == PlayerState::AI ? AIFactory::Create(pl.aiInfo, i, gameWorld) : nullptr);
            }
            bnw::cout << "RUN_AI: constructed " << aiPlayers.size() << " AI slots\n";
        }

        auto nextGF = replay.ReadGF();
        if(!nextGF)
        {
            bnw::cerr << "Empty replay: no commands found\n";
            return 1;
        }

        ReplayStatus status{game, gameWorld, replay.GetLastGF(), std::chrono::steady_clock::now(), startGF};
        auto nextReport = status.startTime + std::chrono::seconds(1);

        bool endOfReplay = false;
        unsigned asyncCount = 0;
        unsigned firstAsyncGF = 0;
        bool firstDivergenceReported = false;

        do
        {
            const unsigned curGF = game.em_->GetCurrentGF();

            AsyncChecksum checksum;
            if(*nextGF == curGF)
                checksum = AsyncChecksum::create(game);

            while(*nextGF == curGF)
            {
                const auto cmd = replay.ReadCommand();
                visit(composeVisitor(
                        [](const Replay::ChatCommand&) {},
                        [&](const Replay::GameCommand& gcmd) {
                            for(const gc::GameCommandPtr& gc : gcmd.cmds.gcs)
                                gc->Execute(game.world_, gcmd.player);

                            const AsyncChecksum& stored = gcmd.cmds.checksum;
                            if(stored.randChecksum == 0)
                                return; // no checksum recorded for this batch

                            // Earliest sign of a *non-RNG* divergence: object counts differ while every
                            // random draw still matches - usually a setup mismatch (SetupResources /
                            // MakeStartPacts). This shows up before the first full async.
                            if(!firstDivergenceReported
                               && (stored.objCt != checksum.objCt || stored.objIdCt != checksum.objIdCt))
                            {
                                firstDivergenceReported = true;
                                bnw::cout << "FIRST obj divergence at GF " << curGF << " (player "
                                          << unsigned(gcmd.player) << "): objCt " << stored.objCt << ":"
                                          << checksum.objCt << " objIdCt " << stored.objIdCt << ":" << checksum.objIdCt
                                          << " rand " << stored.randChecksum << ":" << checksum.randChecksum
                                          << " (match=" << (stored == checksum) << ")\n";
                            }
                            if(trace)
                                bnw::cout << "  gf " << curGF << " p" << unsigned(gcmd.player) << " objCt "
                                          << stored.objCt << ":" << checksum.objCt << " objIdCt " << stored.objIdCt
                                          << ":" << checksum.objIdCt << (stored == checksum ? "" : "  <DIFF>") << "\n";

                            if(stored != checksum)
                            {
                                if(asyncCount == 0)
                                {
                                    firstAsyncGF = curGF;
                                    bnw::cerr << "\nASYNC at GF " << curGF << " (player " << unsigned(gcmd.player)
                                              << ")  stored:actual\n"
                                              << "  rand         " << stored.randChecksum << ":"
                                              << checksum.randChecksum << "\n"
                                              << "  objCt        " << stored.objCt << ":" << checksum.objCt << "\n"
                                              << "  objIdCt      " << stored.objIdCt << ":" << checksum.objIdCt << "\n"
                                              << "  eventCt      " << stored.eventCt << ":" << checksum.eventCt << "\n"
                                              << "  evInstanceCt " << stored.evInstanceCt << ":"
                                              << checksum.evInstanceCt << "\n";
                                    if(verbose)
                                        for(const auto& entry : RANDOM.GetAsyncLog())
                                            bnw::cerr << "  " << entry << "\n";
                                }
                                ++asyncCount;
                            }
                        }),
                      cmd);

                nextGF = replay.ReadGF();
                if(!nextGF)
                {
                    endOfReplay = true;
                    break;
                }
            }

            if(runAI)
            {
                const bool isnwf = curGF % 20 == 0;
                for(auto& ai : aiPlayers)
                {
                    if(!ai)
                        continue;
                    if(isnwf)
                        (void)ai->FetchGameCommands(); // drain+discard, matching the recorder's NWF cadence
                    ai->RunGF(curGF, isnwf);
                }
            }

            game.RunGF();

            // The live table relies on cursor-up by a fixed line count, so the per-GF trace output would
            // garble it - skip the periodic refresh when tracing (the final table still prints).
            if(!trace)
            {
                const auto now = std::chrono::steady_clock::now();
                if(now >= nextReport)
                {
                    nextReport += std::chrono::seconds(1);
                    printTable(status);
                    status.lastReportGF = game.em_->GetCurrentGF();
                }
            }
        } while(!endOfReplay);

        // final table
        printTable(status);
        printConsole("\n");

        const unsigned finalGF = game.em_->GetCurrentGF();
        const float elapsed =
          std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - status.startTime)
            .count();
        bnw::cout << "Finished " << finalGF << " GFs in " << elapsed << "s ("
                  << static_cast<unsigned>(elapsed > 0.f ? finalGF / elapsed : 0.f) << " GF/s)\n";

        if(asyncCount > 0)
        {
            bnw::cerr << "DESYNC: firstAsyncGF=" << firstAsyncGF << " totalAsyncFrames=" << asyncCount << "\n";
            return 2;
        }
        bnw::cout << "IN SYNC: replay verified to lastGF=" << replay.GetLastGF() << "\n";
        return 0;

    } catch(const std::exception& e)
    {
        bnw::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
