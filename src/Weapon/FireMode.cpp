/*
    This file is part of Modern Weapon Behavior.
    Modern Weapon Behavior is free software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.
    Modern Weapon Behavior is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with Modern Weapon Behavior. If not, see <https://www.gnu.org/licenses/>.
*/

#include "FireMode.h"
#include "Actions/FirstPerson.h"
#include "Config/WeaponConfig.h"
#include "Core/EquippedWeapon.h"
#include "Settings/Settings.h"

#include <F4SE/API.h>

namespace
{
    constexpr RE::TESFormID SoundAutoKeywordID = 0x0A191C;
    constexpr RE::TESFormID WeaponTypeAutomaticID = 0x04A0A2;

    std::array<RE::BGSKeyword*, 2> ModeKeywords{};

    std::optional<RE::BSFixedString> StartEvent;
    std::optional<RE::BSFixedString> Annotation;
    std::optional<RE::BSFixedString> DoneEvent;
    std::optional<RE::BSFixedString> ExitEvent;

    std::atomic<bool> Pending{ false };
    std::atomic<bool> Requested{ false };
    std::atomic<bool> SwitchTaskQueued{ false };
    std::atomic<std::uint32_t> SwitchGeneration{ 0 };

    void AddAttachParent(RE::BGSAttachParentArray& Parents, RE::BGSKeyword* Keyword)
    {
        using Value = RE::BGSTypedKeywordValue<RE::KeywordType::kAttachPoint>;
        auto& Memory = RE::MemoryManager::GetSingleton();
        const auto Values = static_cast<Value*>(Memory.Allocate(sizeof(Value) * (Parents.size + 1), 0, false));

        for (std::uint32_t i = 0; i < Parents.size; ++i)
            Values[i] = Parents.array[i];

        Values[Parents.size].keywordIndex = RE::BGSKeyword::GetIndexForTypedKeyword(Keyword, RE::KeywordType::kAttachPoint);

        if (Parents.array)
            Memory.Deallocate(Parents.array, false);

        Parents.array = Values;
        ++Parents.size;
    }

    [[nodiscard]] bool IsCharging(const RE::TESObjectWEAP::InstanceData& Data)
    {
        return Data.flags.any(RE::WEAPON_FLAGS::kChargingAttack, RE::WEAPON_FLAGS::kHoldInputToPower);
    }

    [[nodiscard]] bool ApplyKeywords(RE::TESObjectWEAP::InstanceData& Instance, bool Automatic)
    {
        const auto Keywords = Instance.keywords;

        if (!Keywords)
            return false;

        using RemoveKeywordSignature = void(RE::BGSKeywordForm*, RE::BGSKeyword*);
        static REL::Relocation<RemoveKeywordSignature*> RemoveKeyword{ RE::ID::BGSKeywordForm::RemoveKeyword };

        for (const auto Keyword : ModeKeywords)
        {
            if (!Keyword)
                continue;

            if (Automatic)
                Keywords->AddKeyword(Keyword);
            else
                RemoveKeyword(Keywords, Keyword);
        }

        return true;
    }

    struct EquippedStack
    {
        RE::BGSInventoryItem::Stack* Stack{ nullptr };
        std::uint32_t Index{ 0 };
    };

    [[nodiscard]] EquippedStack FindEquippedStack(RE::BGSInventoryList& List, const RE::TESBoundObject* Base);

    [[nodiscard]] std::optional<std::string> WhyNot(RE::PlayerCharacter* Player)
    {
        const auto Weapon = Core::GetEquippedWeapon(Player);

        if (!Weapon)
            return "no drawn weapon"s;

        const auto ID = Weapon->Base->GetFormID();

        if (!WeaponConfig::MatchFireMode(Weapon->Base, Weapon->Instance))
            return std::format("the weapon {:08X} has no \"fireModeSwitch\" in the JSON files", ID);

        if (IsCharging(*Weapon->Instance))
            return std::format("the weapon {:08X} charges its shots", ID);

        if (auto Busy = FirstPerson::WeaponBusy(Player))
            return Busy;

        if (const auto List = Player->inventoryList)
        {
            const RE::BSAutoReadLock Lock{ List->rwLock };

            if (const auto Stack = FindEquippedStack(*List, Weapon->Base).Stack; Stack && Stack->count > 1)
                return std::format("{} identical copies of the weapon {:08X} share the stack", Stack->count, ID);
        }

        return std::nullopt;
    }

    [[nodiscard]] EquippedStack FindEquippedStack(RE::BGSInventoryList& List, const RE::TESBoundObject* Base)
    {
        for (auto& Item : List.data)
        {
            if (Item.object != Base)
                continue;

            std::uint32_t Index = 0;

            for (auto Stack = Item.stackData.get(); Stack; Stack = Stack->nextStack.get(), ++Index)
            {
                if (Stack->IsEquipped())
                    return { Stack, Index };
            }
        }

        return {};
    }

    void NotSwitched(std::string_view Reason)
    {
        REX::INFO("Fire mode: not switched, {}", Reason);
    }

    void Reequip(RE::PlayerCharacter* Player, RE::TESObjectWEAP* Base, std::uint32_t StackID)
    {
        const auto Manager = RE::ActorEquipManager::GetSingleton();

        if (!Manager)
            return;

        const RE::BGSObjectInstance Object{ Base, nullptr };
        Manager->UnequipObject(Player, &Object, 1, nullptr, StackID, false, false, true, false, nullptr);
        Manager->EquipObject(Player, Object, StackID, 1, nullptr, true, false, true, false, false);
    }

    void Switch(RE::PlayerCharacter* Player)
    {
        const auto Weapon = Core::GetEquippedWeapon(Player);

        if (!Weapon)
        {
            NotSwitched("no drawn weapon"sv);
            return;
        }

        if (IsCharging(*Weapon->Instance))
        {
            NotSwitched(std::format("the weapon {:08X} charges its shots", Weapon->Base->GetFormID()));
            return;
        }

        const auto List = Player->inventoryList;

        if (!List)
        {
            NotSwitched("no inventory"sv);
            return;
        }

        const auto Rule = WeaponConfig::MatchFireMode(Weapon->Base, Weapon->Instance);
        if (!Rule)
        {
            NotSwitched("the weapon has no valid fireModeSwitch configuration"sv);
            return;
        }

        const bool Automatic = !Weapon->Instance->flags.all(RE::WEAPON_FLAGS::kAutomatic);
        const auto Mod = Automatic ? Rule->AutomaticOMod : Rule->SemiAutomaticOMod;
        const bool OwnInstance = Weapon->Item && Weapon->Item->item.instanceData;

        std::uint32_t StackID = 0;

        {
            const RE::BSAutoWriteLock Lock{ List->rwLock };
            const auto [Stack, Index] = FindEquippedStack(*List, Weapon->Base);

            if (!Stack)
            {
                NotSwitched(std::format("the stack of the weapon {:08X} was not found", Weapon->Base->GetFormID()));
                return;
            }

            if (Stack->count > 1)
            {
                NotSwitched(std::format("{} identical copies of the weapon {:08X} share the stack", Stack->count, Weapon->Base->GetFormID()));
                return;
            }

            StackID = Index;

            if (!Stack->extra)
                Stack->extra.reset(new RE::ExtraDataList());

            const auto Extra = Stack->extra.get();
            auto Mods = Extra->GetByType<RE::BGSObjectInstanceExtra>();

            if (!Mods)
            {
                Mods = new RE::BGSObjectInstanceExtra();
                Extra->AddExtra(Mods);
            }

            Mods->AddMod(*Mod, 0, 1, true);

            if (OwnInstance)
            {
                Weapon->Instance->flags.set(Automatic, RE::WEAPON_FLAGS::kAutomatic);

                if (const auto Data = Extra->GetByType<RE::ExtraInstanceData>(); Data && Data->data.get() != Weapon->Instance)
                {
                    if (const auto Other = static_cast<RE::TESObjectWEAP::InstanceData*>(Data->data.get()))
                        Other->flags.set(Automatic, RE::WEAPON_FLAGS::kAutomatic);
                }
            }
            else
            {
                Extra->RemoveExtra<RE::ExtraInstanceData>();
                Extra->CreateInstanceData(Weapon->Base, true);
            }
        }

        if (!OwnInstance)
        {
            REX::INFO("Fire mode: weapon {:08X} -> {} (new instance, equipped again)", Weapon->Base->GetFormID(), Automatic ? "automatic" : "semi-automatic");
            Reequip(Player, Weapon->Base, StackID);
            return;
        }

        const bool Keywords = ApplyKeywords(*Weapon->Instance, Automatic);

        if (Keywords && Weapon->Data)
        {
            RE::BGSObjectInstanceT<RE::TESObjectWEAP> Object{ Weapon->Base, Weapon->Instance };
            Weapon->Data->SetupFireSounds(*Player, Object);
        }

        REX::INFO("Fire mode: weapon {:08X} -> {}{}", Weapon->Base->GetFormID(), Automatic ? "automatic" : "semi-automatic",
            Keywords ? "" : " (sound and perk keywords from the next load: the instance has no keyword list of its own)");
    }

    void QueueSwitchTask()
    {
        // The fire-mode switch uses MWB's original stack-extra mutation path.
        bool expected = false;
        if (!SwitchTaskQueued.compare_exchange_strong(expected, true))
            return;

        const auto Tasks = F4SE::GetTaskInterface();
        if (!Tasks)
        {
            SwitchTaskQueued = false;
            NotSwitched("F4SE game task interface is unavailable"sv);
            return;
        }

        const auto Generation = SwitchGeneration.load();
        Tasks->AddUITask([Generation]()
        {
            if (Generation != SwitchGeneration.load())
                return;

            const auto Player = RE::PlayerCharacter::GetSingleton();
            if (!Player)
            {
                SwitchTaskQueued = false;
                NotSwitched("player is unavailable on the game task queue"sv);
                return;
            }

            if (const auto Reason = WhyNot(Player))
            {
                SwitchTaskQueued = false;
                NotSwitched(*Reason);
                return;
            }

            Switch(Player);
            SwitchTaskQueued = false;
        });
    }
}

namespace FireMode
{
    void Initialize()
    {
        StartEvent.emplace("MWB_SwitchFireModeStart");
        Annotation.emplace("MWB_SwitchFireMode");
        DoneEvent.emplace("MWB_SwitchFireModeDone");
        ExitEvent.emplace("MWB_SwitchFireModeExit");

        ModeKeywords = { RE::TESForm::GetFormByID<RE::BGSKeyword>(SoundAutoKeywordID),
            RE::TESForm::GetFormByID<RE::BGSKeyword>(WeaponTypeAutomaticID) };

        if (!Settings::FireModeSwitch())
        {
            REX::INFO("Fire mode: off in the INI; configured attach points are not added, so the next load drops the fire mode OMODs");
            return;
        }

        const auto DataHandler = RE::TESDataHandler::GetSingleton();
        std::uint32_t Added = 0;
        std::uint32_t Configured = 0;

        for (const auto Weapon : DataHandler->GetFormArray<RE::TESObjectWEAP>())
        {
            if (!Weapon)
                continue;

            const auto Rule = WeaponConfig::MatchFireMode(Weapon, nullptr);
            if (!Rule || !Rule->AttachPoint)
                continue;

            ++Configured;

            if (!Weapon->attachParents.HasKeyword(Rule->AttachPoint))
            {
                AddAttachParent(Weapon->attachParents, Rule->AttachPoint);
                ++Added;
            }
        }

        REX::INFO("Fire mode: configured OMOD attach points added to {} of {} matching weapons", Added, Configured);
    }

    bool Refuse(const RE::BSFixedString& Event)
    {
        if (!StartEvent || Event != *StartEvent)
            return false;

        const auto Reason = Settings::FireModeSwitch() ? WhyNot(RE::PlayerCharacter::GetSingleton()) :
                                                         std::optional<std::string>{ "the feature is off"s };

        if (Reason)
            REX::INFO("Fire mode: {} refused, {}", Event.c_str(), *Reason);

        return Reason.has_value();
    }

    std::optional<bool> HandleWithoutAnimation(const RE::BSFixedString& Event)
    {
        if (!StartEvent || Event != *StartEvent || !Settings::FireModeSwitch())
            return std::nullopt;

        const auto Player = RE::PlayerCharacter::GetSingleton();
        const auto Weapon = Core::GetEquippedWeapon(Player);

        if (!Weapon)
            return std::nullopt;

        const auto Rule = WeaponConfig::MatchFireMode(Weapon->Base, Weapon->Instance);

        if (!Rule || Settings::FireModeSwitchAnimation())
            return std::nullopt;

        if (const auto Reason = WhyNot(Player))
        {
            REX::INFO("Fire mode: {} refused, {}", Event.c_str(), *Reason);
            return false;
        }

        Pending = false;
        Requested = true;

        if (Settings::VerboseLog())
            REX::INFO("Fire mode: {} accepted without an animation", Event.c_str());

        return true;
    }

    void OnEventSent(const RE::BSFixedString& Event, bool Accepted)
    {
        if (!StartEvent || Event != *StartEvent)
            return;

        if (Accepted)
            Pending = true;

        if (Settings::VerboseLog())
            REX::INFO("Fire mode: {} {}", Event.c_str(), Accepted ? "accepted, waiting for the clip" : "refused by the graph");
    }

    void OnAnimationEvent(const RE::BSFixedString& Tag)
    {
        if (!StartEvent)
            return;

        if (Tag == *Annotation || Tag == *DoneEvent)
        {
            if (Pending.exchange(false))
            {
                Requested = true;

                if (Settings::VerboseLog())
                    REX::INFO("Fire mode: {}, switching", Tag.c_str());
            }
        }
        else if (Tag == *ExitEvent)
        {
            if (Pending.exchange(false))
                REX::INFO("Fire mode: not switched, the clip was cut short");
        }
    }

    void Tick(RE::PlayerCharacter* Player)
    {
        if (Requested.exchange(false) && Player)
            QueueSwitchTask();
    }

    void Reset()
    {
        Pending = false;
        Requested = false;
        SwitchGeneration.fetch_add(1);
        SwitchTaskQueued = false;
    }
}
