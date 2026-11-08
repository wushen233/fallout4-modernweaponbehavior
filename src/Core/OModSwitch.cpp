/*
    This file is part of Modern Weapon Behavior.
    Modern Weapon Behavior is free software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.
    This file is additionally covered by the Modding Exception and GPL-3.0
    Linking Exception with Corresponding Source described in EXCEPTIONS.
*/

#include "OModSwitch.h"
#include "Core/EquippedWeapon.h"

#include <F4SE/API.h>

#include <RE/B/BGSInventoryItem.h>
#include <RE/B/BGSInventoryList.h>
#include <RE/B/BGSMod.h>
#include <RE/B/BGSObjectInstance.h>
#include <RE/B/BGSObjectInstanceExtra.h>
#include <RE/B/BipedAnim.h>
#include <RE/B/BSSpinLock.h>
#include <RE/E/EquippedItem.h>
#include <RE/E/EquippedWeaponData.h>
#include <RE/E/ExtraInstanceData.h>
#include <RE/M/MiddleHighProcessData.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESObjectWEAP.h>

#include <limits>

namespace OModSwitch
{
    namespace
    {
        RE::BGSInventoryItem* FindInventoryItem(RE::TESObjectREFR* a_owner, RE::TESBoundObject* a_item)
        {
            if (!a_owner || !a_owner->inventoryList || !a_item)
            {
                return nullptr;
            }

            for (auto& item : a_owner->inventoryList->data)
            {
                if (item.object == a_item)
                {
                    return &item;
                }
            }
            return nullptr;
        }

        RE::BGSInventoryItem::Stack* GetStack(RE::BGSInventoryItem* a_item, std::uint32_t a_stackIndex)
        {
            if (!a_item)
            {
                return nullptr;
            }

            auto* stack = a_item->stackData.get();
            while (a_stackIndex > 0 && stack)
            {
                stack = stack->nextStack.get();
                --a_stackIndex;
            }
            return stack;
        }

        bool StackHasOMod(
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_omod)
        {
            auto* stack = GetStack(a_item, a_stackIndex);
            auto* instanceExtra = stack && stack->extra ? stack->extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
            return instanceExtra && a_omod && instanceExtra->HasMod(*a_omod);
        }

        bool MutateStackOMod(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_omod,
            bool a_attach)
        {
            auto* inventory = a_owner ? a_owner->inventoryList : nullptr;
            if (!inventory || !a_item || !a_omod)
                return false;

            RE::BSAutoWriteLock lock{ inventory->rwLock };
            auto* stack = GetStack(a_item, a_stackIndex);
            if (!stack)
                return false;

            if (!stack->extra)
            {
                if (!a_attach)
                    return true;
                stack->extra.reset(new RE::ExtraDataList());
            }

            auto* instanceExtra = stack->extra->GetByType<RE::BGSObjectInstanceExtra>();
            if (!instanceExtra)
            {
                if (!a_attach)
                    return true;
                instanceExtra = new RE::BGSObjectInstanceExtra();
                stack->extra->AddExtra(instanceExtra);
            }

            if (a_attach)
                instanceExtra->AddMod(*a_omod, 0, 1, true);
            else
                instanceExtra->RemoveMod(a_omod, 0);

            if (instanceExtra->HasMod(*a_omod) != a_attach)
                return false;

            stack->extra->RemoveExtra<RE::ExtraInstanceData>();
            return stack->extra->CreateInstanceData(a_item->object, true) != nullptr;
        }

        bool CanRefreshEquippedItem(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::TBO_InstanceData* a_previousInstanceData)
        {
            auto* actor = a_owner ? a_owner->As<RE::Actor>() : nullptr;
            auto* middleHigh = actor && actor->currentProcess ? actor->currentProcess->middleHigh : nullptr;
            if (!actor || !middleHigh || !a_item || !a_item->object ||
                !a_item->object->As<RE::TESObjectWEAP>() || !a_previousInstanceData || !GetStack(a_item, a_stackIndex))
            {
                return false;
            }

            std::uint32_t matches = 0;
            RE::BSAutoLock lock{ middleHigh->equippedItemsLock };
            for (const auto& equippedItem : middleHigh->equippedItems)
            {
                if (equippedItem.item.object == a_item->object &&
                    equippedItem.item.instanceData.get() == a_previousInstanceData)
                {
                    ++matches;
                }
            }
            return matches == 1;
        }

        bool EnsureItemOModState(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_omod,
            bool a_attached)
        {
            if (StackHasOMod(a_item, a_stackIndex, a_omod) != a_attached)
            {
                const bool writerSucceeded = MutateStackOMod(a_owner, a_item, a_stackIndex, a_omod, a_attached);
                const bool stateMatches = StackHasOMod(a_item, a_stackIndex, a_omod) == a_attached;
                return writerSucceeded && stateMatches;
            }
            return true;
        }

        bool HasSameAttachPoint(
            RE::BGSMod::Attachment::Mod* a_fromOMod,
            RE::BGSMod::Attachment::Mod* a_toOMod)
        {
            return a_fromOMod && a_toOMod &&
                   a_fromOMod->attachPoint.keywordIndex == a_toOMod->attachPoint.keywordIndex &&
                   RE::detail::BGSKeywordGetTypedKeywordByIndex(
                       RE::KeywordType::kAttachPoint, a_fromOMod->attachPoint.keywordIndex) != nullptr;
        }

        void AddAttachParent(RE::BGSAttachParentArray& a_parents, RE::BGSKeyword* a_keyword)
        {
            using Value = RE::BGSTypedKeywordValue<RE::KeywordType::kAttachPoint>;
            auto& memory = RE::MemoryManager::GetSingleton();
            auto* values = static_cast<Value*>(memory.Allocate(sizeof(Value) * (a_parents.size + 1), 0, false));
            for (std::uint32_t i = 0; i < a_parents.size; ++i)
                values[i] = a_parents.array[i];
            values[a_parents.size].keywordIndex = RE::BGSKeyword::GetIndexForTypedKeyword(a_keyword, RE::KeywordType::kAttachPoint);
            if (a_parents.array)
                memory.Deallocate(a_parents.array, false);
            a_parents.array = values;
            ++a_parents.size;
        }

        [[nodiscard]] std::vector<RE::BGSMod::Attachment::Mod*> Candidates(const WeaponConfig::OModSwitchGroup& a_group)
        {
            std::vector<RE::BGSMod::Attachment::Mod*> result;
            const auto attachIndex = a_group.AttachPoint ?
                RE::BGSKeyword::GetIndexForTypedKeyword(a_group.AttachPoint, RE::KeywordType::kAttachPoint) : 0;
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler || !a_group.AttachPoint)
                return result;

            if (a_group.Filter == WeaponConfig::OModFilter::Include)
            {
                for (const auto* mod : a_group.OMods)
                    if (mod)
                        result.push_back(const_cast<RE::BGSMod::Attachment::Mod*>(mod));
            }
            else
            {
                for (auto* mod : dataHandler->GetFormArray<RE::BGSMod::Attachment::Mod>())
                {
                    if (!mod || mod->attachPoint.keywordIndex != attachIndex ||
                        mod->targetFormType != RE::ENUM_FORM_ID::kWEAP)
                        continue;
                    // Parent and instantiation filters encode extra compatibility constraints.
                    // Automatic discovery fails closed for those records; include mode can opt in explicitly.
                    if (a_group.Filter != WeaponConfig::OModFilter::Include &&
                        (mod->attachParents.size != 0 || mod->filterKeywords.size != 0))
                        continue;
                    if (a_group.Filter == WeaponConfig::OModFilter::Exclude &&
                        std::ranges::find(a_group.OMods, mod) != a_group.OMods.end())
                        continue;
                    result.push_back(mod);
                }
            }

            std::ranges::sort(result, {}, &RE::TESForm::GetFormID);
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        void CycleOnGameThread(WeaponConfig::OModCategory a_category)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto weapon = Core::GetEquippedWeapon(player);
            if (!weapon)
            {
                REX::INFO("OMod switch: no drawn weapon");
                return;
            }

            const auto groups = WeaponConfig::MatchOModSwitchGroups(weapon->Base, weapon->Instance);
            const WeaponConfig::OModSwitchGroup* selected = nullptr;
            for (const auto& group : groups)
            {
                if (group.Category != a_category)
                    continue;
                if (selected)
                {
                    REX::WARN("OMod switch: more than one configured group uses the requested category");
                    return;
                }
                selected = &group;
            }
            if (!selected)
            {
                REX::INFO("OMod switch: weapon {:08X} has no group for the requested category", weapon->Base->GetFormID());
                return;
            }

            if (selected->AttachPoint && !weapon->Base->attachParents.HasKeyword(selected->AttachPoint))
                AddAttachParent(weapon->Base->attachParents, selected->AttachPoint);

            auto candidates = Candidates(*selected);
            if (candidates.empty())
            {
                REX::WARN("OMod switch: group '{}' has no compatible OMOD candidates", selected->Id);
                return;
            }

            auto* item = FindInventoryItem(player, weapon->Base);
            const auto stackID = GetEquippedStackIndex(std::monostate{}, player, weapon->Base);
            if (!item || stackID < 0)
            {
                REX::WARN("OMod switch: equipped weapon stack could not be resolved");
                return;
            }
            const auto stackIndex = static_cast<std::uint32_t>(stackID);
            auto* stack = GetStack(item, stackIndex);
            if (!stack || !stack->extra || stack->count > 1)
            {
                REX::WARN("OMod switch: equipped weapon stack is missing instance data or shares {} copies", stack ? stack->count : 0);
                return;
            }

            RE::BGSMod::Attachment::Mod* current = nullptr;
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            const auto attachIndex = RE::BGSKeyword::GetIndexForTypedKeyword(selected->AttachPoint, RE::KeywordType::kAttachPoint);
            for (auto* mod : dataHandler->GetFormArray<RE::BGSMod::Attachment::Mod>())
            {
                if (mod && mod->attachPoint.keywordIndex == attachIndex &&
                    StackHasOMod(item, stackIndex, mod))
                {
                    if (current)
                    {
                        REX::WARN("OMod switch: multiple installed OMODs use attach point {:04X}; refusing to guess", attachIndex);
                        return;
                    }
                    current = mod;
                }
            }

            auto* next = candidates.front();
            if (current)
            {
                const auto it = std::ranges::find(candidates, current);
                if (it != candidates.end())
                {
                    auto nextIt = std::next(it);
                    next = nextIt == candidates.end() ? candidates.front() : *nextIt;
                }
            }
            if (current == next)
            {
                REX::INFO("OMod switch: group '{}' has only one active candidate", selected->Id);
                return;
            }
            if (current && ReplaceItemOMod(std::monostate{}, player, weapon->Base, stackID, current, next))
                REX::INFO("OMod switch: group '{}' changed {:08X} -> {:08X}", selected->Id, current->GetFormID(), next->GetFormID());
            else if (!current && SetItemOMod(std::monostate{}, player, weapon->Base, stackID, next, true))
                REX::INFO("OMod switch: group '{}' attached {:08X}", selected->Id, next->GetFormID());
            else
                REX::WARN("OMod switch: group '{}' could not apply OMOD {:08X}", selected->Id, next->GetFormID());
        }

        void RefreshBipedWeapon(
            RE::BipedAnim* a_biped,
            const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& a_weapon,
            RE::BGSEquipIndex a_equipIndex)
        {
            using func_t = void (*)(
                RE::BipedAnim*,
                const RE::BGSObjectInstanceT<RE::TESObjectWEAP>&,
                RE::BGSEquipIndex);
            static REL::Relocation<func_t> attachBipedWeapon{ REL::ID{ 788361, 2194353 } };
            attachBipedWeapon(a_biped, a_weapon, a_equipIndex);
        }

        bool RefreshEquippedItem(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::TBO_InstanceData* a_previousInstanceData)
        {
            static REL::Relocation<std::uintptr_t> weaponDataVtable{ RE::VTABLE::EquippedWeaponData[0] };
            auto* actor = a_owner ? a_owner->As<RE::Actor>() : nullptr;
            auto* instanceData = a_item ? a_item->GetInstanceData(a_stackIndex) : nullptr;
            auto* middleHigh = actor && actor->currentProcess ? actor->currentProcess->middleHigh : nullptr;
            auto* weapon = a_item && a_item->object ? a_item->object->As<RE::TESObjectWEAP>() : nullptr;
            if (!actor || !middleHigh || !weapon || !instanceData)
            {
                REX::WARN(
                    "Equipped OMOD refresh prerequisites missing: actor={}, middleHigh={}, instanceData={}",
                    actor != nullptr,
                    middleHigh != nullptr,
                    instanceData != nullptr);
                return false;
            }

            std::uint32_t objectMatches = 0;
            std::uint32_t instanceMatches = 0;
            RE::BGSEquipIndex equipIndex{};
            RE::EquippedItem* matchedEquippedItem = nullptr;
            RE::NiPointer<RE::EquippedItemData> equippedItemData;
            RE::EquippedWeaponData* equippedWeaponData = nullptr;
            {
                RE::BSAutoLock lock{ middleHigh->equippedItemsLock };
                for (auto& equippedItem : middleHigh->equippedItems)
                {
                    if (equippedItem.item.object != a_item->object)
                    {
                        continue;
                    }
                    ++objectMatches;
                    if (a_previousInstanceData && equippedItem.item.instanceData.get() == a_previousInstanceData)
                    {
                        ++instanceMatches;
                        matchedEquippedItem = &equippedItem;
                    }
                }

                if (instanceMatches == 1)
                {
                    matchedEquippedItem->item.instanceData.reset(instanceData);
                    equipIndex = matchedEquippedItem->equipIndex;
                    equippedItemData = matchedEquippedItem->data;
                    if (equippedItemData &&
                        *reinterpret_cast<const std::uintptr_t*>(equippedItemData.get()) == weaponDataVtable.address())
                    {
                        equippedWeaponData = static_cast<RE::EquippedWeaponData*>(equippedItemData.get());
                    }
                }
            }

            if (instanceMatches != 1)
            {
                REX::WARN(
                    "Equipped OMOD refresh could not uniquely match item {:08X} (objectMatches={}, instanceMatches={})",
                    a_item->object->GetFormID(),
                    objectMatches,
                    instanceMatches);
                return false;
            }

            bool refreshedThirdPerson = false;
            bool refreshedFirstPerson = false;
            bool refreshedFireSounds = false;
            {
                RE::BGSObjectInstanceT<RE::TESObjectWEAP> weaponInstance{ weapon, instanceData };
                if (equippedWeaponData)
                {
                    equippedWeaponData->SetupFireSounds(*actor, weaponInstance);
                    refreshedFireSounds = true;
                }
                if (actor->biped)
                {
                    RefreshBipedWeapon(actor->biped.get(), weaponInstance, equipIndex);
                    refreshedThirdPerson = true;
                }
                const auto& firstPersonBiped = actor->GetBiped(true);
                if (firstPersonBiped)
                {
                    RefreshBipedWeapon(firstPersonBiped.get(), weaponInstance, equipIndex);
                    refreshedFirstPerson = true;
                }
            }

            REX::INFO(
                "Equipped OMOD data refreshed for item {:08X}, equipIndex={}, thirdPerson={}, firstPerson={}, fireSounds={}",
                a_item->object->GetFormID(),
                equipIndex.index,
                refreshedThirdPerson,
                refreshedFirstPerson,
                refreshedFireSounds);
            return true;
        }

        bool ApplyItemOMod(
            RE::TESObjectREFR* a_owner,
            RE::TESBoundObject* a_itemForm,
            std::int32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_omod,
            bool a_attach)
        {
            if (!a_owner || !a_owner->inventoryList || !a_itemForm || !a_omod || a_stackIndex < 0)
            {
                return false;
            }

            auto* item = FindInventoryItem(a_owner, a_itemForm);
            if (!item)
            {
                REX::WARN(
                    "OMod mutation: item {:08X} absent from owner {:08X} inventory",
                    a_itemForm->GetFormID(),
                    a_owner->GetFormID());
                return false;
            }

            const auto stackIndex = static_cast<std::uint32_t>(a_stackIndex);
            auto* stack = GetStack(item, stackIndex);
            if (!stack)
            {
                REX::WARN("OMod mutation: item {:08X} has no stack {}", a_itemForm->GetFormID(), stackIndex);
                return false;
            }

            const bool hadOMod = StackHasOMod(item, stackIndex, a_omod);
            if (hadOMod == a_attach)
            {
                return true;
            }

            const bool wasEquipped = stack->IsEquipped();
            auto* previousInstanceData = wasEquipped ? item->GetInstanceData(stackIndex) : nullptr;
            if (wasEquipped && !CanRefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
            {
                REX::WARN(
                    "OMod mutation rejected before write: equipped stack identity cannot be resolved for item {:08X}, stack={}",
                    a_itemForm->GetFormID(),
                    stackIndex);
                return false;
            }

            if (!MutateStackOMod(a_owner, item, stackIndex, a_omod, a_attach))
            {
                const bool restored = EnsureItemOModState(a_owner, item, stackIndex, a_omod, hadOMod);
                const bool refreshed = !wasEquipped ||
                                       RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData);
                REX::ERROR(
                    "OMod mutation failed for item {:08X}, OMOD={:08X}; original state restored={}, equipped refresh={}, final OMOD present={}",
                    a_itemForm->GetFormID(),
                    a_omod->GetFormID(),
                    restored,
                    refreshed,
                    StackHasOMod(item, stackIndex, a_omod));
                return false;
            }

            if (!wasEquipped)
            {
                REX::INFO(
                    "OMod changed on unequipped item {:08X}, OMOD {:08X}, attach={}, stack={}",
                    a_itemForm->GetFormID(),
                    a_omod->GetFormID(),
                    a_attach,
                    stackIndex);
                return true;
            }

            if (RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
            {
                return true;
            }

            const bool restored = EnsureItemOModState(a_owner, item, stackIndex, a_omod, hadOMod);
            const bool refreshed = RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData);
            REX::ERROR(
                "OMod mutation refresh failed for item {:08X}, OMOD={:08X}; original state restored={}, equipped refresh={}, final OMOD present={}",
                a_itemForm->GetFormID(),
                a_omod->GetFormID(),
                restored,
                refreshed,
                StackHasOMod(item, stackIndex, a_omod));
            return false;
        }
    }

    void Initialize()
    {
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
            return;

        std::uint32_t added = 0;
        for (auto* weapon : dataHandler->GetFormArray<RE::TESObjectWEAP>())
        {
            if (!weapon)
                continue;
            for (const auto& group : WeaponConfig::MatchOModSwitchGroups(weapon, nullptr))
            {
                auto* attachPoint = group.AttachPoint;
                if (attachPoint && !weapon->attachParents.HasKeyword(attachPoint))
                {
                    AddAttachParent(weapon->attachParents, attachPoint);
                    ++added;
                }
            }
        }
        REX::INFO("OMod switch: injected {} configured attach-point(s)", added);
    }

    bool CycleEquippedOMod(WeaponConfig::OModCategory a_category)
    {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks)
            return false;
        tasks->AddUITask([a_category]() { CycleOnGameThread(a_category); });
        return true;
    }

    bool SetItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_item,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_omod,
        bool a_attach)
    {
        return ApplyItemOMod(a_owner, a_item, a_stackIndex, a_omod, a_attach);
    }

    bool HasItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_itemForm,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_omod)
    {
        if (!a_owner || !a_owner->inventoryList || !a_itemForm || !a_omod || a_stackIndex < 0)
        {
            return false;
        }
        auto* item = FindInventoryItem(a_owner, a_itemForm);
        return item && StackHasOMod(item, static_cast<std::uint32_t>(a_stackIndex), a_omod);
    }

    std::int32_t GetEquippedStackIndex(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_itemForm)
    {
        auto* item = FindInventoryItem(a_owner, a_itemForm);
        if (!item)
        {
            return -1;
        }

        std::int32_t equippedIndex = -1;
        std::uint32_t stackIndex = 0;
        for (auto* stack = item->stackData.get(); stack; stack = stack->nextStack.get(), ++stackIndex)
        {
            if (!stack->IsEquipped())
            {
                continue;
            }
            if (equippedIndex >= 0 || stackIndex > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            {
                return -1;
            }
            equippedIndex = static_cast<std::int32_t>(stackIndex);
        }
        return equippedIndex;
    }

    bool ReplaceItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_itemForm,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_fromOMod,
        RE::BGSMod::Attachment::Mod* a_toOMod)
    {
        if (!a_owner || !a_owner->inventoryList || !a_itemForm || !a_fromOMod || !a_toOMod ||
            a_fromOMod == a_toOMod || a_stackIndex < 0)
        {
            return false;
        }

        auto* item = FindInventoryItem(a_owner, a_itemForm);
        if (!item)
        {
            REX::WARN("OMod replacement: item {:08X} absent from owner {:08X} inventory", a_itemForm->GetFormID(), a_owner->GetFormID());
            return false;
        }

        const auto stackIndex = static_cast<std::uint32_t>(a_stackIndex);
        auto* stack = GetStack(item, stackIndex);
        if (!stack || !StackHasOMod(item, stackIndex, a_fromOMod) || StackHasOMod(item, stackIndex, a_toOMod))
        {
            REX::WARN(
                "OMod replacement precondition failed: item={:08X}, stack={}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
                a_itemForm->GetFormID(),
                stackIndex,
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID(),
                StackHasOMod(item, stackIndex, a_fromOMod),
                StackHasOMod(item, stackIndex, a_toOMod));
            return false;
        }

        if (!HasSameAttachPoint(a_fromOMod, a_toOMod))
        {
            REX::WARN(
                "OMod replacement rejected: source {:08X} and destination {:08X} do not share a valid attach point",
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID());
            return false;
        }

        const bool wasEquipped = stack->IsEquipped();
        auto* previousInstanceData = wasEquipped ? item->GetInstanceData(stackIndex) : nullptr;
        if (wasEquipped && !CanRefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
        {
            REX::WARN(
                "OMod replacement rejected before write: equipped stack identity cannot be resolved for item {:08X}, stack={}",
                a_itemForm->GetFormID(),
                stackIndex);
            return false;
        }

        struct RecoveryResult
        {
            bool destinationRemoved;
            bool sourceRestored;
            bool equippedRefreshed;

            [[nodiscard]] bool Complete() const noexcept
            {
                return destinationRemoved && sourceRestored && equippedRefreshed;
            }
        };
        const auto rollbackReplacement = [&]()
        {
            const bool destinationRemoved = EnsureItemOModState(a_owner, item, stackIndex, a_toOMod, false);
            const bool sourceRestored = EnsureItemOModState(a_owner, item, stackIndex, a_fromOMod, true);
            const bool equippedRefreshed = !wasEquipped ||
                                           RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData);
            return RecoveryResult{ destinationRemoved, sourceRestored, equippedRefreshed };
        };

        // Defer all equipped/runtime model refreshes until both stack mutations finish.
        if (!MutateStackOMod(a_owner, item, stackIndex, a_fromOMod, false))
        {
            const auto recovery = rollbackReplacement();
            REX::ERROR(
                "OMod source removal failed; recovery complete={}, destinationRemoved={}, sourceRestored={}, equippedRefreshed={}, item={:08X}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
                recovery.Complete(),
                recovery.destinationRemoved,
                recovery.sourceRestored,
                recovery.equippedRefreshed,
                a_itemForm->GetFormID(),
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID(),
                StackHasOMod(item, stackIndex, a_fromOMod),
                StackHasOMod(item, stackIndex, a_toOMod));
            return false;
        }
        if (!MutateStackOMod(a_owner, item, stackIndex, a_toOMod, true))
        {
            const auto recovery = rollbackReplacement();
            REX::ERROR(
                "OMod replacement failed; recovery complete={}, destinationRemoved={}, sourceRestored={}, equippedRefreshed={}, item={:08X}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
                recovery.Complete(),
                recovery.destinationRemoved,
                recovery.sourceRestored,
                recovery.equippedRefreshed,
                a_itemForm->GetFormID(),
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID(),
                StackHasOMod(item, stackIndex, a_fromOMod),
                StackHasOMod(item, stackIndex, a_toOMod));
            return false;
        }

        if (StackHasOMod(item, stackIndex, a_fromOMod) || !StackHasOMod(item, stackIndex, a_toOMod))
        {
            const auto recovery = rollbackReplacement();
            REX::ERROR(
                "OMod replacement postcondition failed; recovery complete={}, destinationRemoved={}, sourceRestored={}, equippedRefreshed={}, item={:08X}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
                recovery.Complete(),
                recovery.destinationRemoved,
                recovery.sourceRestored,
                recovery.equippedRefreshed,
                a_itemForm->GetFormID(),
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID(),
                StackHasOMod(item, stackIndex, a_fromOMod),
                StackHasOMod(item, stackIndex, a_toOMod));
            return false;
        }

        if (wasEquipped && !RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
        {
            const auto recovery = rollbackReplacement();
            REX::ERROR(
                "OMod replacement committed but equipped refresh failed; recovery complete={}, destinationRemoved={}, sourceRestored={}, equippedRefreshed={}, item={:08X}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
                recovery.Complete(),
                recovery.destinationRemoved,
                recovery.sourceRestored,
                recovery.equippedRefreshed,
                a_itemForm->GetFormID(),
                a_fromOMod->GetFormID(),
                a_toOMod->GetFormID(),
                StackHasOMod(item, stackIndex, a_fromOMod),
                StackHasOMod(item, stackIndex, a_toOMod));
            return false;
        }

        REX::INFO(
            "OMod replaced for item {:08X}, source={:08X}, destination={:08X}, stack={}, equipped={}",
            a_itemForm->GetFormID(),
            a_fromOMod->GetFormID(),
            a_toOMod->GetFormID(),
            stackIndex,
            wasEquipped);
        return true;
    }

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm)
        {
            return false;
        }

        a_vm->BindNativeMethod("MWBOModSwitch", "SetItemOMod", SetItemOMod);
        a_vm->BindNativeMethod("MWBOModSwitch", "HasItemOMod", HasItemOMod);
        a_vm->BindNativeMethod("MWBOModSwitch", "GetEquippedStackIndex", GetEquippedStackIndex);
        a_vm->BindNativeMethod("MWBOModSwitch", "ReplaceItemOMod", ReplaceItemOMod);
        return true;
    }
}
