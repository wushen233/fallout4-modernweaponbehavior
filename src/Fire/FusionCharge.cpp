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

#include "FusionCharge.h"
#include "Core/GunState.h"
#include "Settings/Settings.h"

extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64();

namespace
{
    constexpr float MillisecondsToSeconds = 0.001f;

    constexpr std::array SightedCancelEvents{ "attackReleaseCharge"sv, "rifleSightedStart"sv };
    constexpr std::array ReadyCancelEvents{ "attackInterrupt"sv, "rifleSightedEnd"sv };
    constexpr std::array SwallowedEvents{ "attackReleaseChargingHold"sv, "attackRelease"sv };

    std::atomic<bool> ChargeAccepted{ false };
    std::atomic<bool> SwallowRelease{ false };
    bool              AutoFired{ false };
    bool              Restarting{ false };
    bool              QueuedByUs{ false };

    void DropOwnQueue(RE::AttackBlockHandler& Handler)
    {
        if (!std::exchange(QueuedByUs, false) || !Handler.rightAttackQueued)
            return;

        Handler.rightAttackQueued = false;

        if (Settings::VerboseLog())
            REX::INFO("Fusion charge: trigger released before the weapon took the next charge, queued attack dropped");
    }

    [[nodiscard]] bool TagIs(const RE::BSFixedString& Tag, std::string_view Name)
    {
        const std::string_view Text{ Tag.c_str() };

        return Text.size() == Name.size() && _strnicmp(Text.data(), Name.data(), Text.size()) == 0;
    }

    [[nodiscard]] bool Charging(const RE::AttackBlockHandler& Handler)
    {
        return ChargeAccepted.load() && Handler.rightAttackLastHeldSeconds < 0.0f;
    }

    [[nodiscard]] float ChargeSeconds(const RE::AttackBlockHandler& Handler)
    {
        return static_cast<float>(GetTickCount64() - Handler.rightAttackTimestamp) * MillisecondsToSeconds;
    }

    void Retarget(RE::ButtonEvent& Copy, const RE::ButtonEvent& From, float Value, float Held)
    {
        *reinterpret_cast<std::uintptr_t*>(std::addressof(Copy)) = *reinterpret_cast<const std::uintptr_t*>(std::addressof(From));
        Copy.value = Value;
        Copy.heldDownSecs = Held;
    }

    void ForwardSwallowed(RE::AttackBlockHandler& Handler, const RE::ButtonEvent& Event, Hooks::AttackButtonFunction Forward)
    {
        SwallowRelease = true;
        Forward(&Handler, &Event);
        SwallowRelease = false;
    }

    void Cancel(RE::PlayerCharacter* Player, float Share)
    {
        if (!Player)
            return;

        const auto State = Core::GetGunState(Player);
        const bool Sighted = State == RE::GUN_STATE::kSighted || State == RE::GUN_STATE::kFireSighted;

        for (const auto Name : Sighted ? SightedCancelEvents : ReadyCancelEvents)
        {
            if (Player->NotifyAnimationGraphImpl(RE::BSFixedString{ Name }))
            {
                if (Settings::VerboseLog())
                    REX::INFO("Fusion charge: released at {:.0f}%, charge cancelled with {} ({})", Share * 100.0f, Name, Sighted ? "aiming" : "from the hip");

                return;
            }
        }

        REX::WARN("Fusion charge: released at {:.0f}%, but the graph refused every cancel event ({}); the weapon may stay charging until the next shot",
            Share * 100.0f, Sighted ? "aiming" : "from the hip");
    }

    void OnHeld(RE::AttackBlockHandler& Handler, const RE::ButtonEvent& Event, const Hooks::AttackTrigger& Trigger, Hooks::AttackButtonFunction Forward)
    {
        if (Charging(Handler))
        {
            Restarting = false;
            QueuedByUs = false;

            const float Seconds = ChargeSeconds(Handler);

            if (Seconds >= Trigger.FullPowerSeconds)
            {
                RE::ButtonEvent Release{ Event };
                Retarget(Release, Event, 0.0f, Event.heldDownSecs);
                ChargeAccepted = false;
                Forward(&Handler, &Release);
                AutoFired = true;

                if (Settings::VerboseLog())
                    REX::INFO("Fusion charge: full after {:.2f}s (full power {:.2f}s), fired", Seconds, Trigger.FullPowerSeconds);

                Forward(&Handler, &Event);
                return;
            }

            RE::ButtonEvent Held{ Event };
            Retarget(Held, Event, Event.value, Seconds);
            Forward(&Handler, &Held);
            return;
        }

        if (Trigger.Ready && !Trigger.Empty && !Handler.rightAttackQueued)
        {
            Handler.rightAttackQueued = true;
            QueuedByUs = true;

            if (!Restarting && Settings::VerboseLog())
                REX::INFO("Fusion charge: trigger still held, charging again as soon as the weapon allows");

            Restarting = true;
        }

        Forward(&Handler, &Event);
    }

    void OnRelease(RE::AttackBlockHandler& Handler, const RE::ButtonEvent& Event, const Hooks::AttackTrigger& Trigger, Hooks::AttackButtonFunction Forward)
    {
        Restarting = false;
        DropOwnQueue(Handler);

        if (Charging(Handler))
        {
            const float Seconds = ChargeSeconds(Handler);
            ChargeAccepted = false;
            AutoFired = false;

            if (Seconds >= Trigger.FullPowerSeconds)
            {
                Forward(&Handler, &Event);
                return;
            }

            ForwardSwallowed(Handler, Event, Forward);
            Cancel(RE::PlayerCharacter::GetSingleton(), Seconds / Trigger.FullPowerSeconds);
            return;
        }

        if (AutoFired)
        {
            AutoFired = false;
            ForwardSwallowed(Handler, Event, Forward);

            if (Settings::VerboseLog())
                REX::INFO("Fusion charge: trigger released after an automatic shot, release kept from the graph");

            return;
        }

        Forward(&Handler, &Event);
    }
}

namespace FusionCharge
{
    void OnButton(RE::AttackBlockHandler& Handler, const RE::ButtonEvent& Event, const Hooks::AttackTrigger& Trigger, Hooks::AttackButtonFunction Forward)
    {
        if (Event.QJustPressed())
        {
            AutoFired = false;
            Restarting = false;
            QueuedByUs = false;
            Forward(&Handler, &Event);
            return;
        }

        if (Event.value != 0.0f)
            OnHeld(Handler, Event, Trigger, Forward);
        else
            OnRelease(Handler, Event, Trigger, Forward);
    }

    bool Refuse(const RE::BSFixedString& Event)
    {
        if (!SwallowRelease.load())
            return false;

        if (std::ranges::none_of(SwallowedEvents, [&](std::string_view Name) { return TagIs(Event, Name); }))
            return false;

        if (Settings::VerboseLog())
            REX::INFO("Fusion charge: {} kept from the graph", Event.c_str());

        return true;
    }

    void OnEventSent(const RE::BSFixedString& Event, bool Accepted)
    {
        if (Accepted && TagIs(Event, "attackStartChargingHold"sv))
            ChargeAccepted = true;
    }

    void Reset()
    {
        ChargeAccepted = false;
        SwallowRelease = false;
        AutoFired = false;
        Restarting = false;
        QueuedByUs = false;
    }
}
