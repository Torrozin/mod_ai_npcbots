#include "AIBanter.h"
#include "AIWorker.h"
#include "NPCBotsConfig.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Creature.h"     
#include "botmgr.h"       // <-- important, gives Player->GetBotMgr() and BotMap
#include <inttypes.h> // For PRIu64
#include "Random.h"
#include "SharedDefines.h"
#include "DBCStores.h"

#include <vector>
#include <algorithm>
#include <random>
#include <deque>
#include <unordered_map>
#include <string>

extern std::unordered_map<uint64, std::deque<std::string>> playerGlobalMemory;
extern std::unordered_map<uint64, std::deque<std::string>> botMemory;
/// Get bot faction name for AI prompt
/// Get bot faction name for AI prompt
static std::string GetBotFactionName(Creature* bot)
{
    if (!bot)
        return "Neutral";

    Player* owner = bot->GetBotOwner();

    // NPCBots follow owner faction
    if (owner)
    {
        return owner->GetTeamId() == TEAM_ALLIANCE
            ? "Alliance"
            : "Horde";
    }

    return "Neutral";
}

// static uint32 banterTimer = 0; //old
uint32 AIBanter::banterTimer = 0;
// Track when each bot last spoke
static std::unordered_map<uint64, uint32> botLastSpeak;

// Track the next allowed speak time per bot
static std::unordered_map<uint64, uint32> botNextSpeakTime;

// Player timing state
std::unordered_map<uint64, uint32> playerNextBanterTime;
std::unordered_map<uint64, uint32> playerLastUpdate;

template<typename T>
void CleanupBotMap(T& map)
{
    for (auto itr = map.begin(); itr != map.end(); )
    {
        bool exists = false;

        auto const& players = ObjectAccessor::GetPlayers();

        for (auto const& pair : players)
        {
            Player* player = pair.second;

            if (!player)
                continue;

            Creature* bot = ObjectAccessor::GetCreature(
                *player,
                ObjectGuid(itr->first));

            if (bot && bot->IsInWorld())
            {
                exists = true;
                break;
            }
        }

        if (!exists)
            itr = map.erase(itr);
        else
            ++itr;
    }
}

// conversation chain tracking
std::unordered_map<uint64, uint32> globalConversationChain;
std::unordered_map<uint64, uint32> lastConversationTime;

/// Find nearby bot (safe AzerothCore version)
Creature* AIBanter::FindNearbyBot(Player* player, Creature* excludeBot)
{
    if (!player)
        return nullptr;

    BotMap const* bots = player->GetBotMgr()->GetBotMap();
    if (!bots || bots->empty())
        return nullptr;

    for (auto const& pair : *bots)
    {
        Creature* bot = pair.second;
        if (!bot) continue;

        // Skip the excluded bot
        if (bot == excludeBot)
            continue;

        // Check distance manually
        if (bot->GetDistance(player) <= 50.0f)
        {
            printf("FindNearbyBot: Found bot %s at distance %.1f\n", bot->GetName().c_str(), bot->GetDistance(player));
            return bot;
        }
    }

    printf("FindNearbyBot: no other bots found within 50 yards\n");
    return nullptr;
}

/// Personality system
static std::string GetPersonality(Creature* bot)
{
    // uint32 entry = bot->GetEntry(); // we do not need this anymore
    uint64 guid = bot->GetGUID().GetRawValue();
    switch ((guid + bot->GetEntry()) % 7)
    {
        case 0: return "funny";
        case 1: return "toxic";
        case 2: return "roleplay";
        case 3: return "serious";
        case 4: return "brave";
        case 5: return "grumpy";
        case 6: return "curious";
        
    }

    return "normal";
}

/// Personality rules
static std::string GetPersonalityRules(const std::string& personality)
{
    if (personality == "funny")
        return "Be humorous, sarcastic, and playful. Occasionally tease others.";

    if (personality == "toxic")
        return "Be rude, arrogant, and dismissive, but not obscene.";

    if (personality == "roleplay")
        return "Speak like a true Warcraft character, immersive and serious.";

    if (personality == "serious")
        return "Be calm, focused, and practical. Avoid jokes and stay on topic.";
    
    if (personality == "brave")
        return "Be bold, fearless, and eager for danger. Push others toward action and risk.";
    
    if (personality == "grumpy")
        return "Be grumpy, blunt, and easily annoyed. Complain about things but stay in character.";
    
    if (personality == "curious")
        return "Be curious and observant. Ask questions, explore ideas, and show interest in everything around you.";

    return "Be neutral.";
}
static std::string GetClassWorldview(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR:
            return "Battle-hardened. Values strength, honor, and combat. Distrusts magic users and prefers direct action.";

        case CLASS_PALADIN:
            return "Devout and righteous. Sees the world as light versus darkness. Distrusts dark magic and corruption.";

        case CLASS_HUNTER:
            return "Survivalist. Judges places by danger and wildlife. Distrusts cities and crowded places. Feels more comfortable in the wild.";

        case CLASS_ROGUE:
            return "Cynical and stealthy. Trusts no one completely. Always looking for hidden motives.";

        case CLASS_PRIEST:
            return "Spiritual. Sees balance between light and shadow. Believes in redemption. Judges actions morally, not practically.";

        case CLASS_DEATH_KNIGHT:
            return "Haunted and grim. Has seen the horrors of undeath. Detached from normal emotions. Finds grim humor in dark situations.";

        case CLASS_SHAMAN:
            return "Connected to the elements. Feels imbalance in the world. Distrusts unnatural forces. Sees the world as interconnected.";

        case CLASS_MAGE:
            return "Curious and intelligent. Fascinated by magic and strange worlds. Sometimes overanalyzes situations. Values knowledge over strength.";

        case CLASS_WARLOCK:
            return "Drawn to dark powers. Finds demonic energy intriguing. Sees demons as tools, not threats. Enjoys unsettling others.";

        case CLASS_DRUID:
            return "Connected to nature. Dislikes corruption and unnatural lands. Feels at home in the wild. Prefers balance over extremes.";

        default:
            return "A seasoned adventurer with varied experiences.";
    }
}

void AIBanter::Update(uint32 diff)
{
    // HARD DISABLE — stop EVERYTHING immediately
    if (!NPCBotsConfig::Enabled || !NPCBotsConfig::BanterChatterEnabled)
    {
        return;
    }   
    
    // Update module timer
    banterTimer += diff;
    
    static uint32 cleanupTimer = 0;
    cleanupTimer += diff;

    if (cleanupTimer >= 600000) // every 10 minutes
    {
        cleanupTimer = 0;

        CleanupBotMap(botMemory);
        CleanupBotMap(botLastSpeak);
        CleanupBotMap(botNextSpeakTime);
    }

    // Do NOT early return anymore — we process continuously
    // and handle timing per-player instead

    // uint32 interval = NPCBotsConfig::UpdateInterval;
    // uint32 nowGlobal = getMSTime();

    // Process ALL players in world
    auto const& players = ObjectAccessor::GetPlayers();

    for (auto const& pair : players)
    {
        Player* player = pair.second;
        if (!player)
            continue;

        // Configurable global player-level cooldown
        uint32 now = getMSTime();
        uint64 playerGuid = player->GetGUID().GetRawValue();

        // Per-player update timing (DESYNC FIX)
        if (playerLastUpdate.find(playerGuid) == playerLastUpdate.end())
        {
            // Initial offset based on GUID (stable, no randomness needed)
            uint32 offset = (playerGuid * 7919u) % NPCBotsConfig::UpdateInterval;
            playerLastUpdate[playerGuid] = now - offset;
        }

        uint32 baseInterval = NPCBotsConfig::UpdateInterval;

        // add jitter to break timing patterns
        uint32 jitter = urand(0, NPCBotsConfig::GlobalJitter);

        // Keep your GUID-based offset (good design)
        // Stable desynced offset per player
        uint32 offset = ((playerGuid * 7919u) ^ (playerGuid >> 8)) % baseInterval;

        // Final next update time
        uint32 requiredDelay = baseInterval + offset + jitter;

        if (now - playerLastUpdate[playerGuid] < requiredDelay)
            continue;

        // Update timestamp
        playerLastUpdate[playerGuid] = now;

        if (lastConversationTime.find(playerGuid) == lastConversationTime.end())
        {
            lastConversationTime[playerGuid] = now;
        }
        bool isConversationActive = (now - lastConversationTime[playerGuid]) < NPCBotsConfig::ConversationResetTime;
 
        // Reset chain if too much time passed (10 sec = new conversation)
        if (now - lastConversationTime[playerGuid] > NPCBotsConfig::ConversationResetTime)
        {
            globalConversationChain[playerGuid] = 0;
        }
    
        // use existing playerGuid (do not redeclare)
        // uint64 playerGuid = player->GetGUID().GetRawValue();

        if (playerNextBanterTime.find(playerGuid) == playerNextBanterTime.end())
            playerNextBanterTime[playerGuid] = 0;

        if (!isConversationActive && (int32)(now - playerNextBanterTime[playerGuid]) < 0)
        {
            continue;
        }
    
        // Get bots in range
        BotMap const* bots = player->GetBotMgr()->GetBotMap();
        if (!bots || bots->empty())
        {
            continue;
        }   
        // PRIORITY SYSTEM: Combat > Banter
        // If ANY bot of this player is in combat → stop ALL banter for this player
        bool playerInCombat = false;

        for (auto const& pair : *bots)
        {
            Creature* bot = pair.second;
            if (!bot)
            {
                continue;
            }
    
            if (bot->IsInCombat())
            {
                playerInCombat = true;
                    break;
            }
        }

        if (playerInCombat)
        {
            continue; // skip this player entirely
        }
    
        std::vector<Creature*> botList;
        for (auto const& pair : *bots)
        {
            Creature* bot = pair.second;
            if (!bot) continue;

            // Skip bots in combat
            if (bot->IsInCombat())
                continue;

            if (bot->GetDistance(player) <= 50.0f)
                botList.push_back(bot);
        }
    if (botList.empty())
        continue;

    // Shuffle bots
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(botList.begin(), botList.end(), g);

    //  Filter eligible bots (per-bot cooldown + recent speaker penalty)
    std::vector<Creature*> eligibleBots;
    for (auto* bot : botList)
    {
        uint64 guid = bot->GetGUID().GetRawValue();

        if (botNextSpeakTime.find(guid) == botNextSpeakTime.end())
            botNextSpeakTime[guid] = 0;

        // Must pass cooldown first
        if ((int32)(now - botNextSpeakTime[guid]) < 0)
            continue;

        //  NEW: RecentSpeakerPenalty
        if (botLastSpeak.find(guid) != botLastSpeak.end())
        {
            if (now - botLastSpeak[guid] < NPCBotsConfig::RecentSpeakerPenalty)
                continue;
        }

        eligibleBots.push_back(bot);
    }
    
    if (eligibleBots.empty())
        continue;

    std::shuffle(eligibleBots.begin(), eligibleBots.end(), g);

    // Pick bot1 and bot2
    Creature* bot1 = eligibleBots[0];
    Creature* bot2 = eligibleBots.size() >= 2 ? eligibleBots[1] : nullptr;

    // SingleBotChance affects bot2 only
    if (bot2 && !roll_chance_i(NPCBotsConfig::SingleBotChance))
        bot2 = nullptr;

    // Swap if only bot2 left
    if (!bot1 && bot2)
    {
        bot1 = bot2;
        bot2 = nullptr;
    }

    if (!bot1)
        continue;
    
    // HARD LIMIT: stop conversation after 5 replies
    if (globalConversationChain[playerGuid] >= NPCBotsConfig::MaxChainLength)
        continue;
    
    // GLOBAL + LOCAL memory combined
    std::string memoryText;
    std::vector<std::string> combinedMemory;
    
    if (bot1)
    {
        auto& dq1 = botMemory[bot1->GetGUID().GetRawValue()];
        combinedMemory.insert(combinedMemory.end(), dq1.begin(), dq1.end());
    }

    if (bot2)
    {
        auto& dq2 = botMemory[bot2->GetGUID().GetRawValue()];
        combinedMemory.insert(combinedMemory.end(), dq2.begin(), dq2.end());
    }

    // 1. Global memory first (world memory)
    auto& globalMemory = playerGlobalMemory[playerGuid];
    size_t maxLines = NPCBotsConfig::MemoryMaxLines;
    size_t startGlobal = globalMemory.size() > maxLines ? globalMemory.size() - maxLines : 0;
    
    for (size_t i = startGlobal; i < globalMemory.size(); ++i)
    {
        memoryText += globalMemory[i] + "\n";
    }
    
    // Local bot memory intentionally not appended here.
    // Global memory already contains formatted named lines
    // and is properly trimmed to MemoryMaxLines.
    // Keeping local storage intact for future systems/debugging.
          
    // Names and personalities
    std::string name1 = bot1->GetName();
    std::string name2 = bot2 ? bot2->GetName() : "";
    uint8 class1 = bot1->GetClass();
    uint8 class2 = bot2 ? bot2->GetClass() : 0;
    std::string worldview1 = GetClassWorldview(class1);
    std::string worldview2 = bot2 ? GetClassWorldview(class2) : "";
    std::string personality1 = GetPersonality(bot1);
    std::string personality2 = bot2 ? GetPersonality(bot2) : "funny";
    std::string rules1 = GetPersonalityRules(personality1);
    std::string rules2 = bot2 ? GetPersonalityRules(personality2) : "";
    
    uint32 zoneId = player->GetZoneId();
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId);

    uint32 areaId = player->GetAreaId();
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);

    std::string zoneName = zone ? zone->area_name[0] : "Unknown";
    std::string areaName = area ? area->area_name[0] : zoneName;
    
    // Build AI prompt
    std::string faction1 = GetBotFactionName(bot1);
    std::string faction2 = bot2 ? GetBotFactionName(bot2) : "";

    // Start prompt
    std::string prompt = R"(You are NPC companions traveling with a player in World of Warcraft.

The world includes Azeroth, Outland, and Northrend.
Speak like adventurers sharing experiences from these lands.

Stay in character based on your personality, faction, and worldview.
You are allies, but may disagree in a natural, friendly way.

Avoid repetition in wording, tone, and structure. Keep dialogue natural and varied.
Do not overuse words like 'whispers', 'shadows', or 'echoes'.

After a few exchanges (3–5 lines), gradually shift the conversation to a related or new topic.
Keep transitions natural and avoid abrupt or frequent topic changes.

Each character may interpret topics differently based on their personality.
Each line should respond to or build on the previous one.

Keep replies short and concise.
Maximum 12 words.
Do not narrate.
Do not explain actions.
)";

prompt += "Location: " + areaName + " in " + zoneName + ".\n";
prompt += R"(
Characters are aware of their surroundings but are not required to mention them.

When the location changes, avoid continuing topics tied to the previous location.
Gradually guide the conversation toward a new or relevant topic.
You may briefly acknowledge the change, but do not dwell on it.
)";
    
    if (!memoryText.empty())
    {
    prompt += "Previous conversation:\n";
    prompt += memoryText + "\n";
    prompt += "Do not repeat lines from the previous conversation.\n";
    }

    prompt += "Character 1: " + name1 + " (" + personality1 + ", " + faction1 + ")\n";
    prompt += "Character 1 worldview: " + worldview1 + "\n";

    if (bot2)
    {
    prompt += "Character 2: " + name2 + " (" + personality2 + ", " + faction2 + ")\n";
    prompt += "Character 2 worldview: " + worldview2 + "\n";
    prompt += "Reply with EXACTLY 2 lines.\n";
    prompt += "Each line must be on its own line separated by a newline.\n";
    prompt += "Do not combine lines. Do not use a single sentence.\n";
    prompt += "Do not add explanations or extra text.\n";
    prompt += "Line 1 must be spoken by " + name1 + ".\n";
    prompt += "Line 2 must be spoken by " + name2 + ".\n";
    prompt += "Output format:\n";
    prompt += "<line 1>\n";
    prompt += "<line 2>\n";
    prompt += "Only these two characters are speaking.\n";
    // prompt += "Do not address or mention any other names from the conversation.\n";
    prompt += "Line 1 may say other characters names in this dialog, do NOT say '" + name1 + "' in this line.\n";
    prompt += "Line 2 may say other characters names in this dialog, do NOT say '" + name2 + "' in this line.\n";
    prompt += "Line 1 may sometimes answer back to the character who says your '" + name1 + "' in the dialog\n";
    prompt += "Line 2 may sometimes answer back to the character who says your '" + name2 + "' in the dialog.\n";
    
    }
    else
    {
    prompt += "Reply with 1 line.\n";
    // prompt += "Do not say your own name.\n";
    prompt += "You may say other characters names in your dialog, do NOT say '" + name1 + "' in your line.\n";
    
    }
    
    // if (!NPCBotsConfig::Enabled)
    // {
    // return;
    // }
    // Increase chain length
    globalConversationChain[playerGuid]++;
    lastConversationTime[playerGuid] = now;
    
        
    //  Send request to AIWorker
    if (!AIWorker::CanPlayerSpeak(playerGuid, NPCBotsConfig::GlobalTalkDelay))
    {
        continue;
    }
    
       
    AIWorker::EnqueueRequest({
    playerGuid, // IMPORTANT FIX
    bot1->GetGUID().GetRawValue(),
    bot2 ? bot2->GetGUID().GetRawValue() : 0,
    prompt
    });

    // Update per-bot cooldowns
    uint32 minC = NPCBotsConfig::BotCooldownMin;
    uint32 maxC = NPCBotsConfig::BotCooldownMax;
    if (maxC < minC) std::swap(minC, maxC);

    botNextSpeakTime[bot1->GetGUID().GetRawValue()] = now + urand(minC, maxC);
    if (bot2)
        botNextSpeakTime[bot2->GetGUID().GetRawValue()] = now + urand(minC, maxC);

    botLastSpeak[bot1->GetGUID().GetRawValue()] = now;
    if (bot2)
        botLastSpeak[bot2->GetGUID().GetRawValue()] = now;

    // Update configurable player-level cooldown from conf
    playerNextBanterTime[playerGuid] = now + NPCBotsConfig::PlayerBanterCooldown;
    } // END player loop
}
