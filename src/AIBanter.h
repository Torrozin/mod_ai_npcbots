#pragma once

#include "Player.h"
#include "Creature.h"
#include "botmgr.h"

class AIBanter
{
public:
    // Find a nearby NPCBot. Exclude a specific bot if needed for dual-bot chatter
    static Creature* FindNearbyBot(Player* player, Creature* excludeBot = nullptr);

    // Called every update cycle to handle banter
    static void Update(uint32 diff);

private:
    static uint32 banterTimer;
};
