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

#include "SemiAutoHold.h"
#include "Settings/Settings.h"

namespace
{
    constexpr float RepeatDelaySeconds = 0.25f;

    bool Logged{ false };
    bool QueuedByUs{ false };

    void DropOwnQueue(RE::AttackBlockHandler& Handler)
    {
        if (!std::exchange(QueuedByUs, false) || !Handler.rightAttackQueued)
            return;

        Handler.rightAttackQueued = false;

        if (Settings::VerboseLog())
            REX::INFO("Semi-automatic hold: trigger released before the next shot, queued shot dropped");
    }
}

namespace SemiAutoHold
{
    void BeforeButton(RE::AttackBlockHandler& Handler, const RE::ButtonEvent& Event, const Hooks::AttackTrigger& Trigger)
    {
        if (Event.QJustPressed())
        {
            Logged = false;
            QueuedByUs = false;
            return;
        }

        if (Event.value == 0.0f)
        {
            Logged = false;
            DropOwnQueue(Handler);
            return;
        }

        if (Event.heldDownSecs < RepeatDelaySeconds || Handler.rightAttackQueued || !Trigger.Ready || Trigger.Empty)
            return;

        Handler.rightAttackQueued = true;
        QueuedByUs = true;

        if (!Logged && Settings::VerboseLog())
            REX::INFO("Semi-automatic hold: trigger held, queuing a shot each frame as a new press would");

        Logged = true;
    }
}
