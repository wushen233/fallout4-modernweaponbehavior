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

namespace Hooks
{
    enum class TriggerKind : std::uint8_t
    {
        kOther,
        kSemiAutomatic,
        kHoldToPower
    };

    struct AttackTrigger
    {
        TriggerKind Kind{ TriggerKind::kOther };
        float       FullPowerSeconds{ 0.0f };
        bool        Empty{ false };
        bool        Ready{ false };
    };

    using AttackButtonFunction = void (*)(RE::AttackBlockHandler*, const RE::ButtonEvent*);

    void InstallAttackButtonHook();
}
