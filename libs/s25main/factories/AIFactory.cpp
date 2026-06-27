// Copyright (C) 2005 - 2021 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AIFactory.h"
#include "ai/DummyAI.h"
#include "ai/aijh/AIPlayerJH.h"
#include "ai/llm/AIPlayerLlm.h"
#include "gameTypes/AIInfo.h"

std::unique_ptr<AIPlayer> AIFactory::Create(const AI::Info& aiInfo, unsigned playerId, const GameWorldBase& world)
{
    switch(aiInfo.type)
    {
        case AI::Type::Dummy: return std::make_unique<DummyAI>(playerId, world, aiInfo.level); break;
        case AI::Type::Llm: return std::make_unique<AIllm::AIPlayerLlm>(playerId, world, aiInfo.level); break;
        case AI::Type::Default:
        default:
        {
            // Reuse the difficulty selector (settable in the lobby UI) to pick between the new
            // improved AI and the original baseline AI, so both can play in the same game for A/B
            // comparison without any extra UI. The "easy" slot is repurposed as the baseline: it runs
            // the original strategy but at the same hard cadence as the improved AI, so an easy-vs-hard
            // match differs only in strategy - a fair comparison matching the ai-battle --baseline tool.
            const bool useImproved = aiInfo.level != AI::Level::Easy;
            const AI::Level level = useImproved ? aiInfo.level : AI::Level::Hard;
            return std::make_unique<AIJH::AIPlayerJH>(playerId, world, level, useImproved);
        }
    }
}
