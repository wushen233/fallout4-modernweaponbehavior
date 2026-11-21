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

#include "SprintCamera.h"
#include "Settings/Settings.h"

namespace
{
    constexpr std::uint16_t SprintingBit = 0x100;

    [[nodiscard]] bool Sprinting(const RE::PlayerCharacter* Player)
    {
        return (static_cast<std::uint16_t>(Player->moveMode) & SprintingBit) != 0;
    }

    void LeaveSprintCamera(RE::PlayerCharacter* Player)
    {
        static const RE::BSFixedString StopEvent{ "CameraSprintStop" };

        if (Player->NotifyAnimationGraphImpl(StopEvent) && Settings::VerboseLog())
            REX::INFO("Sprint camera: the third person graph was in the sprint camera with it turned off, CameraSprintStop sent");
    }
}

namespace SprintCamera
{
    void Tick(RE::PlayerCharacter* Player)
    {
        if (!Player || !Player->Is3rdPersonVisible())
            return;

        static const RE::BSFixedString Variable{ "iMWB_SprintCamera" };
        const bool         Enabled = Settings::ThirdPersonSprintFOV();
        const std::int32_t Wanted = Enabled ? 1 : 0;

        std::int32_t Current = 0;

        if (!Player->GetGraphVariableImplInt(Variable, Current))
            return;

        if (Current != Wanted)
        {
            Player->SetGraphVariableInt(Variable, Wanted);

            if (Settings::VerboseLog())
                REX::INFO("Sprint camera: iMWB_SprintCamera {} -> {}", Current, Wanted);
        }

        if (!Enabled && Sprinting(Player))
            LeaveSprintCamera(Player);
    }
}
