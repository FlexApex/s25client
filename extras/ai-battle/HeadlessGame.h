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
#include <limits>
#include <memory>
#include <vector>

class GameWorld;
class GlobalGameSettings;
class EventManager;
class Savegame;
class GameInterface;

/// Run an ai-only game without user-interface.
class HeadlessGame
{
public:
    /// baselinePlayers: indices of players that should use the ORIGINAL (unimproved) AIJH behaviour.
    /// All other AIJH players use the improved strategy. Used for A/B testing.
    HeadlessGame(const GlobalGameSettings& ggs, const boost::filesystem::path& map, const std::vector<AI::Info>& ais,
                 const std::vector<unsigned>& baselinePlayers = {}, const std::vector<Team>& teams = {});
    /// Continue a saved game: settings, players and world state all come from the .sav file. AI players
    /// are recreated from each slot's stored aiInfo, so the same AIs resume from the snapshot.
    explicit HeadlessGame(const boost::filesystem::path& savegamePath);
    ~HeadlessGame();

    void Run(unsigned maxGF = std::numeric_limits<unsigned>::max());
    void Close();

    void RecordReplay(const boost::filesystem::path& path, unsigned random_init);
    void SaveGame(const boost::filesystem::path& path) const;

    /// Write a CSV row per player every `interval` game frames to `path` (machine-readable trajectory log).
    void EnableStats(const boost::filesystem::path& path, unsigned interval);

    /// One-shot human-readable dump of every AI player's food chain + mine working-state (diagnostic).
    /// Prints food-chain building counts/productivity, per-mine idle/food state, and food-ware stocks so
    /// one can see where a stalled mining economy breaks (no farms? no flour? food not reaching mines?).
    void AnalyzeEconomy() const;

private:
    /// Delegated-to by the savegame constructor (loads the file before game_ is built).
    explicit HeadlessGame(std::unique_ptr<Savegame> save);

    void PrintState();
    void WriteStatsHeader();
    void WriteStatsRow();

    boost::filesystem::path map_;
    Game game_;
    GameWorld& world_;
    EventManager& em_;
    std::vector<std::unique_ptr<AIPlayer>> players_;
    std::vector<bool> improved_;
    bool fromSave_ = false;
    /// No-op GameInterface so world->GetGameInterface() callbacks (e.g. for human player slots in a
    /// loaded savegame) don't dereference null in this headless, client-less runner.
    std::unique_ptr<GameInterface> gameInterface_;

    Replay replay_;
    boost::filesystem::path replayPath_;

    boost::filesystem::path statsPath_;
    unsigned statsInterval_ = 0;
    unsigned lastStatsGf_ = std::numeric_limits<unsigned>::max();
    FILE* statsFile_ = nullptr;

    unsigned lastReportGf_ = 0;
    std::chrono::steady_clock::time_point gameStartTime_;
};
