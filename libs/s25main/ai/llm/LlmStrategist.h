// Copyright (C) 2024 - 2026 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Strategist.h"
#include <string>

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

private:
    std::string requestPath(unsigned gf) const;
    std::string responsePath(unsigned gf) const;
    void writeRequest(unsigned gf, const AIContext& ctx, const EconStats& stats, bool contained,
                      const Strategy& current) const;
    /// Try to read+parse the response for `gf` into `out`. Returns true on success (and cleans up).
    bool tryReadResponse(unsigned gf, Strategy& out, std::string& chat);

    unsigned char playerId_;
    std::string spoolDir_;
    unsigned blockMs_;

    HeuristicStrategist fallback_;
    Strategy llmStrategy_;     // last plan the model returned
    bool haveLlm_ = false;     // have we ever received a plan?
    bool pending_ = false;     // is a request awaiting a response?
    unsigned pendingGf_ = 0;   // gf of the outstanding request
    unsigned lastLlmGf_ = 0;   // gf the last plan was received
    unsigned lastWarnGf_ = 0;  // throttle failure warnings
    std::string rationale_;

    static constexpr unsigned staleLimitGf_ = 4000; // fall back to heuristic if no fresh plan
};

} // namespace AIllm
