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
#include "Core/Core.h"
#include "Core/EquippedWeapon.h"
#include "Core/GunState.h"
#include "Reload/ManualReload.h"
#include "Reload/SightedReload.h"
#include "Settings/Settings.h"

#include <F4SE/API.h>
#include <RE/A/ActorEquipManager.h>

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
#include <RE/P/PipboyDataManager.h>
#include <RE/P/PipboyArray.h>
#include <RE/P/PipboyPrimitiveValue.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/PlayerControls.h>
#include <RE/U/UI.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESObjectWEAP.h>

#include <limits>

namespace OModSwitch
{
    namespace
    {
        struct NativeOModRuntime
        {
            REL::Version version;
            std::uintptr_t writerRva;
            std::uintptr_t writerHandleReadOffset;
            std::uintptr_t equipRva;
            std::uintptr_t graphCallRva;
            std::array<std::uint8_t, 11> graphCallBytes;
        };

        constexpr std::array NativeOModRuntimes{
            NativeOModRuntime{ F4SE::RUNTIME_1_10_163, 0x1A84B0, 0x90, 0xE1BCD0, 0xE1F030,
                { 0x48, 0x8B, 0x4D, 0x6F, 0xB2, 0x01, 0xE8, 0xEB, 0xFA, 0xF5, 0xFF } },
            NativeOModRuntime{ F4SE::RUNTIME_1_11_240, 0x34C400, 0x8B, 0xCE5E20, 0xCE85AB,
                { 0x48, 0x8B, 0x4D, 0x28, 0xB2, 0x01, 0xE8, 0x20, 0x01, 0xF8, 0xFF } },
        };

        using NativeStackWriter = bool(RE::BGSInventoryItem*, RE::BGSInventoryItem::StackDataCompareFunctor&,
            RE::BGSInventoryItem::StackDataWriteFunctor&, bool, std::uint32_t*);
        bool nativeOModRefreshAvailable = false;
        REL::Relocation<NativeStackWriter> nativeStackWriter;
        REL::Relocation<void(RE::PipboyInventoryData*, RE::ENUM_FORM_ID)> queueAmmoCardRefresh;
        thread_local RE::Actor* skipEquipGraphFor = nullptr;
        REL::Relocation<void(RE::Actor*, bool)> originalEquipGraph;
        std::atomic<std::uint64_t> switchGeneration{ 0 };

        using ReloadClock = std::chrono::steady_clock;
        constexpr auto ReloadRetryInterval = std::chrono::milliseconds{ 150 };
        constexpr auto ReloadRetryWindow = std::chrono::seconds{ 2 };

        struct PendingAmmoReload
        {
            std::uint64_t generation;
            RE::TESObjectWEAP* weapon;
            RE::BSTSmartPointer<RE::TBO_InstanceData> instance;
            RE::TESAmmo* ammo;
            ReloadClock::time_point expires;
            ReloadClock::time_point nextAttempt;
            bool queued = false;
        };

        std::mutex pendingReloadLock;
        std::optional<PendingAmmoReload> pendingReload;
        std::uint64_t reloadGeneration = 0;

        void UpdateEquipGraph(RE::Actor* a_actor, bool a_updateWeaponState)
        {
            if (a_actor == skipEquipGraphFor && a_updateWeaponState)
            {
                skipEquipGraphFor = nullptr;
                return;
            }
            originalEquipGraph(a_actor, a_updateWeaponState);
        }

        void InitializeNativeOModRuntime()
        {
            const auto module = REX::FModule::GetExecutingModule();
            const auto version = module.GetFileVersion();
            const auto runtime = std::ranges::find(NativeOModRuntimes, version, &NativeOModRuntime::version);
            nativeOModRefreshAvailable = runtime != NativeOModRuntimes.end();
            if (!nativeOModRefreshAvailable)
                return;

            const auto base = module.GetBaseAddress();
            const auto site = base + runtime->graphCallRva;
            const auto writer = base + runtime->writerRva;
            const auto handleRead = writer + runtime->writerHandleReadOffset;
            // mov rdx,[rsp+60h]; mov eax,[rip+disp32]; cmp [rdx],eax
            constexpr std::array<std::uint8_t, 7> handleLoad{ 0x48, 0x8B, 0x54, 0x24, 0x60, 0x8B, 0x05 };
            constexpr std::array<std::uint8_t, 2> handleCompare{ 0x39, 0x02 };
            if (std::memcmp(reinterpret_cast<const void*>(site - 6), runtime->graphCallBytes.data(), runtime->graphCallBytes.size()) != 0 ||
                REL::Relocation<std::uintptr_t>{ RE::ID::BGSInventoryItem::FindAndWriteStackData }.address() != writer ||
                REL::Relocation<std::uintptr_t>{ RE::ID::ActorEquipManager::EquipObject }.offset() != runtime->equipRva ||
                std::memcmp(reinterpret_cast<const void*>(handleRead), handleLoad.data(), handleLoad.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(handleRead + 11), handleCompare.data(), handleCompare.size()) != 0)
            {
                REX::WARN("OMod switch: native addresses did not match at equip graph RVA {:X}; native equipped refresh unavailable",
                    runtime->graphCallRva);
                return;
            }
            originalEquipGraph = REL::GetTrampoline().write_call<5>(site, UpdateEquipGraph);
            nativeStackWriter = writer;
            REX::INFO("OMod switch: native stack writer RVA {:X}, scoped equip graph hook RVA {:X} enabled",
                runtime->writerRva, runtime->graphCallRva);

            if (version == F4SE::RUNTIME_1_10_163)
            {
                queueAmmoCardRefresh = RE::ID::PipboyInventoryData::QueueItemCardRepopulate.address();
            }
            else
            {
                // AE 1.11.240 ID 2225311 is the underlying set insertion helper.
                // ID 2225281 is the wrapper that takes a form type by value.
                constexpr REL::ID cardRefreshID{ 2225281 };
                constexpr std::array<std::uint8_t, 40> wrapper{
                    0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x81, 0xC1, 0x70, 0x01, 0x00, 0x00,
                    0x4C, 0x8D, 0x4C, 0x24, 0x30, 0x4C, 0x8D, 0x44, 0x24, 0x38, 0x48, 0x8D, 0x54, 0x24, 0x38,
                    0xE8, 0x4D, 0x23, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3
                };
                const auto cardRefresh = base + 0xB46490;
                if (cardRefreshID.address() == cardRefresh &&
                    std::memcmp(reinterpret_cast<const void*>(cardRefresh), wrapper.data(), wrapper.size()) == 0)
                {
                    queueAmmoCardRefresh = cardRefresh;
                    REX::INFO("Ammo switch: verified AE Pip-Boy card refresh wrapper at RVA B46490");
                }
                else
                {
                    REX::WARN("Ammo switch: AE Pip-Boy card refresh wrapper did not match; explicit card refresh disabled");
                }
            }
        }

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

        RE::TBO_InstanceData* RebuildStackInstanceData(
            RE::BGSInventoryItem::Stack* a_stack,
            RE::TESBoundObject* a_object)
        {
            if (!a_stack || !a_stack->extra || !a_object)
                return nullptr;

            a_stack->extra->RemoveExtra<RE::ExtraInstanceData>();
            auto* instanceData = a_stack->extra->CreateInstanceData(a_object, true);
            if (!instanceData)
                return nullptr;

            auto* extraInstanceData = a_stack->extra->GetByType<RE::ExtraInstanceData>();
            if (!extraInstanceData)
            {
                RE::BSTSmartPointer<RE::TBO_InstanceData> data{ instanceData };
                a_stack->extra->AddExtra(new RE::ExtraInstanceData(a_object, std::move(data)));
            }
            else if (extraInstanceData->data.get() != instanceData)
            {
                extraInstanceData->base = a_object;
                extraInstanceData->data.reset(instanceData);
            }
            return instanceData;
        }

        std::span<RE::BGSMod::ObjectIndexData> StackOModEntries(RE::BGSInventoryItem::Stack* a_stack)
        {
            auto* mods = a_stack && a_stack->extra ? a_stack->extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
            return mods && mods->values ? mods->GetIndexData() : std::span<RE::BGSMod::ObjectIndexData>{};
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
            bool a_attach,
            RE::TBO_InstanceData** a_newInstanceData = nullptr)
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

            auto* instanceData = RebuildStackInstanceData(stack, a_item->object);
            if (a_newInstanceData)
                *a_newInstanceData = instanceData;
            return instanceData != nullptr;
        }

        bool ReplaceStackOMod(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_fromOMod,
            RE::BGSMod::Attachment::Mod* a_toOMod,
            RE::TBO_InstanceData** a_newInstanceData = nullptr)
        {
            auto* inventory = a_owner ? a_owner->inventoryList : nullptr;
            if (!inventory || !a_item || !a_fromOMod || !a_toOMod)
                return false;

            RE::BSAutoWriteLock lock{ inventory->rwLock };
            auto* stack = GetStack(a_item, a_stackIndex);
            auto* instanceExtra = stack && stack->extra ? stack->extra->GetByType<RE::BGSObjectInstanceExtra>() : nullptr;
            if (!stack || !instanceExtra || !instanceExtra->HasMod(*a_fromOMod) || instanceExtra->HasMod(*a_toOMod))
                return false;

            // Replace the same-slot OMODs before rebuilding instance data. Rebuilding
            // after each half leaves an invalid equipped intermediate instance.
            instanceExtra->RemoveMod(a_fromOMod, 0);
            instanceExtra->AddMod(*a_toOMod, 0, 1, true);
            if (instanceExtra->HasMod(*a_fromOMod) || !instanceExtra->HasMod(*a_toOMod))
                return false;

            auto* instanceData = RebuildStackInstanceData(stack, a_item->object);
            if (a_newInstanceData)
                *a_newInstanceData = instanceData;
            return instanceData != nullptr;
        }

        RE::TBO_InstanceData* GetEquippedInstanceData(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item)
        {
            auto* actor = a_owner ? a_owner->As<RE::Actor>() : nullptr;
            auto* middleHigh = actor && actor->currentProcess ? actor->currentProcess->middleHigh : nullptr;
            if (!middleHigh || !a_item || !a_item->object)
                return nullptr;

            RE::TBO_InstanceData* instanceData = nullptr;
            std::uint32_t matches = 0;
            RE::BSAutoLock lock{ middleHigh->equippedItemsLock };
            for (const auto& equippedItem : middleHigh->equippedItems)
            {
                if (equippedItem.item.object != a_item->object || !equippedItem.item.instanceData)
                    continue;
                instanceData = equippedItem.item.instanceData.get();
                ++matches;
            }
            return matches == 1 ? instanceData : nullptr;
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

        bool StoreStackInstanceData(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::TBO_InstanceData* a_instanceData)
        {
            auto* inventory = a_owner ? a_owner->inventoryList : nullptr;
            if (!inventory || !a_item || !a_item->object || !a_instanceData)
                return false;

            RE::BSAutoWriteLock lock{ inventory->rwLock };
            auto* stack = GetStack(a_item, a_stackIndex);
            if (!stack)
                return false;
            if (!stack->extra)
                stack->extra.reset(new RE::ExtraDataList());

            auto* extraInstanceData = stack->extra->GetByType<RE::ExtraInstanceData>();
            if (!extraInstanceData)
            {
                RE::BSTSmartPointer<RE::TBO_InstanceData> data{ a_instanceData };
                stack->extra->AddExtra(new RE::ExtraInstanceData(a_item->object, std::move(data)));
            }
            else
            {
                extraInstanceData->base = a_item->object;
                if (extraInstanceData->data.get() != a_instanceData)
                    extraInstanceData->data.reset(a_instanceData);
            }

            extraInstanceData = stack->extra->GetByType<RE::ExtraInstanceData>();
            const bool baseMatches = extraInstanceData && extraInstanceData->base == a_item->object;
            const bool dataMatches = extraInstanceData && extraInstanceData->data.get() == a_instanceData;
            REX::INFO(
                "OMod stack instance write: baseMatches={}, dataMatches={}, wrapper={:p}, expected={:p}, resolved={:p}",
                baseMatches,
                dataMatches,
                static_cast<void*>(extraInstanceData ? extraInstanceData->data.get() : nullptr),
                static_cast<void*>(a_instanceData),
                static_cast<void*>(a_item->GetInstanceData(a_stackIndex)));
            return baseMatches && dataMatches;
        }

        bool EnsureItemOModState(
            RE::TESObjectREFR* a_owner,
            RE::BGSInventoryItem* a_item,
            std::uint32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_omod,
            bool a_attached,
            RE::TBO_InstanceData** a_newInstanceData = nullptr)
        {
            if (StackHasOMod(a_item, a_stackIndex, a_omod) != a_attached)
            {
                const bool writerSucceeded = MutateStackOMod(
                    a_owner, a_item, a_stackIndex, a_omod, a_attached, a_newInstanceData);
                const bool stateMatches = StackHasOMod(a_item, a_stackIndex, a_omod) == a_attached;
                return writerSucceeded && stateMatches;
            }
            if (a_newInstanceData)
                *a_newInstanceData = nullptr;
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

        void QueueWeaponCardRefresh()
        {
            if (!queueAmmoCardRefresh.address())
                return;
            if (auto* pipboyData = RE::PipboyDataManager::GetSingleton())
            {
                queueAmmoCardRefresh(&pipboyData->inventoryData, RE::ENUM_FORM_ID::kWEAP);
                REX::INFO("OMod switch: queued Pip-Boy weapon card refresh");
            }
        }

        bool UpdatePipboyAmmoEquipState(const RE::TESAmmo* a_equippedAmmo)
        {
            auto* pipboyData = RE::PipboyDataManager::GetSingleton();
            auto* inventoryObject = pipboyData ? pipboyData->inventoryData.inventoryObject : nullptr;
            auto* ammoArray = inventoryObject ? inventoryObject->GetMember<RE::PipboyArray*>("44") : nullptr;
            if (!ammoArray)
            {
                REX::WARN("Ammo switch: Pip-Boy ammo list is unavailable for immediate equip marker update");
                return false;
            }

            std::uint32_t cleared = 0;
            bool targetFound = false;
            const auto equippedFormID = a_equippedAmmo ? a_equippedAmmo->GetFormID() : 0;
            for (auto* value : ammoArray->elements)
            {
                auto* item = value ? static_cast<RE::PipboyObject*>(value) : nullptr;
                if (!item)
                    continue;

                auto* formID = item->GetMember<RE::PipboyPrimitiveValue<std::uint32_t>*>("formID");
                auto* equipState = item->GetMember<RE::PipboyPrimitiveValue<std::uint32_t>*>("equipState");
                if (!formID || !equipState)
                    continue;

                const bool isEquippedAmmo = equippedFormID != 0 && formID->m_value == equippedFormID;
                targetFound = targetFound || isEquippedAmmo;
                if (equipState->m_value != (isEquippedAmmo ? 1u : 0u))
                {
                    equipState->m_value = isEquippedAmmo ? 1u : 0u;
                    equipState->m_isDirtyToCompanion = true;
                    if (!isEquippedAmmo)
                        ++cleared;
                }
            }

            REX::INFO("Ammo switch: synchronized Pip-Boy equip marker (ammo={:08X}, targetFound={}, staleMarkersCleared={})",
                equippedFormID, targetFound, cleared);
            return targetFound;
        }

        void SyncPipboyAmmoMarkerFromEquippedWeapon()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto weapon = Core::GetEquippedWeapon(player);
            const RE::TESAmmo* ammo = weapon && weapon->Data ? weapon->Data->ammo : nullptr;
            if (!ammo && weapon && weapon->Instance)
                ammo = weapon->Instance->ammo;
            if (!ammo && weapon && weapon->Base)
                ammo = weapon->Base->weaponData.ammo;

            const auto* weaponBaseAmmo = weapon && weapon->Base ? weapon->Base->weaponData.ammo : nullptr;
            if (ammo && !WeaponConfig::MatchAmmoSwitch(ammo, weaponBaseAmmo))
                ammo = nullptr;
            UpdatePipboyAmmoEquipState(ammo);
        }

        class PipboyAmmoMarkerWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent& a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event.opening || a_event.menuName != "PipboyMenu"sv)
                    return RE::BSEventNotifyControl::kContinue;

                if (const auto* tasks = F4SE::GetTaskInterface())
                    tasks->AddTask([]() { SyncPipboyAmmoMarkerFromEquippedWeapon(); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool StartAmmoReloadAction()
        {
            auto* controls = RE::PlayerControls::GetSingleton();
            return controls && controls->DoAction(RE::DEFAULT_OBJECT::kActionReload, RE::ActionInput::ACTIONPRIORITY::kTry);
        }

        bool ReloadWeaponImmediately(
            RE::Actor* a_actor,
            const RE::BGSObjectInstance& a_weapon,
            std::uint32_t a_equipIndex)
        {
            constexpr std::size_t ReloadWeaponIndex = 0xEF;
            using ReloadWeapon = bool (*)(RE::Actor*, const RE::BGSObjectInstance&, std::uint32_t);
            const auto vtable = *reinterpret_cast<const ReloadWeapon* const*>(a_actor);
            return vtable[ReloadWeaponIndex](a_actor, a_weapon, a_equipIndex);
        }

        void RetryAmmoReload(std::uint64_t a_generation)
        {
            std::optional<PendingAmmoReload> request;
            {
                const std::scoped_lock lock{ pendingReloadLock };
                if (!pendingReload || pendingReload->generation != a_generation)
                    return;
                request = pendingReload;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto live = Core::GetEquippedWeapon(player);
            const bool stillMatches = live && live->Base == request->weapon && live->Item && live->Data &&
                live->Item->item.instanceData.get() == request->instance.get() &&
                live->Instance->ammo == request->ammo && live->Data->ammo == request->ammo &&
                live->Data->ammoCount == 0;
            const bool expired = ReloadClock::now() >= request->expires;
            bool accepted = false;
            bool hasReserve = true;
            const auto* ui = RE::UI::GetSingleton();
            if (stillMatches && !expired && ui && ui->menuMode == 0)
            {
                const auto gunState = Core::GetGunState(player);
                if (gunState == RE::GUN_STATE::kDrawn || gunState == RE::GUN_STATE::kSighted ||
                    (player->meleeAttackState == 0 && Core::IsThirdPersonReadyStance(player, gunState)))
                {
                    std::uint32_t reserve = 0;
                    player->GetItemCount(reserve, request->ammo, false);
                    hasReserve = reserve > 0;
                    if (hasReserve)
                        accepted = StartAmmoReloadAction();
                }
            }

            const std::scoped_lock lock{ pendingReloadLock };
            if (!pendingReload || pendingReload->generation != a_generation)
                return;
            if (accepted || expired || !stillMatches || !hasReserve)
            {
                pendingReload.reset();
                if (accepted)
                    REX::INFO("Ammo switch: game accepted deferred reload action for {:08X} with ammo {:08X}",
                        request->weapon->GetFormID(), request->ammo->GetFormID());
                else if (expired)
                    REX::WARN("Ammo switch: deferred reload timed out; the new ammo is selected");
                else if (!hasReserve)
                    REX::INFO("Ammo switch: deferred reload cancelled; no target ammo in reserve");
                else
                    REX::INFO("Ammo switch: deferred reload cancelled; equipped weapon or magazine changed");
                return;
            }
            pendingReload->queued = false;
            pendingReload->nextAttempt = ReloadClock::now() + ReloadRetryInterval;
        }

        bool RequestAmmoReload(
            RE::PlayerCharacter* a_player,
            RE::TESObjectWEAP* a_expectedWeapon,
            RE::TESAmmo* a_targetAmmo,
            bool a_playAnimation)
        {
            if (!a_player || !a_expectedWeapon || !a_targetAmmo)
                return false;

            ResetPendingAmmoReload();
            const auto weapon = Core::GetEquippedWeapon(a_player);
            if (!weapon || weapon->Base != a_expectedWeapon || !weapon->Instance)
            {
                REX::WARN("Ammo switch: OMOD changed, but the equipped weapon could not be reacquired for reload");
                return false;
            }

            if (!weapon->Data)
            {
                REX::WARN("Ammo switch: OMOD changed, but the equipped weapon has no ammo state to reload");
                return false;
            }

            std::uint32_t ownedTargetAmmo = 0;
            a_player->GetItemCount(ownedTargetAmmo, a_targetAmmo, false);
            if (!ownedTargetAmmo || !weapon->Instance->ammoCapacity)
            {
                REX::WARN("Ammo switch: cannot reload target ammo {:08X} (reserve={}, capacity={})",
                    a_targetAmmo->GetFormID(), ownedTargetAmmo, weapon->Instance->ammoCapacity);
                return false;
            }

            // Start with an empty magazine for the new ammo, even if the native
            // equip populated an initial count. Do not manually credit the old
            // magazine to inventory: the engine's OMOD/equip refresh already
            // accounts for it, and adding it here duplicates those rounds.
            weapon->Data->ammo = a_targetAmmo;
            weapon->Data->ammoCount = 0;

            if (!a_playAnimation)
            {
                if (!weapon->Item)
                {
                    REX::WARN("Ammo switch: cannot instant-reload Pip-Boy selection; equipped inventory item is unavailable");
                    return false;
                }
                const bool reloaded = ReloadWeaponImmediately(
                    a_player, weapon->Item->item, weapon->Item->equipIndex.index);
                REX::INFO("Ammo switch: Pip-Boy instant reload for {:08X} returned {}",
                    a_expectedWeapon->GetFormID(), reloaded);
                QueueWeaponCardRefresh();
                return reloaded;
            }

            QueueWeaponCardRefresh();

            // ReloadWeapon applies ammunition directly. The player's reload action
            // starts the animation and lets the normal ReloadComplete handler load it.
            const auto equippedInstance = weapon->Item ? weapon->Item->item.instanceData : RE::BSTSmartPointer<RE::TBO_InstanceData>{};
            if (StartAmmoReloadAction())
            {
                REX::INFO("Ammo switch: game accepted reload action for {:08X} with ammo {:08X}",
                    a_expectedWeapon->GetFormID(), a_targetAmmo->GetFormID());
                return true;
            }

            if (equippedInstance && F4SE::GetTaskInterface())
            {
                const auto now = ReloadClock::now();
                const std::scoped_lock lock{ pendingReloadLock };
                pendingReload = PendingAmmoReload{ ++reloadGeneration, a_expectedWeapon, equippedInstance, a_targetAmmo,
                    now + ReloadRetryWindow, now + ReloadRetryInterval };
                REX::INFO("Ammo switch: reload action not accepted yet; queued retry with ammo {:08X}",
                    a_targetAmmo->GetFormID());
                return true;
            }

            REX::WARN("Ammo switch: ammo changed, but the reload action was not accepted for weapon {:08X}",
                a_expectedWeapon->GetFormID());
            return false;
        }

        bool ApplyEquippedOModTransition(
            RE::PlayerCharacter* a_owner,
            RE::TESObjectWEAP* a_itemForm,
            std::int32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_fromOMod,
            RE::BGSMod::Attachment::Mod* a_toOMod,
            RE::TESAmmo* a_targetAmmo = nullptr);

        bool SwitchAmmoOnGameThread(
            RE::PlayerCharacter* a_player,
            const Core::EquippedWeapon& a_weapon,
            const WeaponConfig::AmmoSwitchMatch& a_match,
            std::optional<std::size_t> a_targetType = std::nullopt,
            bool a_pipboyReloadAnimation = true,
            bool a_automaticSwitch = false)
        {
            const auto* profile = a_match.Profile;
            if (!a_player || !profile || !profile->AttachPoint || a_match.CurrentType >= profile->Types.size())
                return false;
            if (nativeOModRefreshAvailable && !nativeStackWriter.address())
                return false;
            if (nativeOModRefreshAvailable && Core::GetGunState(a_player) == RE::GUN_STATE::kReloading)
            {
                REX::INFO("Ammo switch: finish the current reload before switching again");
                return false;
            }

            if (!a_weapon.Base->attachParents.HasKeyword(profile->AttachPoint) &&
                !Core::HasDefaultTemplateAttachPoint(a_weapon.Base, profile->AttachPoint))
                AddAttachParent(a_weapon.Base->attachParents, profile->AttachPoint);

            auto* item = FindInventoryItem(a_player, a_weapon.Base);
            const auto stackID = GetEquippedStackIndex(std::monostate{}, a_player, a_weapon.Base);
            if (!item || stackID < 0)
            {
                REX::WARN("Ammo switch: equipped weapon stack could not be resolved");
                return false;
            }

            const auto stackIndex = static_cast<std::uint32_t>(stackID);
            auto* stack = GetStack(item, stackIndex);
            if (!stack || !stack->extra || stack->count > 1)
            {
                REX::WARN("Ammo switch: equipped weapon stack is missing instance data or shares {} copies", stack ? stack->count : 0);
                return false;
            }

            auto currentTypeIndex = a_match.CurrentType;
            RE::BGSMod::Attachment::Mod* installed = nullptr;
            const auto dataHandler = RE::TESDataHandler::GetSingleton();
            const auto attachIndex = RE::BGSKeyword::GetIndexForTypedKeyword(profile->AttachPoint, RE::KeywordType::kAttachPoint);
            if (!dataHandler)
                return false;
            for (auto* mod : dataHandler->GetFormArray<RE::BGSMod::Attachment::Mod>())
            {
                if (!mod || mod->attachPoint.keywordIndex != attachIndex || !StackHasOMod(item, stackIndex, mod))
                    continue;
                if (installed)
                {
                    REX::WARN("Ammo switch: multiple installed OMODs use attach point {:04X}; refusing to guess", attachIndex);
                    return false;
                }
                installed = mod;
            }
            // The live equipped instance can retain its previous ammo pointer after an
            // OMOD mutation. Prefer the installed OMOD as the authoritative type when
            // the ammo-derived match and stack state disagree.
            const auto* ammoMatchedType = &profile->Types[currentTypeIndex];
            const auto installedType = std::ranges::find_if(profile->Types, [installed](const auto& type) {
                return type.OMod == installed;
            });
            if (installedType != profile->Types.end() && installedType->Ammo != ammoMatchedType->Ammo)
            {
                currentTypeIndex = static_cast<std::size_t>(std::distance(profile->Types.begin(), installedType));
                REX::WARN("Ammo switch: corrected stale equipped ammo identity from {:08X} to {:08X} using installed OMOD",
                    ammoMatchedType->Ammo->GetFormID(), installedType->Ammo->GetFormID());
            }

            auto* stackInstanceData = item->GetInstanceData(stackIndex);
            auto* extraInstance = stack->extra->GetByType<RE::ExtraInstanceData>();
            auto* extraInstanceData = extraInstance ? extraInstance->data.get() : nullptr;
            const auto instanceAmmoID = [](RE::TBO_InstanceData* a_instanceData) {
                const auto* weaponInstance = static_cast<RE::TESObjectWEAP::InstanceData*>(a_instanceData);
                return weaponInstance && weaponInstance->ammo ? weaponInstance->ammo->GetFormID() : 0u;
            };
            const auto* equippedInstanceData = a_weapon.Item ? a_weapon.Item->item.instanceData.get() : nullptr;
            REX::INFO(
                "Ammo switch state: installedOMod={:08X}, stackInstance={} ammo={:08X}, extraInstance={} ammo={:08X}, equippedInstance={} ammo={:08X}, equippedDataAmmo={:08X}",
                installed ? installed->GetFormID() : 0u,
                static_cast<void*>(stackInstanceData), instanceAmmoID(stackInstanceData),
                static_cast<void*>(extraInstanceData), instanceAmmoID(extraInstanceData),
                const_cast<void*>(static_cast<const void*>(equippedInstanceData)), instanceAmmoID(const_cast<RE::TBO_InstanceData*>(equippedInstanceData)),
                a_weapon.Data && a_weapon.Data->ammo ? a_weapon.Data->ammo->GetFormID() : 0u);

            const auto* currentType = &profile->Types[currentTypeIndex];
            const bool implicitBaseAmmo = currentTypeIndex == 0 && !installed;
            if (installed != currentType->OMod && !implicitBaseAmmo)
            {
                REX::WARN("Ammo switch: current ammo {:08X} does not match its configured OMOD state", currentType->Ammo->GetFormID());
                return false;
            }

            std::size_t nextIndex = profile->Types.size();
            std::uint32_t ownedTargetAmmo = 0;
            if (a_targetType)
            {
                if (*a_targetType >= profile->Types.size())
                    return false;
                nextIndex = *a_targetType;
                a_player->GetItemCount(ownedTargetAmmo, const_cast<RE::TESAmmo*>(profile->Types[nextIndex].Ammo), false);
                if (ownedTargetAmmo == 0)
                {
                    REX::INFO("Ammo switch: clicked ammo {:08X} is not in the player's inventory",
                        profile->Types[nextIndex].Ammo->GetFormID());
                    return false;
                }
            }
            else
            {
                for (std::size_t offset = 1; offset < profile->Types.size(); ++offset)
                {
                    const auto candidateIndex = (currentTypeIndex + offset) % profile->Types.size();
                    std::uint32_t candidateCount = 0;
                    a_player->GetItemCount(candidateCount, const_cast<RE::TESAmmo*>(profile->Types[candidateIndex].Ammo), false);
                    if (candidateCount > 0)
                    {
                        nextIndex = candidateIndex;
                        ownedTargetAmmo = candidateCount;
                        break;
                    }
                }
            }
            if (nextIndex == profile->Types.size())
            {
                REX::WARN("Ammo switch: no other profile ammo is available in the player's inventory");
                return false;
            }
            if (nextIndex == currentTypeIndex)
                return true;
            const auto& nextType = profile->Types[nextIndex];
            REX::INFO("Ammo switch: selected available type {:08X} ({}) after scanning profile",
                nextType.Ammo->GetFormID(), ownedTargetAmmo);

            const auto* targetOMod = nextType.OMod;
            const bool changed = ApplyEquippedOModTransition(
                a_player,
                a_weapon.Base,
                stackID,
                installed,
                const_cast<RE::BGSMod::Attachment::Mod*>(targetOMod),
                const_cast<RE::TESAmmo*>(nextType.Ammo));

            if (changed)
            {
                REX::INFO("Ammo switch: profile '{}' changed ammo {:08X} -> {:08X}", profile->Id,
                    currentType->Ammo->GetFormID(), nextType.Ammo->GetFormID());
                if (a_automaticSwitch && ManualReload::IsManualReloadRequired(a_weapon.Base, a_weapon.Instance))
                {
                    if (const auto live = Core::GetEquippedWeapon(a_player); live && live->Data)
                    {
                        live->Data->ammo = const_cast<RE::TESAmmo*>(nextType.Ammo);
                        live->Data->ammoCount = 0;
                    }
                    QueueWeaponCardRefresh();
                    REX::INFO("Ammo switch: manual reload governs the newly selected ammo");
                }
                else
                {
                    RequestAmmoReload(
                        a_player, a_weapon.Base, const_cast<RE::TESAmmo*>(nextType.Ammo), a_pipboyReloadAnimation);
                }
            }
            else
            {
                REX::WARN("Ammo switch: profile '{}' failed to apply OMOD transition for {:08X} -> {:08X}", profile->Id,
                    currentType->Ammo->GetFormID(), nextType.Ammo->GetFormID());
            }
            return changed;
        }

        void CycleOnGameThread(std::optional<WeaponConfig::OModCategory> a_category, std::string_view a_keyID = {})
        {
            if (a_category == WeaponConfig::OModCategory::Ammunition)
                REX::INFO("Ammo switch: hotkey task callback started");

            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto weapon = Core::GetEquippedWeapon(player);
            if (!weapon)
            {
                REX::INFO("OMod switch: no drawn weapon");
                return;
            }

            const auto groups = WeaponConfig::MatchOModSwitchGroups(weapon->Base, weapon->Instance);
            if (a_category == WeaponConfig::OModCategory::Ammunition || !a_keyID.empty())
            {
                const auto* currentAmmo = weapon->Instance->ammo ? weapon->Instance->ammo : weapon->Base->weaponData.ammo;
                if (a_category == WeaponConfig::OModCategory::Ammunition)
                    REX::INFO("Ammo switch: equipped weapon {:08X}, current ammo {:08X}",
                        weapon->Base->GetFormID(), currentAmmo ? currentAmmo->GetFormID() : 0);
                if (const auto ammoMatch = WeaponConfig::MatchAmmoSwitch(
                        currentAmmo, weapon->Base ? weapon->Base->weaponData.ammo : nullptr))
                {
                    const bool requestedAmmoProfile = a_category == WeaponConfig::OModCategory::Ammunition ||
                        (!a_keyID.empty() && ammoMatch->Profile->KeyID == a_keyID);
                    const bool conflictingGroup = requestedAmmoProfile && std::ranges::any_of(groups, [&](const auto& group) {
                        return a_keyID.empty() ? group.Category == WeaponConfig::OModCategory::Ammunition : group.KeyID == a_keyID;
                    });
                    if (conflictingGroup)
                    {
                        REX::WARN("Ammo switch: weapon {:08X} has an ammo profile and an OMOD group assigned to the requested hotkey; refusing ambiguous hotkey",
                            weapon->Base->GetFormID());
                        return;
                    }
                    if (requestedAmmoProfile)
                    {
                        SwitchAmmoOnGameThread(player, *weapon, *ammoMatch);
                        return;
                    }
                }
            }
            const WeaponConfig::OModSwitchGroup* selected = nullptr;
            for (const auto& group : groups)
            {
                if ((!a_keyID.empty() && group.KeyID != a_keyID) ||
                    (a_keyID.empty() && (!a_category || group.Category != *a_category)))
                    continue;
                if (selected)
                {
                    REX::WARN("OMod switch: more than one configured group matches the requested category or keyID");
                    return;
                }
                selected = &group;
            }
            if (!selected)
            {
                REX::INFO("OMod switch: weapon {:08X} has no group for the requested category", weapon->Base->GetFormID());
                return;
            }

            const bool refreshEquipped = nativeOModRefreshAvailable && selected->RefreshEquipped;
            if (refreshEquipped)
            {
                const auto gunState = Core::GetGunState(player);
                const bool thirdPerson = player->Is3rdPersonVisible();
                const bool ready = gunState == RE::GUN_STATE::kDrawn ||
                    Core::IsThirdPersonReadyStance(player, gunState);
                if (!ready || player->meleeAttackState != 0 ||
                    (thirdPerson && (!player->GetWeaponMagicDrawn() || SightedReload::AimButtonHeld())))
                {
                    REX::INFO("OMod switch: lower the sights and finish the current weapon action before switching '{}' (gun state {}, third person {})",
                        selected->Id, std::to_underlying(gunState), thirdPerson);
                    return;
                }
            }

            if (selected->AttachPoint && !weapon->Base->attachParents.HasKeyword(selected->AttachPoint) &&
                !Core::HasDefaultTemplateAttachPoint(weapon->Base, selected->AttachPoint))
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
            const auto attachIndex = RE::BGSKeyword::GetIndexForTypedKeyword(selected->AttachPoint, RE::KeywordType::kAttachPoint);
            for (const auto& entry : StackOModEntries(stack))
            {
                auto* mod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(entry.objectID);
                if (!mod || mod->attachPoint.keywordIndex != attachIndex)
                    continue;
                // A vanilla template can retain its collection OMOD alongside the
                // selected weapon OMOD. Disabled entries do not affect the weapon.
                if (entry.disabled || mod->targetFormType != RE::ENUM_FORM_ID::kWEAP)
                {
                    REX::INFO("OMod switch: ignoring stack entry {:08X} at attach point {:04X} (disabled={}, weaponMod={})",
                        entry.objectID, attachIndex, entry.disabled != 0, mod->targetFormType == RE::ENUM_FORM_ID::kWEAP);
                    continue;
                }
                if (current)
                {
                    REX::WARN("OMod switch: multiple active weapon OMODs {:08X} and {:08X} use attach point {:04X}; refusing to guess",
                        current->GetFormID(), entry.objectID, attachIndex);
                    return;
                }
                current = mod;
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
            if (refreshEquipped)
            {
                if (ApplyEquippedOModTransition(player, weapon->Base, stackID, current, next))
                {
                    QueueWeaponCardRefresh();
                    REX::INFO("OMod switch: group '{}' applied {:08X} -> {:08X} through the native path",
                        selected->Id, current ? current->GetFormID() : 0u, next->GetFormID());
                }
                else
                    REX::WARN("OMod switch: group '{}' could not apply OMOD {:08X}", selected->Id, next->GetFormID());
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
            RE::TBO_InstanceData* a_previousInstanceData,
            RE::TBO_InstanceData* a_newInstanceData = nullptr)
        {
            static REL::Relocation<std::uintptr_t> weaponDataVtable{ RE::VTABLE::EquippedWeaponData[0] };
            auto* actor = a_owner ? a_owner->As<RE::Actor>() : nullptr;
            auto* instanceData = a_newInstanceData ? a_newInstanceData :
                                 (a_item ? a_item->GetInstanceData(a_stackIndex) : nullptr);
            if (!instanceData)
            {
                auto* stack = GetStack(a_item, a_stackIndex);
                auto* extraInstanceData = stack && stack->extra ? stack->extra->GetByType<RE::ExtraInstanceData>() : nullptr;
                instanceData = extraInstanceData ? extraInstanceData->data.get() : nullptr;
            }
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

            // Persist before replacing the live equipped pointer. If this fails,
            // the caller can restore the stack while the old instance is still equipped.
            const bool stackInstanceStored = StoreStackInstanceData(a_owner, a_item, a_stackIndex, instanceData);
            if (!stackInstanceStored)
            {
                REX::ERROR("Equipped OMOD refresh could not persist instance data for item {:08X}, stack={}",
                    a_item->object->GetFormID(), a_stackIndex);
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
            auto* fireNodeBefore = equippedWeaponData ? equippedWeaponData->fireNode : nullptr;
            const auto fireLocationCount = equippedWeaponData ? equippedWeaponData->fireLocations.size() : 0;
            {
                RE::BGSObjectInstanceT<RE::TESObjectWEAP> weaponInstance{ weapon, instanceData };
                if (equippedWeaponData)
                {
                    equippedWeaponData->SetupFireSounds(*actor, weaponInstance);
                    refreshedFireSounds = true;
                }
                actor->RemoveWeapon(weaponInstance, equipIndex, false);
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

            auto* fireNodeAfter = equippedWeaponData ? equippedWeaponData->fireNode : nullptr;
            const auto fireLocationCountAfter = equippedWeaponData ? equippedWeaponData->fireLocations.size() : 0;

            REX::INFO(
                "Equipped OMOD data refreshed for item {:08X}, equipIndex={}, thirdPerson={}, firstPerson={}, fireSounds={}, stackInstanceStored={}, fireNode={:p}->{:p}, fireLocations={}->{}",
                a_item->object->GetFormID(),
                equipIndex.index,
                refreshedThirdPerson,
                refreshedFirstPerson,
                refreshedFireSounds,
                stackInstanceStored,
                static_cast<void*>(fireNodeBefore),
                static_cast<void*>(fireNodeAfter),
                fireLocationCount,
                fireLocationCountAfter);
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
                previousInstanceData = GetEquippedInstanceData(a_owner, item);
                if (!CanRefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
                {
                    REX::WARN(
                        "OMod mutation rejected before write: equipped stack identity cannot be resolved for item {:08X}, stack={}",
                        a_itemForm->GetFormID(),
                        stackIndex);
                    return false;
                }
            }

            RE::TBO_InstanceData* newInstanceData = nullptr;
            if (!MutateStackOMod(a_owner, item, stackIndex, a_omod, a_attach, &newInstanceData))
            {
                RE::TBO_InstanceData* restoredInstanceData = nullptr;
                const bool restored = EnsureItemOModState(
                    a_owner, item, stackIndex, a_omod, hadOMod, &restoredInstanceData);
                const bool refreshed = !wasEquipped ||
                                       RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData, restoredInstanceData);
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

            if (RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData, newInstanceData))
            {
                return true;
            }

            RE::TBO_InstanceData* restoredInstanceData = nullptr;
            const bool restored = EnsureItemOModState(
                a_owner, item, stackIndex, a_omod, hadOMod, &restoredInstanceData);
            const bool refreshed = RefreshEquippedItem(
                a_owner, item, stackIndex, previousInstanceData, restoredInstanceData);
            REX::ERROR(
                "OMod mutation refresh failed for item {:08X}, OMOD={:08X}; original state restored={}, equipped refresh={}, final OMOD present={}",
                a_itemForm->GetFormID(),
                a_omod->GetFormID(),
                restored,
                refreshed,
                StackHasOMod(item, stackIndex, a_omod));
            return false;
        }

        bool ApplyEquippedOModTransition(
            RE::PlayerCharacter* a_owner,
            RE::TESObjectWEAP* a_itemForm,
            std::int32_t a_stackIndex,
            RE::BGSMod::Attachment::Mod* a_fromOMod,
            RE::BGSMod::Attachment::Mod* a_toOMod,
            RE::TESAmmo* a_targetAmmo)
        {
            if (!a_owner || !a_itemForm || a_stackIndex < 0 ||
                (!a_fromOMod && !a_toOMod) || a_fromOMod == a_toOMod ||
                (a_fromOMod && a_toOMod && !HasSameAttachPoint(a_fromOMod, a_toOMod)))
                return false;

            if (nativeOModRefreshAvailable)
            {
                auto* manager = RE::ActorEquipManager::GetSingleton();
                if (!nativeStackWriter.address() || !manager)
                    return false;

                const bool preserveMagazine = !a_targetAmmo;
                const auto original = Core::GetEquippedWeapon(a_owner);
                if (!original || original->Base != a_itemForm || !original->Data)
                    return false;
                auto* originalAmmo = original->Instance->ammo;
                const auto originalCapacity = original->Instance->ammoCapacity;
                const auto originalLoaded = original->Data->ammoCount;
                if (!originalAmmo)
                    return false;

                const auto stackIndex = [&]() {
                    return GetEquippedStackIndex(std::monostate{}, a_owner, a_itemForm);
                };
                if (stackIndex() != a_stackIndex)
                    return false;
                if (preserveMagazine)
                {
                    auto* item = FindInventoryItem(a_owner, a_itemForm);
                    const auto* stored = item ?
                        static_cast<const RE::TESObjectWEAP::InstanceData*>(item->GetInstanceData(a_stackIndex)) : nullptr;
                    if (!stored || stored->ammo != originalAmmo || stored->ammoCapacity != originalCapacity)
                    {
                        REX::WARN("OMod switch: inventory and equipped instance disagree on ammo or capacity");
                        return false;
                    }
                    // A loaded save can retain the previous weapon's ammo in the
                    // firing cache even when both weapon instances agree.
                    if (original->Data->ammo != originalAmmo)
                        REX::INFO("OMod switch: equipped ammo cache {:08X} differs from weapon instance {:08X}",
                            original->Data->ammo ? original->Data->ammo->GetFormID() : 0u, originalAmmo->GetFormID());
                }
                const auto hasMod = [&](RE::BGSMod::Attachment::Mod* a_mod) {
                    const auto index = stackIndex();
                    if (!a_mod || index < 0)
                        return false;
                    auto* item = FindInventoryItem(a_owner, a_itemForm);
                    if (!preserveMagazine)
                        return StackHasOMod(item, static_cast<std::uint32_t>(index), a_mod);
                    const auto formID = a_mod->GetFormID();
                    return std::ranges::any_of(StackOModEntries(GetStack(item, static_cast<std::uint32_t>(index))),
                        [formID](const auto& a_entry) { return !a_entry.disabled && a_entry.objectID == formID; });
                };
                const auto writeMod = [&](RE::BGSMod::Attachment::Mod* a_mod, bool a_attach) {
                    const auto index = stackIndex();
                    auto* item = FindInventoryItem(a_owner, a_itemForm);
                    auto* stack = index >= 0 ? GetStack(item, static_cast<std::uint32_t>(index)) : nullptr;
                    if (!stack || stack->count != 1)
                        return false;

                    RE::BGSInventoryItem::CheckStackIDFunctor compare{ static_cast<std::uint32_t>(index) };
                    bool accepted = false;
                    RE::BGSInventoryItem::ModifyModDataFunctor write{ a_mod, 0, a_attach, &accepted };
                    write.shouldSplitStacks = false;
                    // Both verified runtimes dereference the final argument.
                    auto ownerHandle = a_owner->GetHandle().get_handle();
                    if (!ownerHandle)
                        return false;
                    const bool found = nativeStackWriter(item, compare, write, false, &ownerHandle);
                    const bool applied = stackIndex() >= 0 && hasMod(a_mod) == a_attach;
                    REX::INFO("OMod switch: native stack write {:08X}, attach={}, found={}, accepted={}, applied={}",
                        a_mod->GetFormID(), a_attach, found, accepted, applied);
                    return found && accepted && applied;
                };
                bool triedEquip = false;
                const auto equip = [&](RE::TESAmmo* a_expectedAmmo) {
                    const auto index = stackIndex();
                    auto* item = FindInventoryItem(a_owner, a_itemForm);
                    if (index < 0 || !item)
                        return false;
                    auto* instance = static_cast<RE::TESObjectWEAP::InstanceData*>(item->GetInstanceData(index));
                    if (!instance || instance->ammo != a_expectedAmmo ||
                        (preserveMagazine && instance->ammoCapacity != originalCapacity))
                    {
                        REX::WARN("OMod switch: native inventory instance missing or unexpected ammo/capacity "
                            "(ammo={:08X}, expected={:08X}, capacity={}, originalCapacity={})",
                            instance && instance->ammo ? instance->ammo->GetFormID() : 0u, a_expectedAmmo->GetFormID(),
                            instance ? instance->ammoCapacity : 0, originalCapacity);
                        return false;
                    }
                    const RE::BGSObjectInstanceT<RE::TESObjectWEAP> object{ a_itemForm, instance };
                    skipEquipGraphFor = a_owner;
                    triedEquip = true;
                    const bool equipped = manager->EquipObject(a_owner, object, index, 1, nullptr, false, false, false, true, false);
                    const bool skipped = skipEquipGraphFor == nullptr;
                    skipEquipGraphFor = nullptr;
                    const auto live = Core::GetEquippedWeapon(a_owner);
                    const bool matches = equipped && live && live->Base == a_itemForm && live->Item &&
                        live->Item->item.instanceData && live->Item->item.instanceData.get() == instance &&
                        live->Instance->ammo == a_expectedAmmo &&
                        (!preserveMagazine || (live->Data && live->Instance->ammoCapacity == originalCapacity));
                    REX::INFO("OMod switch: native equip returned {}, graphSkipped={}, equippedAmmoMatches={}",
                        equipped, skipped, matches);
                    if (matches && preserveMagazine)
                    {
                        const auto refreshedCount = live->Data->ammoCount;
                        if (live->Data->ammo != a_expectedAmmo)
                        {
                            live->Data->ammo = a_expectedAmmo;
                            REX::INFO("OMod switch: synchronized equipped ammo cache to {:08X}", a_expectedAmmo->GetFormID());
                        }
                        live->Data->ammoCount = originalLoaded;
                        REX::INFO("OMod switch: preserved {} loaded rounds (native equip reported {})",
                            originalLoaded, refreshedCount);
                    }
                    return matches;
                };

                if ((a_fromOMod && !hasMod(a_fromOMod)) ||
                    hasMod(a_toOMod))
                    return false;
                if ((!a_fromOMod || writeMod(a_fromOMod, false)) &&
                    (!a_toOMod || writeMod(a_toOMod, true)) &&
                    (!preserveMagazine || !hasMod(a_fromOMod)) &&
                    equip(a_targetAmmo ? a_targetAmmo : originalAmmo))
                    return true;

                if (hasMod(a_toOMod))
                    writeMod(a_toOMod, false);
                if (a_fromOMod && !hasMod(a_fromOMod))
                    writeMod(a_fromOMod, true);
                const bool restored = stackIndex() >= 0 && !hasMod(a_toOMod) && (!a_fromOMod || hasMod(a_fromOMod));
                const bool equipmentRestored = !triedEquip || (restored && originalAmmo && equip(originalAmmo));
                if (triedEquip && equipmentRestored)
                {
                    if (const auto live = Core::GetEquippedWeapon(a_owner); live && live->Data)
                        live->Data->ammoCount = originalLoaded;
                }
                REX::WARN("OMod switch: native transition failed; stackRestored={}, equipmentRestored={}",
                    restored, equipmentRestored);
                return false;
            }

            if (a_fromOMod && a_toOMod)
                return ReplaceItemOMod(std::monostate{}, a_owner, a_itemForm, a_stackIndex, a_fromOMod, a_toOMod);
            if (a_fromOMod)
                return SetItemOMod(std::monostate{}, a_owner, a_itemForm, a_stackIndex, a_fromOMod, false);
            return SetItemOMod(std::monostate{}, a_owner, a_itemForm, a_stackIndex, a_toOMod, true);
        }
    }

    bool HasAvailableAmmoSwitchTarget(
        RE::PlayerCharacter* a_player,
        const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& a_weapon)
    {
        if (!Settings::AutoSwitchAmmoWhenEmpty() || !a_player || !a_weapon.object)
            return false;

        const auto live = Core::GetEquippedWeapon(a_player);
        const auto* base = a_weapon.object->As<RE::TESObjectWEAP>();
        if (!base)
            return false;
        if (!live || live->Base != base || !live->Data || live->Data->ammoCount != 1)
            return false;

        const auto* instance = static_cast<const RE::TESObjectWEAP::InstanceData*>(a_weapon.instanceData.get());
        const auto* currentAmmo = instance && instance->ammo ? instance->ammo : base->weaponData.ammo;
        const auto match = WeaponConfig::MatchAmmoSwitch(currentAmmo, base->weaponData.ammo);
        if (!match || !match->Profile)
            return false;

        const auto& types = match->Profile->Types;
        for (std::size_t offset = 1; offset < types.size(); ++offset)
        {
            const auto index = (match->CurrentType + offset) % types.size();
            std::uint32_t count = 0;
            a_player->GetItemCount(count, const_cast<RE::TESAmmo*>(types[index].Ammo), false);
            if (count > 0)
                return true;
        }
        return false;
    }

    void OnAmmoDepleted(RE::TESObjectWEAP* a_weapon, const RE::TESObjectWEAP::InstanceData* a_instance)
    {
        if (!Settings::AutoSwitchAmmoWhenEmpty() || !a_weapon || !a_instance)
            return;

        const auto* tasks = F4SE::GetTaskInterface();
        if (!tasks)
            return;

        tasks->AddTask([a_weapon, a_instance]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto live = Core::GetEquippedWeapon(player);
            if (!player || !live || live->Base != a_weapon || !live->Item || !live->Data ||
                live->Item->item.instanceData.get() != a_instance || live->Data->ammoCount != 0)
                return;

            const auto* currentAmmo = live->Instance->ammo ? live->Instance->ammo : a_weapon->weaponData.ammo;
            const auto match = WeaponConfig::MatchAmmoSwitch(currentAmmo, a_weapon->weaponData.ammo);
            if (!match)
                return;

            REX::INFO("Ammo switch: magazine depleted for weapon {:08X}; switching to the next available configured ammo",
                a_weapon->GetFormID());
            if (!SwitchAmmoOnGameThread(player, *live, *match, std::nullopt, true, true))
            {
                REX::INFO("Ammo switch: automatic switch did not find or apply another available ammo type");
                const auto currentWeapon = Core::GetEquippedWeapon(player);
                if (currentWeapon && currentWeapon->Base == a_weapon && currentWeapon->Instance &&
                    !ManualReload::IsManualReloadRequired(currentWeapon->Base, currentWeapon->Instance))
                {
                    const auto* ammo = currentWeapon->Instance->ammo ? currentWeapon->Instance->ammo : a_weapon->weaponData.ammo;
                    if (ammo)
                        RequestAmmoReload(player, a_weapon, const_cast<RE::TESAmmo*>(ammo), true);
                }
            }
        });
    }

    void ResetPendingAmmoReload()
    {
        const std::scoped_lock lock{ pendingReloadLock };
        pendingReload.reset();
    }

    void TickPendingAmmoReload()
    {
        std::uint64_t generation;
        const auto* tasks = F4SE::GetTaskInterface();
        if (!tasks)
            return;
        {
            const std::scoped_lock lock{ pendingReloadLock };
            if (!pendingReload || pendingReload->queued || ReloadClock::now() < pendingReload->nextAttempt)
                return;
            pendingReload->queued = true;
            generation = pendingReload->generation;
        }
        tasks->AddTask([generation]() { RetryAmmoReload(generation); });
    }

    bool SwitchEquippedAmmoFromPipboy(RE::TESAmmo* a_targetAmmo)
    {
        if (!a_targetAmmo)
            return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto weapon = Core::GetEquippedWeapon(player);
        if (!player || !weapon)
            return false;

        if (!weapon->Base || !weapon->Instance)
            return false;
        const auto* currentAmmo = weapon->Instance->ammo ? weapon->Instance->ammo : weapon->Base->weaponData.ammo;
        const auto* weaponBaseAmmo = weapon->Base->weaponData.ammo;
        const auto currentMatch = WeaponConfig::MatchAmmoSwitch(currentAmmo, weaponBaseAmmo);
        const auto targetMatch = WeaponConfig::MatchAmmoSwitch(a_targetAmmo, weaponBaseAmmo);
        if (!currentMatch || !targetMatch || currentMatch->Profile != targetMatch->Profile)
        {
            REX::INFO("Ammo switch: Pip-Boy ammo {:08X} is not compatible with equipped weapon {:08X}; keeping vanilla cannot-equip response",
                a_targetAmmo->GetFormID(), weapon->Base->GetFormID());
            return false;
        }

        REX::INFO("Ammo switch: Pip-Boy selected {:08X} for weapon {:08X}",
            a_targetAmmo->GetFormID(), weapon->Base->GetFormID());
        const auto* previousAmmo = currentAmmo;
        const bool switched = SwitchAmmoOnGameThread(
            player, *weapon, *currentMatch, targetMatch->CurrentType, Settings::PipboyAmmoReloadAnimation());
        if (switched)
            UpdatePipboyAmmoEquipState(a_targetAmmo);
        if (switched && previousAmmo != a_targetAmmo)
        {
            player->PlayPickUpSound(a_targetAmmo, true, false);
        }
        return switched;
    }

    void InstallPipboyAmmoMarkerSync()
    {
        static PipboyAmmoMarkerWatcher watcher;
        if (auto* ui = RE::UI::GetSingleton())
        {
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&watcher);
            REX::INFO("Ammo switch: Pip-Boy equipped-ammo marker sync registered");
        }
    }

    void Initialize()
    {
        InitializeNativeOModRuntime();
        const auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
            return;

        std::uint32_t added = 0;
        std::uint32_t ammoAdded = 0;
        for (auto* weapon : dataHandler->GetFormArray<RE::TESObjectWEAP>())
        {
            if (!weapon)
                continue;
            // The engine validates stored attachments during load, before the hotkey can add the slot.
            if (const auto ammoMatch = WeaponConfig::MatchAmmoSwitch(weapon->weaponData.ammo, weapon->weaponData.ammo))
            {
                auto* attachPoint = ammoMatch->Profile->AttachPoint;
                if (attachPoint && !weapon->attachParents.HasKeyword(attachPoint) &&
                    !Core::HasDefaultTemplateAttachPoint(weapon, attachPoint))
                {
                    AddAttachParent(weapon->attachParents, attachPoint);
                    ++ammoAdded;
                }
            }
            for (const auto& group : WeaponConfig::MatchOModSwitchGroups(weapon, nullptr))
            {
                auto* attachPoint = group.AttachPoint;
                if (attachPoint && !weapon->attachParents.HasKeyword(attachPoint) &&
                    !Core::HasDefaultTemplateAttachPoint(weapon, attachPoint))
                {
                    AddAttachParent(weapon->attachParents, attachPoint);
                    ++added;
                }
            }
        }
        REX::INFO("OMod switch: registered {} ammo and {} attachment slot(s) before save load", ammoAdded, added);
    }

    void TraceAmmoAfterLoad()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto weapon = Core::GetEquippedWeapon(player);
        if (!weapon || !weapon->Data)
            return;

        const auto* ammo = weapon->Instance->ammo;
        const auto* cachedAmmo = weapon->Data->ammo;
        const auto* weaponBaseAmmo = weapon->Base ? weapon->Base->weaponData.ammo : nullptr;
        if (!WeaponConfig::MatchAmmoSwitch(ammo, weaponBaseAmmo) &&
            !WeaponConfig::MatchAmmoSwitch(cachedAmmo, weaponBaseAmmo))
            return;

        REX::INFO("Ammo switch: after load weapon={:08X}, instanceAmmo={:08X}, cachedAmmo={:08X}, loaded={}",
            weapon->Base->GetFormID(), ammo ? ammo->GetFormID() : 0u,
            cachedAmmo ? cachedAmmo->GetFormID() : 0u, weapon->Data->ammoCount);
    }

    void ResetPendingSwitches()
    {
        switchGeneration.fetch_add(1);
    }

    bool CycleEquippedOMod(WeaponConfig::OModCategory a_category)
    {
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks)
        {
            if (a_category == WeaponConfig::OModCategory::Ammunition)
                REX::WARN("Ammo switch: Papyrus hotkey reached MWB, but the F4SE task interface is unavailable");
            return false;
        }
        const auto generation = switchGeneration.load();
        tasks->AddTask([a_category, generation]() {
            if (generation != switchGeneration.load())
            {
                REX::INFO("OMod switch: cancelled queued hotkey during save load");
                return;
            }
            CycleOnGameThread(a_category);
        });
        return true;
    }

    bool CycleEquippedOMod(std::string_view a_keyID)
    {
        if (a_keyID.empty())
            return false;
        const auto tasks = F4SE::GetTaskInterface();
        if (!tasks)
            return false;
        const auto keyID = std::string{ a_keyID };
        const auto generation = switchGeneration.load();
        tasks->AddTask([keyID, generation]() {
            if (generation != switchGeneration.load())
            {
                REX::INFO("OMod switch: cancelled queued hotkey during save load");
                return;
            }
            CycleOnGameThread(std::nullopt, keyID);
        });
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
            previousInstanceData = GetEquippedInstanceData(a_owner, item);
            if (!CanRefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData))
            {
                REX::WARN(
                    "OMod replacement rejected before write: equipped stack identity cannot be resolved for item {:08X}, stack={}",
                    a_itemForm->GetFormID(),
                    stackIndex);
                return false;
            }
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
            RE::TBO_InstanceData* rollbackInstanceData = nullptr;
            bool destinationRemoved = true;
            if (StackHasOMod(item, stackIndex, a_toOMod))
            {
                destinationRemoved = StackHasOMod(item, stackIndex, a_fromOMod) ?
                    MutateStackOMod(a_owner, item, stackIndex, a_toOMod, false, &rollbackInstanceData) :
                    ReplaceStackOMod(a_owner, item, stackIndex, a_toOMod, a_fromOMod, &rollbackInstanceData);
            }
            bool sourceRestored = StackHasOMod(item, stackIndex, a_fromOMod);
            if (!sourceRestored)
            {
                sourceRestored = MutateStackOMod(
                    a_owner, item, stackIndex, a_fromOMod, true, &rollbackInstanceData);
            }
            destinationRemoved = destinationRemoved && !StackHasOMod(item, stackIndex, a_toOMod);
            sourceRestored = sourceRestored && StackHasOMod(item, stackIndex, a_fromOMod);
            const bool equippedRefreshed = !wasEquipped ||
                                           RefreshEquippedItem(
                                               a_owner, item, stackIndex, previousInstanceData, rollbackInstanceData);
            return RecoveryResult{ destinationRemoved, sourceRestored, equippedRefreshed };
        };

        RE::TBO_InstanceData* newInstanceData = nullptr;
        if (!ReplaceStackOMod(a_owner, item, stackIndex, a_fromOMod, a_toOMod, &newInstanceData))
        {
            const auto recovery = rollbackReplacement();
            REX::ERROR(
                "OMod replacement mutation failed; recovery complete={}, destinationRemoved={}, sourceRestored={}, equippedRefreshed={}, item={:08X}, source={:08X}, destination={:08X}, sourcePresent={}, destinationPresent={}",
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

        if (wasEquipped && !RefreshEquippedItem(a_owner, item, stackIndex, previousInstanceData, newInstanceData))
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
