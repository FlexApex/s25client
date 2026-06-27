// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "helpers/EnumArray.h"
#include "gameTypes/Direction.h"
#include <cstdint>
#include <string>

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

// --- Overlay enums (projected from GamePlan/TickStrategy onto Strategy). Every enum has an explicit
//     no-op FIRST member so "absent/garbage output == current behavior" is structural. ---

/// Coarse game phase. Auto = no-op (executor/knobs keep current behavior). DEFAULT Auto.
enum class Phase : uint8_t
{
    Auto,
    Open,
    Expand,
    Consolidate,
    Push,
    Defend
};

/// Focus tag (cheap tactician / heuristic). None = no-op. DEFAULT None.
enum class Focus : uint8_t
{
    None,
    SecureIron,
    SecureCoal,
    SecureStone,
    ExpandFront,
    BoomEconomy,
    AttackEnemy,
    Defend,
    Raid
};

/// Land-claim intent. Auto = no-op. Halt is the ONLY switch that sets wantExpand=false. DEFAULT Auto.
enum class ExpandIntent : uint8_t
{
    Auto,
    Halt,
    Steady,
    Hard
};

/// Attack posture. Auto = no-op (keep current reqRatio). Hold is a real cap signal. DEFAULT Auto.
enum class AttackIntent : uint8_t
{
    Auto,
    Hold,
    Probe,
    Commit,
    AllIn
};

/// Defensive stance. Auto = no-op. DEFAULT Auto.
enum class DefensePosture : uint8_t
{
    Auto,
    Loose,
    Firm,
    Fortress
};

/// Per-HQ-sector spatial intent, indexed by Direction (W,NW,NE,E,SE,SW). Auto = no spatial override
/// (executor uses its existing ore-march/borderland scoring). DEFAULT Auto.
enum class SectorRole : uint8_t
{
    Auto,
    MilitaryPush,
    ExpandEconomy,
    FarmBelt,
    MiningOutpost,
    Hold,
    Ignore
};

/// Long-lived plan produced by the expensive strategist tier. Held in LlmStrategist and projected
/// onto Strategy each tick. Defaults are all no-op (valid=false until the model ever produces a plan).
struct GamePlan
{
    bool valid = false;       // true once the expensive tier ever produced a plan
    std::string strategyName; // free text -> variety/debug
    std::string rationale;    // free text -> chat
    Phase phase = Phase::Auto;
    int primaryTargetEnemy = -1; // playerId or -1 = none
    int feintTargetEnemy = -1;   // playerId or -1 = none
    unsigned timingArmy = 0;     // commit when own military >= this (0 = no gate)
    unsigned timingMinute = 0;   // ...or when game minute >= this (0 = no gate)
    bool economicGambit = false; // boom now, militarize at trigger
    helpers::EnumArray<SectorRole, Direction> sectorRoles{}; // all Auto by default
    unsigned planGf = 0;                                     // gf this plan was received (staleness/age)
};

/// Per-tick adaptation produced by the cheap strategist tier. Held in LlmStrategist and projected
/// onto Strategy each tick. Defaults are all no-op.
struct TickStrategy
{
    std::string diagnosis; // <=120 chars free text -> chat
    Focus focusPrimary = Focus::None;
    Focus focusSecondary = Focus::None;
    ExpandIntent expandIntent = ExpandIntent::Auto;
    AttackIntent attackIntent = AttackIntent::Auto;
    DefensePosture defense = DefensePosture::Auto;
    bool requestReplan = false; // reset to false every tick (never keep prior)
    // The 7 knobs + persona + wantExpand may also appear in a tick response; they land on Strategy.
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

    // --- NEW overlay (projected from GamePlan/TickStrategy; folded into knobs by ApplyFocusToKnobs;
    //     a few read directly by the executor). All defaults are no-op. ---
    Phase phase = Phase::Auto;
    Focus focusPrimary = Focus::None;
    Focus focusSecondary = Focus::None;
    ExpandIntent expandIntent = ExpandIntent::Auto;
    AttackIntent attackIntent = AttackIntent::Auto;
    DefensePosture defense = DefensePosture::Auto;

    // Read DIRECTLY by the executor (not via knobs):
    int primaryTargetEnemy = -1;
    int feintTargetEnemy = -1;
    unsigned timingTriggerArmy = 0;
    unsigned timingTriggerMinute = 0;
    bool economicGambit = false;
    helpers::EnumArray<SectorRole, Direction> sectorRoles{}; // all Auto

    std::string diagnosis; // for chat/debug (no mechanical effect)
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

    // resource-distance awareness (255 = none found in radius)
    unsigned nearestIronDist = 255, nearestCoalDist = 255, nearestStoneDist = 255, nearestGraniteDist = 255;
    // chain-health diagnostics
    bool stoneStarved = false;    // stones < 20 (gates mil blds + smelters)
    bool ironChainBroken = false; // have mil blds but a weapons-chain link missing
    bool foodSecure = false;      // farms>=2 && (mills>0 || fisheries>0)
};

// withResourceScan: also run the bounded r=40 nearest-unclaimed-resource scans (iron/coal/granite/
// stone). These feed ONLY the LLM request snapshot, so the executor's hot per-frame calls leave it
// false (the no-LLM floor does zero radius scans, as in the validated M0 baseline); the strategist
// tick passes llmDriven_ so the scans run at most ~once / 1000 GF when a model is actually consuming them.
EconStats gatherEconStats(const AIContext& ctx, unsigned gf, bool withResourceScan = false);

} // namespace AIllm
