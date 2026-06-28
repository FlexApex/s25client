// Copyright (C) 2005 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Headless replay verifier: replays a .rpl through the real engine exactly like GameClient does in
// replay mode (load map/savegame -> SetupResources -> MakeStartPacts -> InitAfterLoad -> execute the
// recorded commands at their GFs, comparing the stored AsyncChecksum against the freshly computed one).
// Unlike the GUI client it does no rendering, so it is a pure check of simulation determinism between
// the HeadlessGame recorder and the engine's replay path. Reports the first desync GF and its checksum
// breakdown (rand/objCt/objIdCt/eventCt/evInstanceCt) so we can tell *what* diverged.

#include "AsyncChecksum.h"
#include "EventManager.h"
#include "Game.h"
#include "GamePlayer.h"
#include "ILocalGameState.h"
#include "Replay.h"
#include "RttrConfig.h"
#include "Savegame.h"
#include "ai/AIPlayer.h"
#include "factories/AIFactory.h"
#include "network/PlayerGameCommands.h"
#include "ogl/glAllocator.h"
#include "random/Random.h"
#include "variant.h"
#include "world/GameWorld.h"
#include "world/MapLoader.h"
#include "gameTypes/MapInfo.h"
#include "libsiedler2/libsiedler2.h"
#include "s25util/tmpFile.h"
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/iostream.hpp>

namespace bnw = boost::nowide;
namespace bfs = boost::filesystem;

namespace {
struct MockGameState : ILocalGameState
{
    unsigned GetPlayerId() const override { return 0; }
    bool IsHost() const override { return true; }
    std::string FormatGFTime(unsigned) const override { return ""; }
    void SystemChat(const std::string&) override {}
};
} // namespace

int main(int argc, char** argv)
{
    bnw::args _(argc, argv);
    bnw::nowide_filesystem();
    if(argc < 2)
    {
        bnw::cerr << "usage: replay-verify <replay.rpl>\n";
        return 1;
    }

    libsiedler2::setAllocator(new GlAllocator());
    RTTRCONFIG.Init();

    const bfs::path replayPath = argv[1];
    Replay replay;
    if(!replay.LoadHeader(replayPath))
    {
        bnw::cerr << "Failed to load replay header: " << replayPath << "\n";
        return 1;
    }
    MapInfo mapInfo;
    if(!replay.LoadGameData(mapInfo))
    {
        bnw::cerr << "Failed to load replay game data\n";
        return 1;
    }

    std::vector<PlayerInfo> players;
    for(unsigned i = 0; i < replay.GetNumPlayers(); i++)
        players.emplace_back(replay.GetPlayer(i));

    Game game(replay.ggs, /*startGF*/ 0, players);
    RANDOM.Init(replay.getSeed());
    GameWorld& gameWorld = game.world_;

    const bool isSavegame = mapInfo.type == MapType::Savegame;
    if(isSavegame)
    {
        MockGameState gs;
        mapInfo.savegame->sgd.ReadSnapshot(game, gs);
    } else
    {
        TmpFile mapfile;
        mapfile.close();
        if(!mapInfo.mapData.DecompressToFile(mapfile.filePath))
        {
            bnw::cerr << "Failed to decompress embedded map\n";
            return 1;
        }
        MapLoader loader(gameWorld);
        if(!loader.Load(mapfile.filePath))
        {
            bnw::cerr << "Failed to load map\n";
            return 1;
        }
        // Match GameClient::StartGame: fixFish for >= 8.3 replays, then team pacts.
        // Env switches let us replicate an OLD recorder that skipped these (to localize a desync).
        if(!std::getenv("RTTR_VERIFY_NO_SETUPRES"))
        {
            const bool fixFish = replay.GetMinorVersion() >= 3;
            MapLoader::SetupResources(gameWorld, fixFish);
        }
        if(!std::getenv("RTTR_VERIFY_NO_PACTS"))
            for(unsigned i = 0; i < gameWorld.GetNumPlayers(); ++i)
                gameWorld.GetPlayer(i).MakeStartPacts();
    }
    gameWorld.InitAfterLoad();

    bnw::cout << "Replay: " << replayPath.filename() << " seed=" << replay.getSeed()
              << " players=" << replay.GetNumPlayers() << " lastGF=" << replay.GetLastGF()
              << " minorVer=" << unsigned(replay.GetMinorVersion()) << " savegame=" << isSavegame << "\n";

    const bool trace = std::getenv("RTTR_VERIFY_TRACE") != nullptr;

    // Diagnostic: replicate the recorder's execution by ALSO constructing+running the AI players each GF
    // (their generated commands are discarded - the replay still drives the world via recorded commands).
    // This tests whether the mere PRESENCE of the AI during simulation (notification subscriptions, lazy
    // world reads) is what makes a HeadlessGame recording diverge from a no-AI replay.
    const bool runAI = std::getenv("RTTR_VERIFY_RUN_AI") != nullptr;
    std::vector<std::unique_ptr<AIPlayer>> aiPlayers;
    if(runAI)
    {
        for(unsigned i = 0; i < gameWorld.GetNumPlayers(); ++i)
        {
            const GamePlayer& pl = gameWorld.GetPlayer(i);
            if(pl.ps == PlayerState::AI)
                aiPlayers.push_back(AIFactory::Create(pl.aiInfo, i, gameWorld));
            else
                aiPlayers.push_back(nullptr);
        }
        bnw::cout << "RUN_AI: constructed " << aiPlayers.size() << " AI slots\n";
    }

    auto nextGF = replay.ReadGF();
    bool endOfReplay = !nextGF.has_value();
    unsigned asyncFrames = 0;
    unsigned firstAsyncGF = 0;
    bool firstDivergenceReported = false;

    while(!endOfReplay)
    {
        const unsigned curGF = game.em_->GetCurrentGF();
        AsyncChecksum checksum;
        if(nextGF && *nextGF == curGF)
            checksum = AsyncChecksum::create(game);
        while(nextGF && *nextGF == curGF)
        {
            const auto cmd = replay.ReadCommand();
            visit(composeVisitor([](const Replay::ChatCommand&) {},
                                 [&](const Replay::GameCommand& gcmd) {
                                     for(const gc::GameCommandPtr& gc : gcmd.cmds.gcs)
                                         gc->Execute(game.world_, gcmd.player);
                                     const AsyncChecksum& exp = gcmd.cmds.checksum;
                                     // First point where object counts diverge (usually earlier than the
                                     // first full async, which also needs rand/event to mismatch). The
                                     // cause lies between the previous command-GF and this one.
                                     if(exp.randChecksum != 0 && !firstDivergenceReported
                                        && (exp.objCt != checksum.objCt || exp.objIdCt != checksum.objIdCt))
                                     {
                                         firstDivergenceReported = true;
                                         bnw::cout << "FIRST obj divergence at GF " << curGF << " (player "
                                                   << unsigned(gcmd.player) << "): objCt " << exp.objCt << ":"
                                                   << checksum.objCt << " objIdCt " << exp.objIdCt << ":"
                                                   << checksum.objIdCt << " rand " << exp.randChecksum << ":"
                                                   << checksum.randChecksum << " (match=" << (exp == checksum) << ")\n";
                                     }
                                     if(trace && exp.randChecksum != 0)
                                         bnw::cout << "  gf " << curGF << " p" << unsigned(gcmd.player) << " objCt "
                                                   << exp.objCt << ":" << checksum.objCt << " objIdCt " << exp.objIdCt
                                                   << ":" << checksum.objIdCt << (exp == checksum ? "" : "  <DIFF>")
                                                   << "\n";
                                     if(exp.randChecksum != 0 && exp != checksum)
                                     {
                                         if(asyncFrames == 0)
                                         {
                                             firstAsyncGF = curGF;
                                             bnw::cout << "ASYNC at GF " << curGF << " (player " << unsigned(gcmd.player)
                                                       << "): orig:replay\n"
                                                       << "  rand        " << exp.randChecksum << ":"
                                                       << checksum.randChecksum << "\n"
                                                       << "  objCt       " << exp.objCt << ":" << checksum.objCt << "\n"
                                                       << "  objIdCt     " << exp.objIdCt << ":" << checksum.objIdCt
                                                       << "\n"
                                                       << "  eventCt     " << exp.eventCt << ":" << checksum.eventCt
                                                       << "\n"
                                                       << "  evInstanceCt " << exp.evInstanceCt << ":"
                                                       << checksum.evInstanceCt << "\n";
                                         }
                                         ++asyncFrames;
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
        if(game.em_->GetCurrentGF() > replay.GetLastGF())
            break;
    }

    if(asyncFrames == 0)
        bnw::cout << "IN SYNC: replay verified to lastGF=" << replay.GetLastGF() << "\n";
    else
        bnw::cout << "DESYNC: firstAsyncGF=" << firstAsyncGF << " totalAsyncFrames=" << asyncFrames << "\n";
    return asyncFrames ? 2 : 0;
}
