#pragma once

#include "Define.h"

#include <string>
#include <vector>

namespace NPCBotsConfig
{
    inline bool Enabled = true;
    inline static bool BanterChatterEnabled = true;
    inline static bool CombatChatterEnabled = true;
    inline std::string Endpoint = "http://127.0.0.1:3000/chat";
    // Multiple endpoints support
    inline std::vector<std::string> Endpoints;

    // Load balancing mode: "single", "roundrobin"
    inline std::string LoadBalancingMode = "single";

    // Banter settings
    inline uint32 SingleBotChance = 65;         // % chance to allow dual-bot conversation (bot2 participates)
    inline uint32 UpdateInterval = 60000;       // Time in ms between checks
    inline uint32 BotCooldownMin = 120000;       // Minimum per-bot cooldown (ms)
    inline uint32 BotCooldownMax = 180000;       // Maximum per-bot cooldown (ms)
    inline uint32 MemoryMaxLines = 30;         // Max stored lines for BOTH bot memory and player global conversation memory
    // --- Banter timing polish ---
    inline uint32 GlobalJitter = 3000;          // extra randomness on global tick
    inline uint32 RecentSpeakerPenalty = 8000;  // prevent same bot speaking too soon

    // New: player-level cooldown
    inline uint32 PlayerBanterCooldown = 90000;  // ms between ANY bot lines per player
    
    inline uint32 MaxChainLength = 10; // fallback must match config default
    inline uint32 ConversationResetTime = 180000;
    
    // Amount of worker threads.
    inline uint32 WorkerThreads = 2;
    
    inline uint32 GlobalTalkDelay = 1500;
    
    //  AI safety limits
    inline uint32 MaxQueueSize = 20;            // Max pending AI requests
    inline uint32 MaxActiveRequestsPerPlayer = 1; // Max simultaneous AI calls per player
    inline uint32 CombatAIMinInterval = 5000;   // Min ms between combat AI calls per bot
    inline uint32 HttpTimeoutMs = 5000;         // Max ms for AI HTTP connect/send
    inline uint32 HttpResponseTimeoutMs = 30000; // Max ms to wait for an AI HTTP response
    
    // Combat chatter timing
    inline uint32 CombatChatterMinTime = 10000; // ms
    inline uint32 CombatChatterMaxTime = 30000; // ms
    
    // Combat chances (percent)
    inline uint32 CombatOnEnterChance = 40;
    inline uint32 CombatUpdateChance = 50;
    inline uint32 CombatDamageChance = 60;
    inline uint32 CombatVictoryChance = 40;
    
}
