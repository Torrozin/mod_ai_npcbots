#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "botmgr.h"
#include "bot_ai.h"
#include "botgossip.h"
#include "GossipDef.h"
#include "Chat.h"
#include "SharedDefines.h"
#include "ObjectAccessor.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

static std::unordered_map<uint64, uint32> lastCommandTime;

namespace
{
    static uint32 constexpr BOT_GOSSIP_ACTION_INFO_DEF = 1000;

    struct PortalCommand
    {
        char const* Command;
        uint32 SpellId;
        int32 Team;
        uint8 MinLevel;
    };

    static int32 constexpr TEAM_NEUTRAL_PORTAL = -1;

    static PortalCommand const PortalCommands[] =
    {
        // Alliance portals
        { "stormwind",     10059, TEAM_ALLIANCE, 40 },
        { "sw",            10059, TEAM_ALLIANCE, 40 },
        { "ironforge",     11416, TEAM_ALLIANCE, 40 },
        { "if",            11416, TEAM_ALLIANCE, 40 },
        { "darnassus",     11419, TEAM_ALLIANCE, 50 },
        { "darn",          11419, TEAM_ALLIANCE, 50 },
        { "exodar",        32266, TEAM_ALLIANCE, 40 },

        // Horde portals
        { "orgrimmar",     11417, TEAM_HORDE, 40 },
        { "org",           11417, TEAM_HORDE, 40 },
        { "undercity",     11418, TEAM_HORDE, 40 },
        { "uc",            11418, TEAM_HORDE, 40 },
        { "thunderbluff",  11420, TEAM_HORDE, 50 },
        { "thunder bluff", 11420, TEAM_HORDE, 50 },
        { "tb",            11420, TEAM_HORDE, 50 },
        { "silvermoon",    32267, TEAM_HORDE, 40 },

        // Outland/Northrend portals
        { "shattrath",     33691, TEAM_ALLIANCE, 65 },
        { "shatt",         33691, TEAM_ALLIANCE, 65 },
        { "shattrath",     35717, TEAM_HORDE, 65 },
        { "shatt",         35717, TEAM_HORDE, 65 },
        { "dalaran",       53142, TEAM_NEUTRAL_PORTAL, 74 },
    };

    static bool IsCommandBoundary(std::string const& text, size_t pos)
    {
        if (pos >= text.size())
            return true;

        return std::isspace(static_cast<unsigned char>(text[pos])) != 0;
    }

    static bool HasBotCommand(std::string const& text, std::string const& command)
    {
        std::string needle = "bot " + command;
        size_t pos = text.find(needle);

        while (pos != std::string::npos)
        {
            size_t before = pos;
            size_t after = pos + needle.length();

            if ((before == 0 || std::isspace(static_cast<unsigned char>(text[before - 1])) != 0) &&
                IsCommandBoundary(text, after))
            {
                return true;
            }

            pos = text.find(needle, pos + 1);
        }

        return false;
    }

    static PortalCommand const* FindPortalCommand(std::string const& text, TeamId team)
    {
        for (PortalCommand const& portal : PortalCommands)
        {
            if (portal.Team != TEAM_NEUTRAL_PORTAL && portal.Team != static_cast<int32>(team))
                continue;

            if (HasBotCommand(text, portal.Command))
                return &portal;
        }

        return nullptr;
    }
}

void CleanupBotPartyCommandPlayer(uint64 playerGuid)
{
    lastCommandTime.erase(playerGuid);
}

void CleanupBotPartyCommandStalePlayers()
{
    for (auto itr = lastCommandTime.begin(); itr != lastCommandTime.end(); )
    {
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid(itr->first));

        if (!player || !player->IsInWorld())
            itr = lastCommandTime.erase(itr);
        else
            ++itr;
    }
}

class BotPartyCommandsScript : public PlayerScript
{
public:
    BotPartyCommandsScript() : PlayerScript("BotPartyCommandsScript") { }

    bool OnPlayerCanUseChat(Player* player,
                        uint32 type,
                        uint32 /*language*/,
                        std::string& msg,
                        Group* /*group*/) override
    {
        if (!player || msg.empty())
            return true;

        // Party chat on this fork/core
        if (type != 51)
            return true;

        uint32 now = getMSTime();
        uint64 playerGuid = player->GetGUID().GetRawValue();

        // Prevent duplicate triggers
        auto itr = lastCommandTime.find(playerGuid);

        if (itr != lastCommandTime.end())
        {
            if (now - itr->second < 250)
                return true;
        }

        lastCommandTime[playerGuid] = now;

        // Convert message to lowercase
        std::string text = msg;
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

        bool wantsFood = HasBotCommand(text, "food");
        bool wantsWater = HasBotCommand(text, "water");
        PortalCommand const* portalCommand = FindPortalCommand(text, player->GetTeamId());

        // Ignore unrelated messages
        if (!wantsFood && !wantsWater && !portalCommand)
            return true;
        
        // Get player's bots
        BotMap const* bots = player->GetBotMgr()->GetBotMap();

        if (!bots || bots->empty())
            return true;

        Creature* mageBot = nullptr;

        // Find first owned mage bot
        for (auto const& pair : *bots)
        {
            Creature* bot = pair.second;

            if (!bot)
                continue;

            if (!bot->IsNPCBot())
                continue;

            if (bot->GetBotOwner() != player)
                continue;

            if (bot->GetClass() != CLASS_MAGE)
                continue;

            if (!bot->IsAlive())
                continue;

            if (bot->IsInCombat())
                continue;

            if (bot->GetDistance(player) > 50.0f)
                continue;

            mageBot = bot;
            break;
        }

        if (!mageBot)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "No available mage bot found.");

            return true;
        }

        if (wantsFood)
        {
            mageBot->GetBotAI()->OnGossipSelect(
                player,
                mageBot,
                GOSSIP_SENDER_CLASS,
                1001);

            return true;
        }

        if (wantsWater)
        {
            mageBot->GetBotAI()->OnGossipSelect(
                player,
                mageBot,
                GOSSIP_SENDER_CLASS,
                1002);

            return true;
        }

        if (portalCommand)
        {
            if (mageBot->GetLevel() < portalCommand->MinLevel)
                return true;

            mageBot->GetBotAI()->OnGossipSelect(
                player,
                mageBot,
                GOSSIP_SENDER_CLASS_ACTION1,
                BOT_GOSSIP_ACTION_INFO_DEF + portalCommand->SpellId);

            return true;
        }

        return true;
    }
};

void AddSC_BotPartyCommands()
{
    new BotPartyCommandsScript();
}
