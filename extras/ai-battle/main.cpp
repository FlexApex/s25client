// Copyright (C) 2005 - 2024 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GlobalGameSettings.h"
#include "HeadlessGame.h"
#include "QuickStartGame.h"
#include "addons/const_addons.h"
#include "gameTypes/TeamTypes.h"
#include <sstream>
#include "RTTR_Version.h"
#include "RttrConfig.h"
#include "ai/random.h"
#include "files.h"
#include "random/Random.h"
#include "s25util/System.h"
#include "s25util/strAlgos.h"

#include <memory>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/filesystem.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/program_options.hpp>
#if BOOST_VERSION >= 109000
#    include <optional>
using std::optional;
#else
#    include <boost/optional.hpp>
using boost::optional;
#endif

namespace bnw = boost::nowide;
namespace bfs = boost::filesystem;
namespace po = boost::program_options;

int main(int argc, char** argv)
{
    bnw::nowide_filesystem();
    bnw::args _(argc, argv);

    optional<std::string> replay_path;
    optional<std::string> savegame_path;
    unsigned random_init = static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    unsigned random_ai_init = random_init;

    po::options_description desc("Allowed options");
    // clang-format off
    desc.add_options()
        ("help,h", "Show help")
        ("map,m", po::value<std::string>()->required(),"Map (.swd/.wld) or savegame (.sav) to load")
        ("ai", po::value<std::vector<std::string>>(),"AI player(s) to add (required for maps; ignored for .sav, which carries its own players)")
        ("objective", po::value<std::string>()->default_value("domination"),"domination(default)|conquer")
        ("replay", po::value(&replay_path),"Filename to write replay to (optional)")
        ("save", po::value(&savegame_path),"Filename to write savegame to (optional)")
        ("stats", po::value<std::string>(),"CSV file to write per-player trajectory stats to (optional)")
        ("statsInterval", po::value<unsigned>()->default_value(1000),"Game-frame interval between stats rows")
        ("baseline", po::value<std::vector<unsigned>>()->multitoken(),"Player index(es) using the ORIGINAL (unimproved) AI, for A/B testing")
        ("teams", po::value<std::string>(),"Team assignment, e.g. \"0,1;2,3\" for a 2v2 (groups separated by ';', player indices by ',')")
        ("inexhaustibleMines", po::bool_switch(),"Enable INEXHAUSTIBLE_MINES addon (mines never deplete)")
        ("goldDeposits", po::value<unsigned>(),"CHANGE_GOLD_DEPOSITS selection: 0=none 1=remove 2=iron 3=coal 4=granite")
        ("maxRank", po::value<unsigned>(),"MAX_RANK selection: 0=General(4) .. 4=Private(0)")
        ("random_init", po::value(&random_init),"Seed value for the random number generator (optional)")
        ("random_ai_init", po::value(&random_ai_init),"Seed value for the AI random number generator (optional)")
        ("maxGF", po::value<unsigned>()->default_value(std::numeric_limits<unsigned>::max()),"Maximum number of game frames to run (optional)")
        ("version", "Show version information and exit")
        ;
    // clang-format on

    if(argc == 1)
    {
        bnw::cerr << desc << std::endl;
        return 1;
    }

    po::variables_map options;
    try
    {
        po::store(po::command_line_parser(argc, argv).options(desc).run(), options);

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

    try
    {
        // We print arguments and seed in order to be able to reproduce crashes.
        for(int i = 0; i < argc; ++i)
            bnw::cout << argv[i] << " ";
        bnw::cout << std::endl;
        bnw::cout << "random_init: " << random_init << std::endl;
        bnw::cout << "random_ai_init: " << random_ai_init << std::endl;
        bnw::cout << std::endl;

        RTTRCONFIG.Init();
        RANDOM.Init(random_init);
        AI::getRandomGenerator().seed(random_ai_init);

        const bfs::path mapPath = RTTRCONFIG.ExpandPath(options["map"].as<std::string>());
        const bool isSavegame = s25util::toLower(mapPath.extension().string()) == ".sav";

        std::unique_ptr<HeadlessGame> game;
        if(isSavegame)
        {
            // Settings, players and world state all come from the save. --ai / addon / team flags
            // are ignored (the save already encodes them).
            bnw::cout << "Loading savegame: " << mapPath << std::endl;
            game = std::make_unique<HeadlessGame>(mapPath);
        } else
        {
            if(!options.count("ai"))
            {
                bnw::cerr << "--ai is required when loading a map (only .sav files carry their own players)"
                          << std::endl;
                return 1;
            }
            const std::vector<AI::Info> ais = ParseAIOptions(options["ai"].as<std::vector<std::string>>());

            GlobalGameSettings ggs;
            const auto objective = options["objective"].as<std::string>();
            if(objective == "domination")
                ggs.objective = GameObjective::TotalDomination;
            else if(objective == "conquer")
                ggs.objective = GameObjective::Conquer3_4;
            else
            {
                bnw::cerr << "unknown objective: " << objective << std::endl;
                return 1;
            }

            ggs.objective = GameObjective::TotalDomination;

            // Addon settings (so simulations can match a real game's rules)
            if(options["inexhaustibleMines"].as<bool>())
                ggs.setSelection(AddonId::INEXHAUSTIBLE_MINES, 1);
            if(options.count("goldDeposits"))
                ggs.setSelection(AddonId::CHANGE_GOLD_DEPOSITS, options["goldDeposits"].as<unsigned>());
            if(options.count("maxRank"))
                ggs.setSelection(AddonId::MAX_RANK, options["maxRank"].as<unsigned>());

            std::vector<unsigned> baselinePlayers;
            if(options.count("baseline"))
                baselinePlayers = options["baseline"].as<std::vector<unsigned>>();

            // Team assignment, e.g. "0,1;2,3". Player -> Team (Team1, Team2, ...).
            std::vector<Team> teams;
            if(options.count("teams"))
            {
                const std::string spec = options["teams"].as<std::string>();
                std::stringstream groups(spec);
                std::string group;
                unsigned teamIdx = 0;
                while(std::getline(groups, group, ';'))
                {
                    const Team team = static_cast<Team>(static_cast<uint8_t>(Team::Team1) + teamIdx);
                    std::stringstream members(group);
                    std::string idx;
                    while(std::getline(members, idx, ','))
                    {
                        if(idx.empty())
                            continue;
                        const unsigned p = static_cast<unsigned>(std::stoul(idx));
                        if(p >= teams.size())
                            teams.resize(p + 1, Team::None);
                        teams[p] = team;
                    }
                    ++teamIdx;
                }
            }

            game = std::make_unique<HeadlessGame>(ggs, mapPath, ais, baselinePlayers, teams);
        }

        if(replay_path)
            game->RecordReplay(*replay_path, random_init);
        if(options.count("stats"))
            game->EnableStats(options["stats"].as<std::string>(), options["statsInterval"].as<unsigned>());

        game->Run(options["maxGF"].as<unsigned>());
        game->Close();
        if(savegame_path)
            game->SaveGame(*savegame_path);
    } catch(const std::exception& e)
    {
        bnw::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
