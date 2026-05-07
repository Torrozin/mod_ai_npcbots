#ifndef AICOMBAT_H
#define AICOMBAT_H

#include "Creature.h"
#include "Unit.h"

class AICombat
{
public:
    static void OnEnterCombat(Creature* bot, Unit* target);
    static void OnDamageTaken(Creature* bot, uint32& damage);
    static void UpdateCombat(Creature* bot, uint32 diff);
    
    static void OnCombatEnd(Creature* bot);
};

#endif
