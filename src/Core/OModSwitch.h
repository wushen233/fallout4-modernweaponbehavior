/*
    This file is part of Modern Weapon Behavior.
    Modern Weapon Behavior is free software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.
    This file is additionally covered by the Modding Exception and GPL-3.0
    Linking Exception with Corresponding Source described in EXCEPTIONS.
*/

#pragma once

#include <cstdint>
#include <variant>

#include <RE/Fallout.h>
#include <RE/B/BGSMod.h>

#include "Config/WeaponConfig.h"

namespace RE::BSScript
{
    class IVirtualMachine;
}

namespace OModSwitch
{
    bool CycleEquippedOMod(WeaponConfig::OModCategory a_category);
    bool CycleEquippedOMod(std::string_view a_keyID);
    bool SwitchEquippedAmmoFromPipboy(RE::TESAmmo* a_targetAmmo);
    void InstallPipboyAmmoMarkerSync();
    void Initialize();
    void TraceAmmoAfterLoad();
    void ResetPendingSwitches();
    void ResetPendingAmmoReload();
    void TickPendingAmmoReload();

    bool SetItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_item,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_omod,
        bool a_attach);

    bool HasItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_item,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_omod);

    std::int32_t GetEquippedStackIndex(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_item);

    bool ReplaceItemOMod(
        std::monostate,
        RE::TESObjectREFR* a_owner,
        RE::TESBoundObject* a_item,
        std::int32_t a_stackIndex,
        RE::BGSMod::Attachment::Mod* a_fromOMod,
        RE::BGSMod::Attachment::Mod* a_toOMod);

    bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm);
}
