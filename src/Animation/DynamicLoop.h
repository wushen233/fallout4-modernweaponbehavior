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

namespace DynamicLoop
{
    [[nodiscard]] bool Tracking();

    void Tick();

    void OnClipActivated(const void* Clip, const void* Character);

    [[nodiscard]] float BeforeUpdate(void* Clip, float Delta);

    void AfterUpdate(void* Clip);

    void OnClipDeactivated(const void* Clip);

    [[nodiscard]] std::uint32_t End(RE::Actor* Actor);

    [[nodiscard]] std::uint32_t Release(RE::Actor* Actor);

    [[nodiscard]] bool Looping(RE::Actor* Actor);

    void Reset();
}
