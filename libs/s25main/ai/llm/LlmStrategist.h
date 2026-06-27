// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Strategist.h"
#include "gameTypes/Direction.h"
#include "gameTypes/MapCoordinates.h"
#include <iosfwd>
#include <string>
#include <vector>

namespace AIllm {

/// Strategist backed by an external model through a *file oracle*: each tick it writes a compact
/// JSON world snapshot to a spool directory and reads back a plan that a sidecar produced by calling
/// the LLM. The sidecar (extras/ai-battle/llm_sidecar.py) reaches any OpenAI-compatible endpoint
/// configured in .env, so the C++ side stays model-agnostic and needs no network code.
///
/// Robust by construction:
///  - A HeuristicStrategist provides the baseline every tick, so the AI always plays well even with
///    no sidecar, a slow model, or malformed output.
///  - Async by default (write request, apply the answer a tick later) so the game never blocks; a
///    blocking mode (blockMs > 0) gives reproducible, synchronous LLM-in-the-loop runs for testing.
///  - On persistent failure it keeps playing on the heuristic and surfaces a chat warning so the
///    player can pause, fix the setup, and resume.
class LlmStrategist final : public IStrategist
{
public:
    LlmStrategist(unsigned char playerId, std::string spoolDir, unsigned blockMs, Persona persona);

    void Update(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                Strategy& strategy) override;
    const std::string& lastRationale() const override { return rationale_; }
    const GamePlan& gamePlan() const override { return plan_; }
    std::string takeImportantMessage() override;

private:
    /// Which model tier a request targets. Cheap = fast tactician (every tick); Expensive = strong
    /// strategist (rare: opening, periodic deep-think, escalation). Carried inside the request JSON.
    enum class Tier
    {
        Cheap,
        Expensive
    };
    /// What kind of answer a request wants. Tick = light adaptation; Plan = a full long-lived GamePlan.
    enum class RequestKind
    {
        Tick,
        Plan
    };

    /// One of the 6 HQ-centered sectors (indexed by Direction). Distances are tile counts; -1 = none
    /// found in the scan radius. Built only for an expensive request and cached (SPEC §2.1.4, §5.3).
    struct SectorDigest
    {
        int room = 0;            // count of unowned buildable (>=Hut) tiles in this sector (open ground)
        int nearestIron = -1;    // distance to nearest unclaimed ore/stone, or -1 = none in radius
        int nearestCoal = -1;
        int nearestGranite = -1;
        int nearestStone = -1;
        bool hasWater = false;   // any sea point in this sector
        int enemyDir = -1;       // playerId of an enemy HQ lying in this sector, or -1
        int enemyDist = -1;      // distance to that enemy HQ, or -1
        bool chokepoint = false; // tight (room<thresh) AND facing an enemy -> a defensible neck
    };
    /// Full HQ-centered map picture (6 sectors). Expensive/plan-only, cached for mapRefreshGf_ frames.
    struct MapDigest
    {
        bool valid = false;
        MapPoint hq = MapPoint::Invalid();
        unsigned scanRadius = 0;
        SectorDigest sectors[6];
        unsigned builtGf = 0;
    };
    /// Per-opponent snapshot (rebuilt every tick, O(P)).
    struct OpponentInfo
    {
        int id = -1;
        unsigned military = 0;
        unsigned buildings = 0;
        unsigned country = 0;
        int trend = 0;         // -1 shrinking / 0 steady / +1 growing (vs last tick, dead-banded)
        bool attackingUs = false;
        int dirFromHq = -1;    // Direction index of the enemy HQ from our HQ, or -1
        int distFromHq = 255;  // distance to the enemy HQ, or 255 if our HQ is lost
    };
    /// All attackable opponents this tick, plus cheap extrema for the model's convenience.
    struct OpponentDigest
    {
        std::vector<OpponentInfo> enemies;
        int strongestId = -1;
        int weakestId = -1;
        int closestId = -1;
        unsigned builtGf = 0;
    };

    std::string requestPath(unsigned gf) const;
    std::string responsePath(unsigned gf) const;
    void writeRequest(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                      const Strategy& cur, Tier tier, RequestKind kind) const;
    /// Try to read+parse the response for `gf` into the held contracts. Returns true on success (and
    /// cleans up the request/response files). `out` carries the 7 knobs/persona/wantExpand; `plan`/
    /// `tick` carry the GamePlan/TickStrategy keys; `chat` carries free-text narration.
    bool tryReadResponse(unsigned gf, Strategy& out, GamePlan& plan, TickStrategy& tick, std::string& chat);
    /// Consume a previously-requested answer (shared by the async and blocking paths). Updates the held
    /// llmStrategy_/plan_/tick_ and bookkeeping; returns true if a usable answer was parsed.
    bool consume(unsigned gf, Strategy& base);
    /// Copy the held plan_/tick_ intent fields onto `out` so ApplyFocusToKnobs + the executor see them.
    void projectPlanAndTick(Strategy& out) const;
    /// Decide whether this tick should issue an expensive plan request (vs a cheap tick request).
    bool wantExpensivePlan(unsigned gf, const EconStats& s) const;
    /// Update trend/escalation memory from this tick's stats (sets escalatePending_ on big swings).
    void detectEvents(unsigned gf, const EconStats& s);
    /// Build the cached map digest (single bounded HQ-guarded radius pass). Expensive/plan-only.
    void buildMapDigest(const AIContext& ctx, unsigned gf);
    /// Build the per-opponent digest (O(P), every tick): trend, attackingUs proxy, bearing/distance.
    void buildOpponentDigest(const AIContext& ctx, unsigned gf, const EconStats& s);
    /// Emit the opponents[] array: full per-enemy when `full`, else the condensed top-2 by threat.
    static void appendOpponents(std::ostream& o, const OpponentDigest& od, bool full);
    /// Emit the mapDigest object (6 sectors + hints). Plan/expensive only; caller guards on validity.
    static void appendMapDigest(std::ostream& o, const MapDigest& md);

    unsigned char playerId_;
    std::string spoolDir_;
    unsigned blockMs_;

    HeuristicStrategist fallback_;
    Strategy llmStrategy_;     // last knob plan the model returned
    GamePlan plan_;            // long-lived plan (overwritten only by a plan response; kept otherwise)
    TickStrategy tick_;        // per-tick adaptation (reset of requestReplan each tick)
    bool haveLlm_ = false;     // have we ever received a plan?
    bool pending_ = false;     // is a request awaiting a response?
    unsigned pendingGf_ = 0;   // gf of the outstanding request
    RequestKind pendingKind_ = RequestKind::Tick; // kind of the outstanding request
    unsigned lastLlmGf_ = 0;   // gf the last knob plan was received
    unsigned lastWarnGf_ = 0;  // throttle failure warnings
    std::string rationale_;
    std::string pendingNotice_;     // IMPORTANT in-game message to surface once (fallback/recovery/new plan)
    std::string lastAnnouncedPlan_; // strategyName last announced in-game (so we chat a plan only when it changes)

    unsigned numPlayers_ = 0; // sized once; clamps player-id fields and trend memory

    // Tier/escalation bookkeeping.
    bool planEverRequested_ = false; // A3: distinguishes "never requested" from "requested at gf 0" so
                                     // the gf-0 opening (lastPlanReqGf_=0) doesn't disable the min-gap gate
    unsigned lastPlanReqGf_ = 0; // gf the last expensive plan request was issued
    unsigned lastPlanGf_ = 0;    // gf the last expensive plan answer was received
    bool escalatePending_ = false;
    unsigned lastSnapMil_ = 0, lastSnapNMil_ = 0, lastSnapBestEnemyMil_ = 0;
    std::vector<unsigned> lastEnemyMil_; // per-player military trend memory (sized to numPlayers_)

    // Digests (SPEC §5). Map digest is expensive/plan-only and cached; opponent digest every tick.
    MapDigest mapDigest_;
    OpponentDigest oppDigest_;

    static constexpr unsigned staleLimitGf_ = 4000;      // fall back to heuristic knobs if no fresh plan
    static constexpr unsigned replanIntervalGf_ = 20000; // periodic deep-think cadence
    static constexpr unsigned planMinGapGf_ = 4000;      // min gap between expensive plan requests
    static constexpr unsigned scanRadius_ = 40;          // map-digest scan radius (tiles)
    static constexpr unsigned mapRefreshGf_ = 15000;     // rebuild the (cached) map digest at most this often
    static constexpr int chokeRoomThresh_ = 12;          // sector room below this + facing an enemy = chokepoint
};

} // namespace AIllm
