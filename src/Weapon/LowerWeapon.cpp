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

#include "LowerWeapon.h"
#include "Actions/FirstPerson.h"
#include "Core/GunState.h"
#include "Settings/Settings.h"

namespace
{
    constexpr RE::GUN_STATE GunDownState = RE::GUN_STATE::kBlocked;

    const RE::BSFixedString& GunDownEvent()
    {
        static const RE::BSFixedString Event{ "GunDown" };
        return Event;
    }

    const RE::BSFixedString& GunUpEvent()
    {
        static const RE::BSFixedString Event{ "GunUp" };
        return Event;
    }

    bool Refused(std::string_view Reason)
    {
        if (Settings::VerboseLog())
            REX::INFO("Lower weapon: refused, {}", Reason);

        return false;
    }
}

namespace LowerWeapon
{
    bool Toggle()
    {
        const auto Player = RE::PlayerCharacter::GetSingleton();

        if (!Player)
            return false;

        if (Player->Is3rdPersonVisible())
            return Refused("third person"sv);

        if (!Player->GetWeaponMagicDrawn())
            return Refused("no drawn weapon"sv);

        const bool Lowered = Core::GetGunState(Player) == GunDownState;

        if (!Lowered)
        {
            if (const auto Busy = FirstPerson::WeaponBusy(Player))
                return Refused(*Busy);
        }

        const auto& Event = Lowered ? GunUpEvent() : GunDownEvent();
        const auto& Other = Lowered ? GunDownEvent() : GunUpEvent();
        bool        Accepted = Player->NotifyAnimationGraphImpl(Event);

        if (Settings::VerboseLog())
            REX::INFO("Lower weapon: {} {} (gun state {})", Event.c_str(), Accepted ? "accepted" : "refused", std::to_underlying(Core::GetGunState(Player)));

        if (!Accepted && Lowered)
        {
            Accepted = Player->NotifyAnimationGraphImpl(Other);

            if (Settings::VerboseLog())
                REX::INFO("Lower weapon: {} {}", Other.c_str(), Accepted ? "accepted" : "refused");
        }

        return Accepted;
    }
}
