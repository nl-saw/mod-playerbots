/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it
 * and/or modify it under version 2 of the License, or (at your option), any later version.
 */

#include "CheckMountStateAction.h"
#include "BattlegroundWS.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellAuraEffects.h"

bool CheckMountStateAction::Execute(Event event)
{
    // Cache combat related flags
    bool noAttackers = !AI_VALUE2(bool, "combat", "self target") || !AI_VALUE(uint8, "attacker count");
    Unit* enemyPlayer = AI_VALUE(Unit*, "enemy player target");
    Unit* dpsTarget = AI_VALUE(Unit*, "dps target");
    
    bool shouldDismount = false;
    bool shouldMount = false;
    
    Unit* currentTarget = AI_VALUE(Unit*, "current target");
    if (currentTarget)
    {
        float dismountDistance = CalculateDismountDistance();
        float mountDistance = CalculateMountDistance();

        // Cache combat reach and distance calculations
        float botCombatReach = bot->GetCombatReach();
        float targetCombatReach = currentTarget->GetCombatReach();
        float combatReach = botCombatReach + targetCombatReach;
        float distanceToTarget = bot->GetExactDist(currentTarget);

        shouldDismount = (distanceToTarget <= dismountDistance + combatReach);
        shouldMount = (distanceToTarget > mountDistance + combatReach);
    }
    else
    {
        shouldMount = true;
    }
    
    // Dismount if needed when mounted
    if (bot->IsMounted() && shouldDismount)
    {
        Dismount();
        return true;
    }
    
    Player* master = GetMaster();
    bool inBattleground = bot->InBattleground();
    
    if (master && !inBattleground)
    {
        masterInShapeshiftForm = master->GetShapeshiftForm();

        Group* group = bot->GetGroup();
        if (!group || group->GetLeaderGUID() != master->GetGUID())
            return false;

        if (ShouldFollowMasterMountState(master, noAttackers, shouldMount))
            return Mount();

        if (ShouldDismountForMaster(master))
        {
            Dismount();
            return true;
        }
        
        return false;
    }
    
    if (!master && !inBattleground)
    {
        if (!bot->IsMounted() && noAttackers && shouldMount && !bot->IsInCombat())
            return Mount();
    }
    
    if (inBattleground && shouldMount && noAttackers && !bot->IsInCombat() && !bot->IsMounted())
    {
        // WSG Specific - Do not mount when carrying the flag
        if (bot->GetBattlegroundTypeId() == BATTLEGROUND_WS)
        {
            if (bot->HasAura(23333) || bot->HasAura(23335))
                return false;
        }
        return Mount();
    }
    
    if (!bot->IsFlying() && shouldDismount && bot->IsMounted() &&
        (enemyPlayer || dpsTarget || (!noAttackers && bot->IsInCombat())))
    {
        Dismount();
        return true;
    }
    
    return false;
}

bool CheckMountStateAction::isUseful()
{
    if (botAI->IsInVehicle() || bot->isDead() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT) ||
        !bot->IsOutdoors() || bot->InArena())
    {
        return false;
    }

    // Cache position and ground level once
    float posZ = bot->GetPositionZ();
    float groundLevel = bot->GetMapWaterOrGroundLevel(bot->GetPositionX(), bot->GetPositionY(), posZ);
    if (!bot->IsMounted() && posZ < groundLevel)
        return false;

    if (!GET_PLAYERBOT_AI(bot)->HasStrategy("mount", BOT_STATE_NON_COMBAT) && !bot->IsMounted())
        return false;

    if (bot->GetLevel() < sPlayerbotAIConfig->useGroundMountAtMinLevel)
        return false;

    if (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))
        return false;

    if (bot->HasAuraType(SPELL_AURA_TRANSFORM) && bot->IsInDisallowedMountForm())
        return false;

    if (bot->InBattleground())
    {
        if (Battleground* bg = bot->GetBattleground())
        {
            if (bg->GetStatus() == STATUS_WAIT_JOIN && bg->GetStartDelayTime() > BG_START_DELAY_30S)
                return false;
        }
    }
    
    return true;
}

bool CheckMountStateAction::Mount()
{
    if (bot->isMoving())
        bot->StopMoving();

    Player* master = GetMaster();
    botAI->RemoveShapeshift();
    botAI->RemoveAura("tree of life");

    int32 masterSpeed = CalculateMasterMountSpeed(master);
    bool hasSwiftMount = CheckForSwiftMount();

    auto allSpells = GetAllMountSpells();
    int32 masterMountType = GetMountType(master);

    if (TryPreferredMount(master))
        return true;

    // Cache the spells for the mount type
    auto spellsIt = allSpells.find(masterMountType);
    if (spellsIt != allSpells.end())
    {
        auto& spells = spellsIt->second;
        if (hasSwiftMount)
            FilterMountsBySpeed(spells, masterSpeed);

        if (TryRandomMount(spells))
            return true;
    }

    auto items = AI_VALUE2(std::vector<Item*>, "inventory items", "mount");
    if (!items.empty())
        return UseItemAuto(*items.begin());

    return false;
}

float CheckMountStateAction::CalculateDismountDistance() const
{
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float dismountDistance = isMelee ? sPlayerbotAIConfig->meleeDistance + 2.0f : sPlayerbotAIConfig->spellDistance + 2.0f;
    if (bot->getClass() == CLASS_WARRIOR)
        dismountDistance = std::max(18.0f, dismountDistance);
    return dismountDistance;
}

float CheckMountStateAction::CalculateMountDistance() const
{
    bool isMelee = PlayerbotAI::IsMelee(bot);
    float baseDistance = isMelee ? sPlayerbotAIConfig->meleeDistance + 10.0f : sPlayerbotAIConfig->spellDistance + 10.0f;
    return std::max(21.0f, baseDistance);
}

void CheckMountStateAction::Dismount()
{
    WorldPacket emptyPacket;
    bot->GetSession()->HandleCancelMountAuraOpcode(emptyPacket);
}

bool CheckMountStateAction::ShouldFollowMasterMountState(Player* master, bool noAttackers, bool shouldMount) const
{
    // Cache master mount state evaluation
    bool masterMounted = master->IsMounted();
    bool masterFlying = (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC || masterInShapeshiftForm == FORM_TRAVEL);
    bool isMasterMounted = masterMounted || masterFlying;
    
    return isMasterMounted && !bot->IsMounted() && noAttackers &&
           shouldMount && !bot->IsInCombat() && botAI->GetState() != BOT_STATE_COMBAT;
}

bool CheckMountStateAction::ShouldDismountForMaster(Player* master) const
{
    bool masterMounted = master->IsMounted();
    bool masterFlying = (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC || masterInShapeshiftForm == FORM_TRAVEL);
    return !(masterMounted || masterFlying) && bot->IsMounted();
}

int32 CheckMountStateAction::CalculateMasterMountSpeed(Player* master) const
{
    int32 ridingSkill = bot->GetPureSkillValue(SKILL_RIDING);
    int32 botLevel = bot->GetLevel();
    
    if (ridingSkill <= 75 && botLevel < static_cast<int32>(sPlayerbotAIConfig->useFastGroundMountAtMinLevel))
        return 59;

    if (master && !bot->InBattleground())
    {
        auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        if (!auraEffects.empty())
        {
            SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
            // Cache base points from both effects
            int32 effect1 = masterSpell->Effects[1].BasePoints;
            int32 effect2 = masterSpell->Effects[2].BasePoints;
            return std::max(effect1, effect2);
        }
        else if (masterInShapeshiftForm == FORM_FLIGHT_EPIC)
            return 279;
        else if (masterInShapeshiftForm == FORM_FLIGHT)
            return 149;
    }
    else
    {
        for (const auto& entry : bot->GetSpellMap())
        {
            uint32 spellId = entry.first;
            auto spellState = entry.second;
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;
            if (spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
                continue;
            if (spellState->State == PLAYERSPELL_REMOVED || !spellState->Active || spellInfo->IsPassive())
                continue;
            
            int32 effect1 = spellInfo->Effects[1].BasePoints;
            int32 effect2 = spellInfo->Effects[2].BasePoints;
            int32 speed = std::max(effect1, effect2);
            if (speed > 59)
            {
                if (bot->InBattleground())
                    return (speed > 99) ? 99 : speed;
                return speed;
            }
        }
    }
    
    return 59;
}

bool CheckMountStateAction::CheckForSwiftMount() const
{
    for (const auto& entry : bot->GetSpellMap())
    {
        uint32 spellId = entry.first;
        auto spellState = entry.second;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;
        if (spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;
        if (spellState->State == PLAYERSPELL_REMOVED || !spellState->Active || spellInfo->IsPassive())
            continue;
        
        int32 effect1 = spellInfo->Effects[1].BasePoints;
        int32 effect2 = spellInfo->Effects[2].BasePoints;
        int32 effect = std::max(effect1, effect2);
        // Check both for ground and flying mounts (with proper aura names)
        if ((effect > 59 && spellInfo->Effects[1].ApplyAuraName != SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ||
            (effect > 149 && spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
        {
            return true;
        }
    }
    return false;
}

std::map<uint32, std::map<int32, std::vector<uint32>>> CheckMountStateAction::GetAllMountSpells() const
{
    // Outer map: index (0 for ground, 1 for flight), inner map: effect speed => vector of spell IDs.
    std::map<uint32, std::map<int32, std::vector<uint32>>> allSpells;
    
    for (const auto& entry : bot->GetSpellMap())
    {
        uint32 spellId = entry.first;
        auto spellState = entry.second;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;
        if (spellInfo->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            continue;
        if (spellState->State == PLAYERSPELL_REMOVED || !spellState->Active || spellInfo->IsPassive())
            continue;
        
        int32 effect1 = spellInfo->Effects[1].BasePoints;
        int32 effect2 = spellInfo->Effects[2].BasePoints;
        int32 effect = std::max(effect1, effect2);
        
        // Determine the index: flight if aura name matches or for specific mount IDs.
        uint32 index = (spellInfo->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        spellInfo->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                        spellInfo->Id == 54729) ? 1 : 0;
        
        allSpells[index][effect].push_back(spellId);
    }
    
    return allSpells;
}

bool CheckMountStateAction::TryPreferredMount(Player* master) const
{
    static bool tableExists = false;
    static bool tableChecked = false;

    if (!tableChecked)
    {
        QueryResult checkTable = PlayerbotsDatabase.Query(
            "SELECT EXISTS(SELECT * FROM information_schema.tables WHERE table_schema = 'acore_playerbots' AND table_name = 'playerbots_preferred_mounts')");
        if (checkTable)
        {
            tableExists = (checkTable->Fetch()[0].Get<uint32>() == 1);
        }
        tableChecked = true;
    }
    
    if (tableExists)
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT spellid FROM playerbots_preferred_mounts WHERE guid = {} AND type = {}",
            bot->GetGUID().GetCounter(), GetMountType(master));
        
        if (result)
        {
            std::vector<uint32> mounts;
            do
            {
                mounts.push_back(result->Fetch()[0].Get<uint32>());
            } while (result->NextRow());
            
            if (!mounts.empty())
            {
                // Pick a random mount from the list
                uint32 index = urand(0, mounts.size() - 1);
                uint32 chosenSpell = mounts[index];
                if (sSpellMgr->GetSpellInfo(chosenSpell))
                    return botAI->CastSpell(chosenSpell, bot);
            }
        }
    }
    
    return false;
}

uint32 CheckMountStateAction::GetMountType(Player* master) const
{
    if (!master)
        return 0;

    auto auraEffects = master->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    if (!auraEffects.empty())
    {
        SpellInfo const* masterSpell = auraEffects.front()->GetSpellInfo();
        return (masterSpell->Effects[1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                masterSpell->Effects[2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) ? 1 : 0;
    }
    else if (masterInShapeshiftForm == FORM_FLIGHT || masterInShapeshiftForm == FORM_FLIGHT_EPIC)
        return 1;
    
    return 0;
}

void CheckMountStateAction::FilterMountsBySpeed(std::map<int32, std::vector<uint32>>& spells, int32 masterSpeed) const
{
    // Instead of iterating repeatedly, iterate through each key once.
    for (auto& pair : spells)
    {
        int32 currentSpeed = pair.first;
        // For each mount candidate, if its speed is too low relative to masterSpeed, clear the candidate vector.
        if (masterSpeed > 59 && currentSpeed < 99)
        {
            pair.second.clear();
        }
        if (masterSpeed > 149 && currentSpeed < 279)
        {
            pair.second.clear();
        }
    }
}

bool CheckMountStateAction::TryRandomMount(const std::map<int32, std::vector<uint32>>& spells) const
{
    // Iterate over each speed group. If one is non-empty, pick a random spell.
    for (const auto& pair : spells)
    {
        const auto& ids = pair.second;
        if (!ids.empty())
        {
            uint32 index = urand(0, ids.size() - 1);
            if (index < ids.size())
                return botAI->CastSpell(ids[index], bot);
        }
    }
    return false;
}
