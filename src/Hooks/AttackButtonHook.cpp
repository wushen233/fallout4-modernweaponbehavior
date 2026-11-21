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

#include "AttackButtonHook.h"
#include "Core/EquippedWeapon.h"
#include "Fire/FusionCharge.h"
#include "Fire/SemiAutoHold.h"
#include "Settings/Settings.h"

namespace
{
    constexpr std::size_t ButtonEventSlot = 0x08;

    [[nodiscard]] std::string_view VtableClassName(std::uintptr_t Vtable)
    {
        struct CompleteObjectLocator
        {
            std::uint32_t Signature;
            std::uint32_t Offset;
            std::uint32_t CdOffset;
            std::uint32_t TypeDescriptor;
        };

        const auto Locator = *reinterpret_cast<const CompleteObjectLocator* const*>(Vtable - sizeof(void*));

        if (!Locator)
            return {};

        const auto Base = REX::FModule::GetExecutingModule().GetBaseAddress();

        return reinterpret_cast<const char*>(Base + Locator->TypeDescriptor + 2 * sizeof(void*));
    }

    [[nodiscard]] bool VatsOpen()
    {
        static const RE::BSFixedString VatsMenu{ "VATSMenu" };
        const auto UI = RE::UI::GetSingleton();

        return UI && UI->GetMenuOpen(VatsMenu);
    }

    [[nodiscard]] Hooks::AttackTrigger Classify(RE::PlayerCharacter* Player)
    {
        Hooks::AttackTrigger Trigger;

        const auto Weapon = Core::GetEquippedWeapon(Player);

        if (!Weapon || !Weapon->Base || Weapon->Base->weaponData.type.get() != RE::WEAPON_TYPE::kGun)
            return Trigger;

        const RE::TESObjectWEAP::InstanceData& Data = Weapon->Instance ? *Weapon->Instance : Weapon->Base->weaponData;

        if (Data.flags.any(RE::WEAPON_FLAGS::kHoldInputToPower))
        {
            Trigger.Kind = Data.fullPowerSeconds > 0.0f ? Hooks::TriggerKind::kHoldToPower : Hooks::TriggerKind::kOther;
            Trigger.FullPowerSeconds = Data.fullPowerSeconds;
        }
        else if (!Data.flags.any(RE::WEAPON_FLAGS::kAutomatic, RE::WEAPON_FLAGS::kChargingAttack))
        {
            Trigger.Kind = Hooks::TriggerKind::kSemiAutomatic;
        }

        Trigger.Empty = Data.ammo && Data.ammoCapacity > 0 && Weapon->Data && Weapon->Data->ammoCount == 0;
        Trigger.Ready = Player->weaponState == RE::WEAPON_STATE::kDrawn && !VatsOpen();

        return Trigger;
    }

    struct AttackButton
    {
        static void Thunk(RE::AttackBlockHandler* This, const RE::ButtonEvent* Event)
        {
            const bool Fusion = Settings::FusionCharge();
            const bool SemiAuto = Settings::SemiAutoHold();

            if (!This || !Event || (!Fusion && !SemiAuto) || Event->QUserEvent() != "PrimaryAttack"sv)
            {
                Original(This, Event);
                return;
            }

            const auto Player = RE::PlayerCharacter::GetSingleton();
            const auto Trigger = Player ? Classify(Player) : Hooks::AttackTrigger{};

            if (Fusion && Trigger.Kind == Hooks::TriggerKind::kHoldToPower)
            {
                FusionCharge::OnButton(*This, *Event, Trigger, Forward);
                return;
            }

            if (SemiAuto && Trigger.Kind == Hooks::TriggerKind::kSemiAutomatic)
                SemiAutoHold::BeforeButton(*This, *Event, Trigger);

            Original(This, Event);
        }

        static void Forward(RE::AttackBlockHandler* This, const RE::ButtonEvent* Event)
        {
            Original(This, Event);
        }

        inline static REL::Relocation<decltype(Thunk)> Original;
    };
}

namespace Hooks
{
    void InstallAttackButtonHook()
    {
        REL::Relocation<std::uintptr_t> Vtable{ RE::VTABLE::AttackBlockHandler[0] };
        const auto Controls = RE::PlayerControls::GetSingleton();
        const auto Live = Controls && Controls->attackHandler ? *reinterpret_cast<std::uintptr_t*>(Controls->attackHandler) : 0;
        const auto Name = VtableClassName(Vtable.address());

        const bool Known = Name == ".?AVAttackBlockHandler@@"sv || Name == ".?AUAttackBlockHandler@@"sv;

        if (!Known || Live != Vtable.address())
        {
            REX::ERROR("Attack button: AttackBlockHandler vtable mismatch (id {:X} '{}', live {:X}). Fusion charge and semi-automatic hold disabled.",
                Vtable.address(), Name, Live);
            return;
        }

        AttackButton::Original = Vtable.write_vfunc(ButtonEventSlot, AttackButton::Thunk);
        REX::INFO("Attack button hook installed");
    }
}
