#include "AICombat.h"
#include "Player.h"
#include "AIWorker.h"
#include "NPCBotsConfig.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "botmgr.h"
#include "Chat/Chat.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include "Random.h"
#include <unordered_map>

std::string GetCreatureRaceName(Unit* unit)
{
    if (!unit)
    {
        return "Unknown";
    }
    
    // ---------- Player races (WotLK only) ----------
    if (unit->GetTypeId() == TYPEID_PLAYER)
    {
        Player* player = unit->ToPlayer();
        switch (player->getRace())
        {
            case RACE_HUMAN: return "Human";
            case RACE_ORC: return "Orc";
            case RACE_DWARF: return "Dwarf";
            case RACE_NIGHTELF: return "Night Elf";
            case RACE_UNDEAD_PLAYER: return "Undead";
            case RACE_TAUREN: return "Tauren";
            case RACE_GNOME: return "Gnome";
            case RACE_TROLL: return "Troll";
            case RACE_BLOODELF: return "Blood Elf";
            case RACE_DRAENEI: return "Draenei";
            default: return "Unknown Player Race";
        }
    }

    // ---------- Creature / NPC classification ----------
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = unit->ToCreature();
        if (!creature || !creature->GetCreatureTemplate())
        {
            return "Unknown Creature";
        }
        
        // 1. NAME-BASED DETECTION (strongest)
        std::string name = creature->GetName();
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        static const std::vector<std::pair<std::string, std::string>> raceKeywords =
        {
            {"murloc", "Murloc"},
            {"naga", "Naga"},
            {"kobold", "Kobold"},
            {"gnoll", "Gnoll"},
            {"ogre", "Ogre"},
            {"harpy", "Harpy"},
            {"satyr", "Satyr"},
            {"dragon", "Dragon"},
            {"drake", "Dragon"},
            {"whelp", "Dragon"},
            {"ghoul", "Undead"},
            {"skeleton", "Undead"},
            {"zombie", "Undead"},
            {"scarlet", "Human"},
            {"cultist", "Cultist"},
            {"bandit", "Human"},
            {"pirate", "Human"},
            {"vrykul", "Vrykul"},
            {"tuskarr", "Tuskarr"},
            {"nerub", "Nerubian"},
            {"wolf", "wolf"},
            {"bear", "bear"},
            {"spider", "spider"},
            {"boar", "boar"},
            {"faceless", "Faceless One"}
        };

        for (const auto& [key, value] : raceKeywords)
        {
            if (lower.find(key) != std::string::npos)
                return value;
        }

        // 2. CREATURE TYPE (fallback)
        uint32 type = creature->GetCreatureTemplate()->type;

        switch (type)
        {
            case CREATURE_TYPE_BEAST: return "Beast";
            case CREATURE_TYPE_DRAGONKIN: return "Dragonkin";
            case CREATURE_TYPE_DEMON: return "Demon";
            case CREATURE_TYPE_ELEMENTAL: return "Elemental";
            case CREATURE_TYPE_GIANT: return "Giant";
            case CREATURE_TYPE_UNDEAD: return "Undead";
            case CREATURE_TYPE_HUMANOID: return "Humanoid";
            case CREATURE_TYPE_CRITTER: return "Critter";
            case CREATURE_TYPE_MECHANICAL: return "Mechanical";
            case CREATURE_TYPE_TOTEM: return "Totem";
            case CREATURE_TYPE_NON_COMBAT_PET: return "Pet";
            case CREATURE_TYPE_GAS_CLOUD: return "Gas Cloud";
            default: return "Creature";
        }
    }

    return "Unknown";
}

/// ============================
/// Personality system
/// ============================
static std::string GetPersonality(Creature* bot)
{
    uint32 entry = bot->GetEntry();

    switch (entry % 3)
    {
        case 0: return "funny";
        case 1: return "toxic";
        case 2: return "roleplay";
    }

    return "normal";
}

/// ============================
/// Personality rules
/// ============================
static std::string GetPersonalityRules(const std::string& personality)
{
    if (personality == "funny")
        return "Be humorous, sarcastic, and playful.";

    if (personality == "toxic")
        return "Be rude, arrogant, and insulting but not obscene.";

    if (personality == "roleplay")
        return "Speak like a true Warcraft character, immersive and serious.";

    return "Be neutral.";
}

/// ============================
/// Combat chatter timer storage
/// ============================
static std::unordered_map<uint64, uint32> combatTalkTimer;

// hard limiter for AI calls per bot
static std::unordered_map<uint64, uint32> lastAIRequestTime;

/// ============================
/// ENTER COMBAT (15% chance)
/// ============================
void AICombat::OnEnterCombat(Creature* bot, Unit*)
{

    if (!NPCBotsConfig::Enabled || !NPCBotsConfig::CombatChatterEnabled)
    {
        return;
    }

    if (!roll_chance_i(NPCBotsConfig::CombatOnEnterChance))
    {
        return;
    }
    
    // Declare all variables once
    std::string personality = GetPersonality(bot);
    std::string rules = GetPersonalityRules(personality);

    Unit* victim = bot->GetVictim();
    std::string enemyName = victim ? victim->GetName() : "enemy";
    std::string enemyRace = GetCreatureRaceName(victim);
    
    if (enemyRace == "Unknown")
    {
        enemyRace = "";
    }
    // Build AI prompt
    std::string prompt = "You are a World of Warcraft NPC named " + bot->GetName() + ".\n"
    "The world includes Azeroth, Outland, and Northrend.\n"
    "You are entering combat. React naturally to the situation.\n";

    if (!enemyRace.empty())
    {
        prompt += "You are fighting " + enemyName + " (" + enemyRace + ").\n";
    }
    else
    {
        prompt += "You are fighting " + enemyName + ".\n";
    }

    prompt += "Never use the words 'mortal' or 'adventurer'.\n"
    "Personality: " + personality + "\n"
    + rules + "\n"
    "Use ONE very short combat shout only (max 5 words).\n";
    if (!enemyRace.empty())
    {
        prompt += "Sometimes refer to the enemy by name (" + enemyName + ") or race (" + enemyRace + ") naturally, but not always.\n";
    }
    else
    {
        prompt += "Sometimes refer to the enemy by name (" + enemyName + ") naturally, but not always.\n";
    }

    // Enqueue request
    Player* owner = bot->GetBotOwner();
    if (!owner)
    {
        return;
    }
    
    uint64 playerGuid = owner->GetGUID().GetRawValue();
    
    uint32 now = getMSTime();
    uint64 botGuid = bot->GetGUID().GetRawValue();

    // combat AI spam limiter per bot
    auto it = lastAIRequestTime.find(botGuid);

    if (it != lastAIRequestTime.end())
    {
        if (now - it->second < NPCBotsConfig::CombatAIMinInterval)
        {
            return;
        }
    }

    lastAIRequestTime[botGuid] = now;

    if (!AIWorker::CanPlayerSpeak(playerGuid, NPCBotsConfig::GlobalTalkDelay))
    {
        return;
    }

    AIWorker::EnqueueRequest({
        playerGuid,
        bot->GetGUID().GetRawValue(),
        0,
        prompt
    });
  
}

/// ============================
/// LOW HP (10% chance)
/// ============================
void AICombat::OnDamageTaken(Creature* bot, uint32&)
{

    if (!NPCBotsConfig::Enabled || !NPCBotsConfig::CombatChatterEnabled)
    {
        return;
    }

    if (bot->GetHealthPct() >= 30.0f)
    {
        return;
    }
    
    if (!roll_chance_i(NPCBotsConfig::CombatDamageChance))
    {
        return;
    }   

    std::string personality = GetPersonality(bot);
    std::string rules = GetPersonalityRules(personality);

    Unit* victim = bot->GetVictim();
    std::string targetName = victim ? victim->GetName() : "enemy";

    std::string prompt =
        "You are a World of Warcraft NPC named " + bot->GetName() + ".\n"
        "You are about to die fighting " + targetName + ".\n"
        "Never use the words 'mortal' or 'adventurer'.\n"
        "Personality: " + personality + "\n"
        + rules + "\n"
        "Use ONE very short panic line (max 5 words).";

    Player* owner = bot->GetBotOwner();
    if (!owner)
    {
        return;
    }
    
    uint64 playerGuid = owner->GetGUID().GetRawValue();

    if (!AIWorker::CanPlayerSpeak(playerGuid, NPCBotsConfig::GlobalTalkDelay))
    {
        return;
    }

    AIWorker::EnqueueRequest({
        playerGuid,
        bot->GetGUID().GetRawValue(),
        0,
        prompt
    });
}

/// ============================
/// MID-FIGHT CHATTER
/// ============================
void AICombat::UpdateCombat(Creature* bot, uint32 diff)
{

    if (!NPCBotsConfig::Enabled || !NPCBotsConfig::CombatChatterEnabled)
    {
        return;
    }
    
    uint64 guid = bot->GetGUID().GetRawValue();
    
    Unit* victim = bot->GetVictim();
    
    Player* owner = bot->GetBotOwner();
    if (!owner)
    {
        return;
    }
    
    BotMap const* bots = owner->GetBotMgr()->GetBotMap();
    uint32 botCount = bots ? bots->size() : 1;
    // Combat chatter is automatically scaled by number of bots:
    // - More bots = slower and less frequent chatter
    // - Timer is increased (min/max * scale)
    // - Chance is reduced (chance / scale)
    // - Additional suppression applied for large groups (>6 bots)
    // Internal scaling: reduces chatter frequency in larger groups
    // Scale increases based on bot count (botCount / 4)
    // Effects:
    // - Increases chatter delay (min/max time * scale)
    // - Reduces chatter chance (chance / scale)
    // Result: larger groups = less frequent combat chatter (prevents spam)
    uint32 scale = std::max(1u, botCount / 4);

    /// Initialize timer
    if (combatTalkTimer.find(guid) == combatTalkTimer.end())
    {
        // Reminder new code line will be removed when working
        uint32 minTime = NPCBotsConfig::CombatChatterMinTime * scale;
        uint32 maxTime = NPCBotsConfig::CombatChatterMaxTime * scale;

        combatTalkTimer[guid] = urand(minTime, maxTime);
        // end reminder
    }

    /// Countdown
    if (combatTalkTimer[guid] <= diff)
    {
        
        // Reminder new code
        // combatTalkTimer[guid] = 0; // No need to zero, we overwrite it anyway

        uint32 minTime = NPCBotsConfig::CombatChatterMinTime * scale;
        uint32 maxTime = NPCBotsConfig::CombatChatterMaxTime * scale;

        combatTalkTimer[guid] = urand(minTime, maxTime);
        // reminder
        

        // Scale down chance in larger groups to further reduce chatter spam
        if (NPCBotsConfig::CombatUpdateChance == 0)
        {
            return;
        }
        
        uint32 scaledChance = NPCBotsConfig::CombatUpdateChance / scale;
        scaledChance = std::max(5u, scaledChance);

        if (!roll_chance_i(scaledChance))
        {
            return;
        }

        std::string personality = GetPersonality(bot);
        std::string rules = GetPersonalityRules(personality);

        std::string targetName = "enemy";

        if (victim)
        {
            std::string name = victim->GetName();
            if (!name.empty())
            {
                targetName = name;
            }
        }
        
        std::string enemyRace = GetCreatureRaceName(victim);
        // printf("[AI DEBUG] Name: %s | Race: %s\n", targetName.c_str(), enemyRace.c_str());

        if (enemyRace == "None" || enemyRace == "Unknown" || enemyRace.empty())
        {
            enemyRace = "";
        }
        
        std::string prompt = "You are a World of Warcraft NPC named " + bot->GetName() + ".\n";
       
        if (!enemyRace.empty())
        {
            prompt += "The enemy is " + targetName + " (" + enemyRace + ").\n";
        }
        else
        {
            prompt += "The enemy is " + targetName + ".\n";
        }
        
        prompt += "You are fighting the enemy.\n";
        prompt += "Never speak as the enemy.\n";
        prompt += "Never use the words 'mortal' or 'adventurer'.\n";
        prompt += "Personality: " + personality + "\n";
        prompt += rules + "\n";
        prompt += "Use ONE very short combat line (max 5 words).";
        
        uint32 now = getMSTime();

        // combat AI spam limiter per bot
        auto it = lastAIRequestTime.find(guid);

        if (it != lastAIRequestTime.end())
        {
            if (now - it->second < NPCBotsConfig::CombatAIMinInterval)
            {
                return;
            }
        }

        uint64 playerGuid = owner->GetGUID().GetRawValue();
    
        // Extra suppression: reduce chatter further in large groups
        if (botCount > 6 && urand(0, 100) < 50)
        {
            return;
        }
        
        lastAIRequestTime[guid] = now;
        // Reminder new code
        if (!AIWorker::CanPlayerSpeak(playerGuid, NPCBotsConfig::GlobalTalkDelay))
        {
            return;
        }
    
    
        AIWorker::EnqueueRequest({
            playerGuid,
            bot->GetGUID().GetRawValue(),
            0,
            prompt
        });
    
    }
    else
    {
        combatTalkTimer[guid] -= diff;
    }
}

/// ============================
/// VICTORY LINE (15% chance)
/// ============================
void AICombat::OnCombatEnd(Creature* bot)
{

    if (!NPCBotsConfig::Enabled || !NPCBotsConfig::CombatChatterEnabled)
    {
        return;
    }

    if (!roll_chance_i(NPCBotsConfig::CombatVictoryChance))
    {
        return;
    }
    
    std::string personality = GetPersonality(bot);
    std::string rules = GetPersonalityRules(personality);

    std::string prompt =
        "You are a World of Warcraft NPC named " + bot->GetName() + ".\n"
        "The enemy has been defeated.\n"
        "Never use the words 'mortal' or 'adventurer'.\n"
        "Personality: " + personality + "\n"
        + rules + "\n"
        "Say a short victory line (max 8 words).";

    Player* owner = bot->GetBotOwner();
    if (!owner)
    {
        return;
    }

    uint64 playerGuid = owner->GetGUID().GetRawValue();

    if (!AIWorker::CanPlayerSpeak(playerGuid, NPCBotsConfig::GlobalTalkDelay))
    {
        return;
    }
    
    AIWorker::EnqueueRequest({
        playerGuid,
        bot->GetGUID().GetRawValue(),
        0,
        prompt
    });
}
