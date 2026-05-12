#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "botmgr.h"
#include "Group.h"
#include "Map.h"
#include "CellImpl.h"
#include "Config.h"
#include "Log.h"

#include <cstdio>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <deque>
#include <regex>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>

#include "AIBanter.h"
#include "AIWorker.h"
#include "AICombat.h"
#include "NPCBotsConfig.h"

namespace
{
    std::thread g_AIStartupThread;
    std::atomic<bool> g_AIStartupCancelled = false;
}

void AddSC_BotPartyCommands();

std::string RemoveTrailingName(const std::string& line, const std::string& botName)
{
    std::regex pattern(R"([\s]*[-/]\s*)" + botName + R"(\s*$)");
    return std::regex_replace(line, pattern, "");
}

static void TrimChatLine(std::string& line)
{
    line.erase(0, line.find_first_not_of(" \t\r\n"));

    size_t end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos)
    {
        line.clear();
        return;
    }

    line.erase(end + 1);
}

std::unordered_map<uint64, std::deque<std::string>> botMemory;  // stores previous lines per bot
static std::atomic<bool> AIShuttingDown(false);
static uint32 g_AICleanupTimer = 0;
std::unordered_map<uint64, std::deque<std::string>> playerGlobalMemory;
extern std::unordered_map<uint64, uint32> lastConversationTime;
extern std::unordered_map<uint64, uint32> globalConversationChain;
extern std::unordered_map<uint64, uint32> playerNextBanterTime;
extern std::unordered_map<uint64, uint32> playerLastUpdate;
static std::unordered_map<uint64, uint32> botLastSpeak;

static Creature* SafeGetCreature(WorldObject* context, uint64 guid)
{
    Creature* c = ObjectAccessor::GetCreature(*context, ObjectGuid(guid));

    if (!c)
        return nullptr;

    if (!c->IsInWorld())
        return nullptr;

    if (c->IsDuringRemoveFromWorld())
        return nullptr;

    return c;
}

/// ============================
/// Player Chat Hook (AIChat removed)
/// ============================
class AIPlayerScript : public PlayerScript
{
public:
    AIPlayerScript() : PlayerScript("AIPlayerScript") {}

    void OnPlayerText(Player* player, std::string& msg)
    {
        if (!player || msg.empty())
            return;

        // Must have a selected target
        Unit* target = player->GetSelectedUnit();
        if (!target)
            return;

        Creature* bot = target->ToCreature();
        if (!bot)
            return;

        // Only respond to NPCBots owned by the player
        if (!bot->IsNPCBot() || bot->GetBotOwner() != player)
            return;

        // Optional: distance check
        if (bot->GetDistance(player) > 50.0f)
            return;

        // Debug: show detection
        player->SendSystemMessage("DEBUG: Selected bot detected: " + bot->GetName());

        // AIChat removed
        // Previous line: AIChat::HandlePlayerMessage(player, bot, msg);

        // Debug: confirm enqueue (optional, can keep for testing)
        player->SendSystemMessage("DEBUG: Player message handled for bot " + bot->GetName());
    }
    
    void OnLogout(Player* player)
    {
        if (!player)
            return;

        uint64 playerGuid = player->GetGUID().GetRawValue();

        playerNextBanterTime.erase(playerGuid);
        playerLastUpdate.erase(playerGuid);

        AIWorker::CleanupPlayerTalkTime(playerGuid);
    }
};

/// ============================
/// World Update (NPCBot AI Core)
/// ============================
class AIWorldScript : public WorldScript
{
public:
    AIWorldScript() : WorldScript("AIWorldScript") {}

    std::unordered_set<uint64> activeCombat;

    void OnUpdate(uint32 diff)
    {
    
        if (!NPCBotsConfig::Enabled || AIShuttingDown.load())
        {
            return;
        }
        
        g_AICleanupTimer += diff;

        if (g_AICleanupTimer >= 300000) // every 5 minutes
        {
            g_AICleanupTimer = 0;

            auto CleanupPlayerMap = [](auto& map)
            {
                for (auto itr = map.begin(); itr != map.end(); )
                {
                    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(itr->first));

                    if (!player || !player->IsInWorld())
                    {
                        itr = map.erase(itr);
                    }
                    else
                    {
                        ++itr;
                    }
                }
            };

            CleanupPlayerMap(playerGlobalMemory);
            CleanupPlayerMap(lastConversationTime);
            CleanupPlayerMap(globalConversationChain);
            // Banter timing cleanup
            CleanupPlayerMap(playerNextBanterTime);
            CleanupPlayerMap(playerLastUpdate);
        }
        
        AIBanter::Update(diff);

        auto const& players = ObjectAccessor::GetPlayers();

        for (auto const& pair : players)
        {
            Player* player = pair.second;
            if (!player)
                continue;

            BotMap const* bots = player->GetBotMgr()->GetBotMap();
            if (!bots)
                continue;

            for (auto const& botPair : *bots)
            {
                Creature* bot = botPair.second;
                if (!bot)
                    continue;

                uint64 guid = bot->GetGUID().GetRawValue();

                if (bot->IsInCombat())
                {
                    if (activeCombat.find(guid) == activeCombat.end())
                    {
                        activeCombat.insert(guid);
                        AICombat::OnEnterCombat(bot, bot->GetVictim());
                    }

                    AICombat::UpdateCombat(bot, diff);

                    if (bot->GetHealthPct() < 30.0f)
                    {
                        uint32 dummy = 0;
                        AICombat::OnDamageTaken(bot, dummy);
                    }
                }
                else
                {
                    if (activeCombat.find(guid) != activeCombat.end())
                    {
                        Player* owner = bot->GetBotOwner();
                        bool wipe = true;

                        if (owner && owner->IsAlive())
                            wipe = false;

                        if (owner && owner->GetGroup())
                        {
                            Group* group = owner->GetGroup();

                            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                            {
                                Player* member = itr->GetSource();
                                if (member && member->IsAlive())
                                {
                                    wipe = false;
                                    break;
                                }
                            }
                        }

                        if (!wipe)
                        {
                            AICombat::OnCombatEnd(bot);
                        }

                        activeCombat.erase(guid);
                    }
                }
            }
        }

                    /// 🔹 Handle AI responses (GLOBAL, not per player)
            AIResponse res;
            while (!AIShuttingDown.load() && AIWorker::PopResponse(res))
            {
                if (AIShuttingDown)
                    break;
                
                Player* owner = ObjectAccessor::FindPlayer(ObjectGuid(res.playerGUID));
                if (!owner || !owner->IsInWorld())
                    continue;
                
                Creature* bot1 = SafeGetCreature(owner, res.botGUID);
                Creature* bot2 = SafeGetCreature(owner, res.botGUID2);

                // 🔒 HARD SAFETY CHECKS (fix for _AddAura crash)
                if (!bot1 || !bot1->IsInWorld() || bot1->IsDuringRemoveFromWorld())
                    continue;

                if (bot2 && (!bot2->IsInWorld() || bot2->IsDuringRemoveFromWorld()))
                    bot2 = nullptr;

                std::string text = res.text;
                std::string originalText = text;

                if (!text.empty() && text.front() == '"') text.erase(0, 1);
                if (!text.empty() && text.back() == '"') text.pop_back();

                size_t pos;
                while ((pos = text.find("\\n")) != std::string::npos)
                    text.replace(pos, 2, "\n");
                
                /// 🔹 REMOVE <...> TAGS (GLOBAL) — only if needed
                if (text.find('<') != std::string::npos)
                {
                    size_t start;
                    while ((start = text.find('<')) != std::string::npos)
                    {
                        size_t end = text.find('>', start);

                        // If no closing > → remove everything after <
                        if (end == std::string::npos)
                        {
                            text.erase(start);
                            break;
                        }

                        text.erase(start, end - start + 1);
                    }
                }

                if (text.find("  ") != std::string::npos)
                {
                    text.erase(std::unique(text.begin(), text.end(),
                        [](char a, char b) { return a == ' ' && b == ' '; }),
                        text.end());
                }

                std::vector<std::string> lines;
                std::stringstream ss(text);
                std::string line;

                while (std::getline(ss, line))
                {
                    TrimChatLine(line);

                    if (line.empty())
                        continue;

                    /// 🔹 EXTRA SAFETY: remove any <...> left in this line
                    if (line.find('<') != std::string::npos)
                    {
                        size_t lstart = line.find('<');
                        while (lstart != std::string::npos)
                        {
                            size_t lend = line.find('>', lstart);

                            if (lend == std::string::npos)
                            {
                                line.erase(lstart);
                                break;
                            }

                            line.erase(lstart, lend - lstart + 1);
                            lstart = line.find('<');
                        }
                    }
               

                    size_t colonPos = line.find(": ");
                    if (colonPos != std::string::npos && colonPos < 25)
                        line = line.substr(colonPos + 2);

                    if (!line.empty() && line.front() == '"') line.erase(0, 1);
                    if (!line.empty() && line.back() == '"') line.pop_back();

                    TrimChatLine(line);

                    if (line.empty())
                        continue;

                    std::string lower = line;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c)
                        {
                            return std::tolower(c);
                        });

                    bool isMeta =
                    (lower.find("here are") != std::string::npos ||
                     lower.find("here's") != std::string::npos ||
                     lower.find("heres") != std::string::npos ||
                     lower.find("here is") != std::string::npos ||
                     lower.find("i suggest") != std::string::npos ||
                     lower.find("you could") != std::string::npos ||
                     lower.find("you can") != std::string::npos ||
                     lower.find("below") != std::string::npos ||
                     lower.find("following") != std::string::npos)
                    &&
                    (lower.find("conversation") != std::string::npos ||
                     lower.find("response") != std::string::npos ||
                     lower.find("responses") != std::string::npos ||
                     lower.find("reply") != std::string::npos ||
                     lower.find("replies") != std::string::npos ||
                     lower.find("dialogue") != std::string::npos ||
                     lower.find("exchange") != std::string::npos ||
                     lower.find("line") != std::string::npos ||
                     lower.find("lines") != std::string::npos);

                    if (isMeta)
                        continue;

                    if (lower.find("i'm ready") != std::string::npos) continue;
                    if (line.length() > 200) continue;

                    std::string bot1Name;
                    std::string bot2Name;

                    if (bot1 && bot1->IsInWorld() && !bot1->IsDuringRemoveFromWorld())
                        bot1Name = bot1->GetName();

                    if (bot2 && bot2->IsInWorld() && !bot2->IsDuringRemoveFromWorld())
                        bot2Name = bot2->GetName();

                    if (!bot1Name.empty())
                        line = RemoveTrailingName(line, bot1Name);

                    if (!bot2Name.empty())
                        line = RemoveTrailingName(line, bot2Name);

                    lines.push_back(line);
                }

                if (lines.empty())
                {
                    TrimChatLine(originalText);

                    if (!originalText.empty() &&
                        originalText != "..." &&
                        originalText.find("ERROR") == std::string::npos &&
                        originalText.length() <= 200)
                    {
                        lines.push_back(originalText);
                    }
                }

                if (bot1 && bot1->IsInWorld() && !bot1->IsDuringRemoveFromWorld() && !lines.empty() &&
                    lines[0] != "..." &&
                    lines[0].find("ERROR") == std::string::npos)
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, bot1, owner, lines[0]);
                    owner->GetSession()->SendPacket(&data);
                }

                if (bot2 && lines.size() == 1)
                    lines.push_back("...");

                if (bot2 && bot2->IsInWorld() && !bot2->IsDuringRemoveFromWorld() && lines.size() > 1 &&
                    lines[1] != "..." &&
                    lines[1].find("ERROR") == std::string::npos)
                {
                    WorldPacket data;
                    ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_SAY, LANG_UNIVERSAL, bot2, owner, lines[1]);
                    owner->GetSession()->SendPacket(&data);
                }

                uint64 playerGuid = owner->GetGUID().GetRawValue();

                if (bot1 && !lines.empty())
                {
                    if (lines[0] != "..." && lines[0].find("ERROR") == std::string::npos)
                    {
                        auto& mem1 = botMemory[bot1->GetGUID().GetRawValue()];
                        mem1.push_back(lines[0]);

                        while (mem1.size() > NPCBotsConfig::MemoryMaxLines)
                            mem1.pop_front();

                        auto& globalMemory = playerGlobalMemory[playerGuid];
                        globalMemory.push_back(bot1->GetName() + ": " + lines[0]);

                        while (globalMemory.size() > NPCBotsConfig::MemoryMaxLines)
                            globalMemory.pop_front();
                    }
                }

                lastConversationTime[playerGuid] = getMSTime();

                if (bot2 && lines.size() > 1)
                {
                    if (lines[1] != "..." && lines[1].find("ERROR") == std::string::npos)
                    {
                        auto& mem2 = botMemory[bot2->GetGUID().GetRawValue()];
                        mem2.push_back(lines[1]);

                        while (mem2.size() > NPCBotsConfig::MemoryMaxLines)
                            mem2.pop_front();

                        auto& globalMemory = playerGlobalMemory[playerGuid];
                        globalMemory.push_back(bot2->GetName() + ": " + lines[1]);

                        while (globalMemory.size() > NPCBotsConfig::MemoryMaxLines)
                            globalMemory.pop_front();
                    }
                }
            }

    }
};

/// ============================
/// Module init
/// ============================
class mod_ai_npcbots : public WorldScript
{
public:
    mod_ai_npcbots() : WorldScript("mod_ai_npcbots") { }

    static bool Enabled;
    static std::string Endpoint;
    static std::string Endpoints;
    static std::string LoadBalancingMode;

    static uint32 SingleBotChance;
    static uint32 UpdateInterval;
    static uint32 BotCooldownMin;
    static uint32 BotCooldownMax;
    static uint32 MemoryMaxLines;

void OnStartup() override
{
AIShuttingDown = false;

NPCBotsConfig::Enabled = sConfigMgr->GetOption<bool>("AI.Enabled", NPCBotsConfig::Enabled);
NPCBotsConfig::BanterChatterEnabled = sConfigMgr->GetOption<bool>("Banter.ChatterEnabled", NPCBotsConfig::BanterChatterEnabled);
NPCBotsConfig::CombatChatterEnabled = sConfigMgr->GetOption<bool>("Combat.ChatterEnabled", NPCBotsConfig::CombatChatterEnabled);
NPCBotsConfig::Endpoint = sConfigMgr->GetOption<std::string>("AI.Endpoint", NPCBotsConfig::Endpoint);
// 🔹 Load balancing mode
NPCBotsConfig::LoadBalancingMode = sConfigMgr->GetOption<std::string>("AI.LoadBalancingMode", "single");

// 🔹 Multiple endpoints (comma-separated)
std::string endpointsStr = sConfigMgr->GetOption<std::string>("AI.Endpoints", "");

// Clear default endpoints if config is provided
if (!endpointsStr.empty())
{
    NPCBotsConfig::Endpoints.clear();

    std::stringstream ss(endpointsStr);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        // Trim spaces (important!)
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);

        if (!item.empty())
            NPCBotsConfig::Endpoints.push_back(item);
    }
}

NPCBotsConfig::SingleBotChance = sConfigMgr->GetOption<uint32>("Banters.SingleBotChance", NPCBotsConfig::SingleBotChance);
NPCBotsConfig::UpdateInterval = sConfigMgr->GetOption<uint32>("Banters.UpdateInterval", NPCBotsConfig::UpdateInterval);
NPCBotsConfig::BotCooldownMin = sConfigMgr->GetOption<uint32>("Banters.BotCooldownMin", NPCBotsConfig::BotCooldownMin);
NPCBotsConfig::BotCooldownMax = sConfigMgr->GetOption<uint32>("Banters.BotCooldownMax", NPCBotsConfig::BotCooldownMax);
NPCBotsConfig::MemoryMaxLines = sConfigMgr->GetOption<uint32>("Banters.MemoryMaxLines", NPCBotsConfig::MemoryMaxLines);
NPCBotsConfig::PlayerBanterCooldown = sConfigMgr->GetOption<uint32>("Banters.PlayerBanterCooldown", NPCBotsConfig::PlayerBanterCooldown);
NPCBotsConfig::MaxChainLength = sConfigMgr->GetOption<uint32>("Banters.MaxChainLength", NPCBotsConfig::MaxChainLength);
NPCBotsConfig::ConversationResetTime = sConfigMgr->GetOption<uint32>("Banters.ConversationResetTime", NPCBotsConfig::ConversationResetTime);

NPCBotsConfig::GlobalJitter = sConfigMgr->GetOption<uint32>("Banters.GlobalJitter", NPCBotsConfig::GlobalJitter);
NPCBotsConfig::RecentSpeakerPenalty = sConfigMgr->GetOption<uint32>("Banters.RecentSpeakerPenalty", NPCBotsConfig::RecentSpeakerPenalty);

NPCBotsConfig::WorkerThreads = sConfigMgr->GetOption<uint32>("AI.WorkerThreads", NPCBotsConfig::WorkerThreads);
NPCBotsConfig::GlobalTalkDelay = sConfigMgr->GetOption<uint32>("AI.GlobalTalkDelay", NPCBotsConfig::GlobalTalkDelay);
NPCBotsConfig::MaxQueueSize = sConfigMgr->GetOption<uint32>("AI.MaxQueueSize", NPCBotsConfig::MaxQueueSize);
NPCBotsConfig::MaxActiveRequestsPerPlayer = sConfigMgr->GetOption<uint32>("AI.MaxActiveRequestsPerPlayer", NPCBotsConfig::MaxActiveRequestsPerPlayer);
NPCBotsConfig::CombatAIMinInterval = sConfigMgr->GetOption<uint32>("AI.CombatAIMinInterval", NPCBotsConfig::CombatAIMinInterval);
NPCBotsConfig::HttpTimeoutMs = sConfigMgr->GetOption<uint32>("AI.HttpTimeoutMs", NPCBotsConfig::HttpTimeoutMs);
NPCBotsConfig::HttpResponseTimeoutMs = sConfigMgr->GetOption<uint32>("AI.HttpResponseTimeoutMs", NPCBotsConfig::HttpResponseTimeoutMs);

NPCBotsConfig::CombatChatterMinTime = sConfigMgr->GetOption<uint32>("Combat.ChatterMinTime", NPCBotsConfig::CombatChatterMinTime);
NPCBotsConfig::CombatChatterMaxTime = sConfigMgr->GetOption<uint32>("Combat.ChatterMaxTime", NPCBotsConfig::CombatChatterMaxTime);
NPCBotsConfig::CombatOnEnterChance = sConfigMgr->GetOption<uint32>("Combat.OnCombatChance", NPCBotsConfig::CombatOnEnterChance);
NPCBotsConfig::CombatUpdateChance = sConfigMgr->GetOption<uint32>("Combat.UpdateCombatChance", NPCBotsConfig::CombatUpdateChance);
NPCBotsConfig::CombatDamageChance = sConfigMgr->GetOption<uint32>("Combat.OnDamageChance", NPCBotsConfig::CombatDamageChance);
NPCBotsConfig::CombatVictoryChance = sConfigMgr->GetOption<uint32>("Combat.VictoryChance", NPCBotsConfig::CombatVictoryChance);
    
        
    // ========================================
    // 🔥 Banter config validation & safety
    // ========================================

    // SingleBotChance clamp (0–100)
    if (NPCBotsConfig::SingleBotChance > 100)
    {
    NPCBotsConfig::SingleBotChance = 100;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.SingleBotChance > 100. Clamped to 100.\n");
    printf("\033[0m");
    }

    // UpdateInterval must never be 0 because banter uses it for GUID-based modulo timing.
    if (NPCBotsConfig::UpdateInterval == 0)
    {
    NPCBotsConfig::UpdateInterval = 1;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.UpdateInterval was 0. Set to 1 ms.\n");
    printf("\033[0m");
    }

    // UpdateInterval vs BotCooldownMin
    if (NPCBotsConfig::UpdateInterval > NPCBotsConfig::BotCooldownMin)
    {
    NPCBotsConfig::UpdateInterval = NPCBotsConfig::BotCooldownMin;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.UpdateInterval > BotCooldownMin. Adjusted to match BotCooldownMin.\n");
    printf("\033[0m");
    }

    // BotCooldownMin vs BotCooldownMax
    if (NPCBotsConfig::BotCooldownMin > NPCBotsConfig::BotCooldownMax)
    {
    uint32 min = NPCBotsConfig::BotCooldownMin;
    uint32 max = NPCBotsConfig::BotCooldownMax;

    std::swap(NPCBotsConfig::BotCooldownMin, NPCBotsConfig::BotCooldownMax);

    printf("\033[1;33m");
    printf("[AI WARNING] Banters cooldown invalid (Min=%u > Max=%u). Values swapped.\n", min, max);
    printf("\033[0m");
    }

    // PlayerBanterCooldown vs UpdateInterval
    if (NPCBotsConfig::PlayerBanterCooldown < NPCBotsConfig::UpdateInterval)
    {
    NPCBotsConfig::PlayerBanterCooldown = NPCBotsConfig::UpdateInterval;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.PlayerBanterCooldown < UpdateInterval. Adjusted.\n");
    printf("\033[0m");
    }

    // ConversationResetTime vs PlayerBanterCooldown
    if (NPCBotsConfig::ConversationResetTime <= NPCBotsConfig::PlayerBanterCooldown)
    {
    NPCBotsConfig::ConversationResetTime = NPCBotsConfig::PlayerBanterCooldown + 1000;

    printf("\033[1;33m");
    printf("[AI WARNING] ConversationResetTime <= PlayerBanterCooldown. Adjusted.\n");
    printf("\033[0m");
    }

    // MaxChainLength sanity
    if (NPCBotsConfig::MaxChainLength == 0)
    {
    NPCBotsConfig::MaxChainLength = 1;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.MaxChainLength was 0. Set to 1.\n");
    printf("\033[0m");
    }

    // MemoryMaxLines sanity
    if (NPCBotsConfig::MemoryMaxLines == 0)
    {
    NPCBotsConfig::MemoryMaxLines = 10;

    printf("\033[1;33m");
    printf("[AI WARNING] Banters.MemoryMaxLines was 0. Set to 10.\n");
    printf("\033[0m");
    }

    // Validate combat chatter timing
    if (NPCBotsConfig::CombatChatterMinTime > NPCBotsConfig::CombatChatterMaxTime)
    {
    uint32 min = NPCBotsConfig::CombatChatterMinTime;
    uint32 max = NPCBotsConfig::CombatChatterMaxTime;

    std::swap(NPCBotsConfig::CombatChatterMinTime, NPCBotsConfig::CombatChatterMaxTime);

    printf("\033[1;33m");
    printf("[AI WARNING] Combat chatter invalid (Min=%u > Max=%u). Values swapped.\n", min, max);
    printf("\033[0m");
    }
    
    if (NPCBotsConfig::GlobalJitter > 30000) // 30 sec cap (reasonable)
    {
        printf("\033[1;33m");
        printf("[AI WARNING] Banters.GlobalJitter too high (%u). Clamped to 30000.\n", NPCBotsConfig::GlobalJitter);
        printf("\033[0m");

        NPCBotsConfig::GlobalJitter = 30000;
    }
    
    if (NPCBotsConfig::RecentSpeakerPenalty > NPCBotsConfig::BotCooldownMax)
    {
        printf("\033[1;33m");
        printf("[AI WARNING] RecentSpeakerPenalty > BotCooldownMax. Clamped.\n");
        printf("\033[0m");

        NPCBotsConfig::RecentSpeakerPenalty = NPCBotsConfig::BotCooldownMax;
    }
    
    if (NPCBotsConfig::RecentSpeakerPenalty < 1000)
    {
        printf("\033[1;33m");
        printf("[AI WARNING] RecentSpeakerPenalty very low (%u). May cause repetition.\n", NPCBotsConfig::RecentSpeakerPenalty);
        printf("\033[0m");
    }
    
    // ========================================
    // 🔥 Combat + AI config validation
    // ========================================

    // Clamp all combat chances (0–100)
    auto clampChance = [](uint32& val, const char* name)
    {
    if (val > 100)
    {
        printf("\033[1;33m");
        printf("[AI WARNING] %s > 100. Clamped to 100.\n", name);
        printf("\033[0m");
        val = 100;
    }
};

    clampChance(NPCBotsConfig::CombatOnEnterChance, "Combat.OnCombatChance");
    clampChance(NPCBotsConfig::CombatUpdateChance, "Combat.UpdateCombatChance");
    clampChance(NPCBotsConfig::CombatDamageChance, "Combat.OnDamageChance");
    clampChance(NPCBotsConfig::CombatVictoryChance, "Combat.VictoryChance");


    // Ensure GlobalTalkDelay is sane
    if (NPCBotsConfig::GlobalTalkDelay < 200)
    {
    NPCBotsConfig::GlobalTalkDelay = 200;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.GlobalTalkDelay too low. Set to minimum 200 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::GlobalTalkDelay > 10000)
    {
    NPCBotsConfig::GlobalTalkDelay = 10000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.GlobalTalkDelay too high. Clamped to 10000 ms.\n");
    printf("\033[0m");
    }


    // Worker threads sanity
    if (NPCBotsConfig::WorkerThreads == 0)
    {
    NPCBotsConfig::WorkerThreads = 1;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.WorkerThreads was 0. Set to 1.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::WorkerThreads > 16)
    {
    NPCBotsConfig::WorkerThreads = 16;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.WorkerThreads too high. Clamped to 16.\n");
    printf("\033[0m");
    }
    
    // ========================================
    // 🔥 AI safety limits validation
    // ========================================

    // MaxQueueSize sanity
    if (NPCBotsConfig::MaxQueueSize == 0)
    {
    NPCBotsConfig::MaxQueueSize = 1;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.MaxQueueSize was 0. Set to 1.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::MaxQueueSize > 1000)
    {
    NPCBotsConfig::MaxQueueSize = 1000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.MaxQueueSize too high. Clamped to 1000.\n");
    printf("\033[0m");
    }


    // MaxActiveRequestsPerPlayer sanity
    if (NPCBotsConfig::MaxActiveRequestsPerPlayer == 0)
    {
    NPCBotsConfig::MaxActiveRequestsPerPlayer = 1;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.MaxActiveRequestsPerPlayer was 0. Set to 1.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::MaxActiveRequestsPerPlayer > 10)
    {
    NPCBotsConfig::MaxActiveRequestsPerPlayer = 10;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.MaxActiveRequestsPerPlayer too high. Clamped to 10.\n");
    printf("\033[0m");
    }


    // CombatAIMinInterval sanity
    if (NPCBotsConfig::CombatAIMinInterval < 500)
    {
    NPCBotsConfig::CombatAIMinInterval = 500;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.CombatAIMinInterval too low. Set to 500 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::CombatAIMinInterval > 60000)
    {
    NPCBotsConfig::CombatAIMinInterval = 60000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.CombatAIMinInterval too high. Clamped to 60000 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::HttpTimeoutMs < 500)
    {
    NPCBotsConfig::HttpTimeoutMs = 500;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.HttpTimeoutMs too low. Set to 500 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::HttpTimeoutMs > 60000)
    {
    NPCBotsConfig::HttpTimeoutMs = 60000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.HttpTimeoutMs too high. Clamped to 60000 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::HttpResponseTimeoutMs < 1000)
    {
    NPCBotsConfig::HttpResponseTimeoutMs = 1000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.HttpResponseTimeoutMs too low. Set to 1000 ms.\n");
    printf("\033[0m");
    }

    if (NPCBotsConfig::HttpResponseTimeoutMs > 120000)
    {
    NPCBotsConfig::HttpResponseTimeoutMs = 120000;

    printf("\033[1;33m");
    printf("[AI WARNING] AI.HttpResponseTimeoutMs too high. Clamped to 120000 ms.\n");
    printf("\033[0m");
    }
    
    // just a check to see if all values are respected from conf
    printf("\033[1;36m"); // bright cyan

    printf("\n========================================\n");
    printf("   AI CONFIG LOADED\n");
    printf("========================================\n");
    
    printf(" AI: Enabled=%s\n",
    NPCBotsConfig::Enabled ? "Yes" : "No");
    
    printf(" Combat: Min=%u ms | Max=%u ms\n",
    NPCBotsConfig::CombatChatterMinTime,
    NPCBotsConfig::CombatChatterMaxTime);

    printf(" Banter: Chance=%u | Update=%u | MinCD=%u | MaxCD=%u\n",
    NPCBotsConfig::SingleBotChance,
    NPCBotsConfig::UpdateInterval,
    NPCBotsConfig::BotCooldownMin,
    NPCBotsConfig::BotCooldownMax);

    printf(" PlayerCD=%u | Chain=%u | Reset=%u | Memory=%u\n",
    NPCBotsConfig::PlayerBanterCooldown,
    NPCBotsConfig::MaxChainLength,
    NPCBotsConfig::ConversationResetTime,
    NPCBotsConfig::MemoryMaxLines);
    
    printf(" GlobalTalkDelay=%u ms\n",
    NPCBotsConfig::GlobalTalkDelay);

    printf(" Combat Chances: Enter=%u | Update=%u | Damage=%u | Victory=%u\n",
    NPCBotsConfig::CombatOnEnterChance,
    NPCBotsConfig::CombatUpdateChance,
    NPCBotsConfig::CombatDamageChance,
    NPCBotsConfig::CombatVictoryChance);
    
    printf(" AI Limits: Queue=%u | ActivePerPlayer=%u | CombatInterval=%u | HttpTimeout=%u | HttpResponseTimeout=%u\n",
    NPCBotsConfig::MaxQueueSize,
    NPCBotsConfig::MaxActiveRequestsPerPlayer,
    NPCBotsConfig::CombatAIMinInterval,
    NPCBotsConfig::HttpTimeoutMs,
    NPCBotsConfig::HttpResponseTimeoutMs);
    
    printf(" Workers: Threads=%u\n",
    NPCBotsConfig::WorkerThreads);
    
    printf(" AI Endpoint: %s\n",
    NPCBotsConfig::Endpoint.c_str());
    
    printf(" AI Endpoints: ");

    if (NPCBotsConfig::Endpoints.empty())
    {
        printf("(none)");
    }
    else
    {
        for (size_t i = 0; i < NPCBotsConfig::Endpoints.size(); ++i)
        {
            printf("%s", NPCBotsConfig::Endpoints[i].c_str());

            if (i < NPCBotsConfig::Endpoints.size() - 1)
                printf(", ");
        }
    }

printf("\n");

    printf(" Load Balancing: %s\n",
    NPCBotsConfig::LoadBalancingMode.c_str());

    printf("========================================\n\n");

    printf("\033[0m"); // reset color
    
    g_AIStartupCancelled = false;

    if (g_AIStartupThread.joinable())
        g_AIStartupThread.join();

    g_AIStartupThread = std::thread([]{

    for (int i = 0; i < 40; ++i)
    {
        if (g_AIStartupCancelled)
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (g_AIStartupCancelled)
        return;

    if (!NPCBotsConfig::Enabled)
        return;

    AIWorker::Start();
});

}

void OnShutdown() override
{
       
    printf("\033[1;33m[AI] Disabling AI before shutdown...\033[0m\n");

    printf("\033[1;33m[AI] Stopping AI worker...\033[0m\n");

    // 1. Disable new work
    AIShuttingDown = true;
    NPCBotsConfig::Enabled = false;
    
    // 2. STOP worker threads properly
    g_AIStartupCancelled = true;

    if (g_AIStartupThread.joinable())
        g_AIStartupThread.join();
    
    AIWorker::Stop();

        
}
};

/// ============================
/// Register
/// ============================
void Addmod_ai_npcbotsScripts()
{
    printf("--------AI MODULE LOADED---------\n");

    // ---------------------
    // Start AIWorker here — required for bot chatter
    // Safe shutdown guaranteed by RAII and non-blocking Stop()
    // ---------------------
    //AIWorker::Start();

    // Register scripts
    AddSC_BotPartyCommands();
    
    new AIPlayerScript();
    new AIWorldScript();
    new mod_ai_npcbots();
}

