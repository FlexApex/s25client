// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

class AIInterface;
class GamePlayer;
class GameWorldBase;
class GlobalGameSettings;

namespace AIllm {

/// A coarse "personality" that biases the opening and overall style. Picked once per game (from the
/// AI RNG) so different seeds play visibly differently — variety is a feature.
enum class Persona : uint8_t
{
    Balanced, // even economy/military, opportunistic
    Rusher,   // early military + attacks
    Boomer,   // economy first, big late army
    Turtle,   // defensive, hold and out-produce
    Expander  // aggressive land grab
};

/// The contract between the strategist (slow brain) and the executor (fast hands).
///
/// All knobs are coarse 0..10 weights (unless noted). The executor scales its concrete decisions
/// (wanted building counts, military sliders, attack thresholds) off these, so the strategist never
/// has to deal with map coordinates or individual buildings.
struct Strategy
{
    Persona persona = Persona::Balanced;

    int expansionAggression = 5; // 0..10 how hard to push military buildings (territory)
    int economyFocus = 5;        // 0..10 weight on production depth (boards/food/tools)
    int militaryFocus = 5;       // 0..10 weight on the weapons chain (coal/iron/smelter/armory)
    int attackAggression = 4;    // 0..10 attack frequency + willingness to commit

    int recruitRatio = 10; // 0..10 -> military slider[0]
    int frontierFill = 8;  // 0..8  -> military slider[7]

    bool wantExpand = true; // master switch for claiming new land
};

/// Read-only handles a strategist may use to inspect the world.
struct AIContext
{
    const AIInterface& aii;
    const GamePlayer& player;
    const GameWorldBase& gwb;
    const GlobalGameSettings& ggs;
    unsigned char playerId;
};

/// Compact, derived view of our economy + the strongest enemy. Computed cheaply each strategist tick
/// and reused by the executor for wanted-count scaling.
struct EconStats
{
    unsigned gf = 0;
    unsigned nMil = 0;      // finished military buildings (territory proxy)
    unsigned nMilSites = 0; // military buildings under construction
    unsigned nStore = 0;    // warehouses incl. HQ + harbors

    unsigned boards = 0, stones = 0;
    unsigned soldiers = 0, swords = 0, shields = 0, beer = 0, helpers = 0;
    unsigned armories = 0, coalMines = 0, ironMines = 0, ironsmelters = 0;

    unsigned myMilitary = 0, myBuildings = 0;
    unsigned bestEnemyMilitary = 0, bestEnemyBuildings = 0;

    bool hasGold = true; // false in gold->granite / gold-removed rulesets
};

EconStats gatherEconStats(const AIContext& ctx, unsigned gf);

} // namespace AIllm
