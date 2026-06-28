// Copyright (C) 2005 - 2024 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Game.h"
#include "Replay.h"
#include "ai/AIPlayer.h"
#include "gameTypes/AIInfo.h"
#include "gameTypes/TeamTypes.h"
#include <boost/filesystem.hpp>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

class GameWorld;
class GlobalGameSettings;
class EventManager;
class GameInterface;

/// Run an ai-only game without user-interface.
class HeadlessGame
{
public:
    /// @param ais       one entry per AI player to add (in seating order).
    /// @param teams     optional team per AI (same order/length as `ais`; default: all Team::None).
    /// @param positions optional map start-position (slot) per AI (same order/length as `ais`). Lets the
    ///                  AIs occupy specific, fixed HQ positions (e.g. {0,2}) instead of the first N; the
    ///                  other map slots are left empty. Default: 0,1,2,... (the first N positions).
    HeadlessGame(const GlobalGameSettings& ggs, const boost::filesystem::path& map, const std::vector<AI::Info>& ais,
                 const std::vector<Team>& teams = {}, const std::vector<unsigned>& positions = {});
    ~HeadlessGame();

    void Run(unsigned maxGF = std::numeric_limits<unsigned>::max());
    void Close();

    void RecordReplay(const boost::filesystem::path& path, unsigned random_init);
    void SaveGame(const boost::filesystem::path& path) const;

    /// Write a CSV row per (used) player every `interval` game frames to `path` (machine-readable
    /// trajectory log). Call before Run().
    void EnableStats(const boost::filesystem::path& path, unsigned interval);

    /// Abort the game early once one player dominates: after `minGF`, if a single still-alive player's
    /// populated-land (Country) statistic is at least `factor` times the best of all the others, the game
    /// stops (reported with reason "dominance"). factor <= 0 disables. Saves wall-clock on lopsided games.
    void EnableDominanceAbort(unsigned minGF, double factor);

private:
    void PrintState();
    void WriteStatsHeader();
    void WriteStatsRow();
    /// Print the final machine-readable RESULT block (parsed by tooling such as tools/ai-eval/eval.py).
    void PrintResult(const char* reason);
    /// Whether the dominance-abort condition currently holds.
    bool DominanceReached() const;

    boost::filesystem::path map_;
    Game game_;
    GameWorld& world_;
    EventManager& em_;
    /// One slot per map player; an entry is null for an empty/unused slot (so indices line up with
    /// world_.GetPlayer(i)).
    std::vector<std::unique_ptr<AIPlayer>> players_;
    /// No-op GameInterface so the engine's world-callbacks (winner/defeat/cheats) don't dereference null
    /// in this headless, client-less runner.
    std::unique_ptr<GameInterface> gameInterface_;

    Replay replay_;
    boost::filesystem::path replayPath_;

    boost::filesystem::path statsPath_;
    unsigned statsInterval_ = 0;
    unsigned lastStatsGf_ = std::numeric_limits<unsigned>::max();
    FILE* statsFile_ = nullptr;

    unsigned dominanceMinGf_ = 0;
    double dominanceFactor_ = 0.0;

    unsigned lastReportGf_ = 0;
    std::chrono::steady_clock::time_point gameStartTime_;
};
