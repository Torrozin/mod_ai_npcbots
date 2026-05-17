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
#include <string>
#include <unordered_map>

static std::unordered_map<uint64, uint32> lastCommandTime;

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
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);

        bool wantsFood = (text.find("bot food") != std::string::npos);
        bool wantsWater = (text.find("bot water") != std::string::npos);

        // Ignore unrelated messages
        if (!wantsFood && !wantsWater)
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

        return true;
    }
};

void AddSC_BotPartyCommands()
{
    new BotPartyCommandsScript();
}
