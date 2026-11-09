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

#pragma once

#include <RE/M/MeleeThrowHandler.h>
#include <RE/P/PlayerControls.h>

namespace Core
{
    [[nodiscard]] inline RE::GUN_STATE GetGunState(const RE::Actor* Actor)
    {
        constexpr std::int32_t GunStateMask = 0xF;

        return static_cast<RE::GUN_STATE>(std::to_underlying(Actor->gunState) & GunStateMask);
    }

    [[nodiscard]] inline bool IsThirdPersonReadyStance(const RE::PlayerCharacter* Player, RE::GUN_STATE State)
    {
        if (!Player->Is3rdPersonVisible() || !Player->GetWeaponMagicDrawn())
            return false;
        if (State == RE::GUN_STATE::kRelaxed)
            return true;
        if (State != RE::GUN_STATE::kThrowing)
            return false;

        // The third-person graph also reports state 5 when ready after ADS or fire.
        const auto Controls = RE::PlayerControls::GetSingleton();
        const auto Throw = Controls ? Controls->meleeThrowHandler : nullptr;
        return Throw && !Throw->heldStateActive && !Throw->queueThrow;
    }
}
