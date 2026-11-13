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

#include "InputHandler.h"
#include "Core/OModSwitch.h"
#include "Melee/HoldBlock.h"
#include "Reload/LoopReload.h"
#include "Reload/SightedReload.h"
#include "Weapon/ScopeSway.h"

#include <F4SE/InputMap.h>
#include <glaze/json.hpp>

#include <RE/U/UI.h>

#include <Windows.h>
#undef ERROR

#include <fstream>

struct McmKeybind
{
    std::string Id;
    std::string ModName;
    std::uint32_t Keycode = 0;
    std::uint32_t Modifiers = 0;
};

struct McmKeybindFile
{
    std::uint32_t Version = 0;
    std::vector<McmKeybind> Keybinds;
};

template <>
struct glz::meta<McmKeybind>
{
    using T = McmKeybind;
    static constexpr auto value = glz::object("id", &T::Id, "modName", &T::ModName, "keycode", &T::Keycode, "modifiers", &T::Modifiers);
};

template <>
struct glz::meta<McmKeybindFile>
{
    using T = McmKeybindFile;
    static constexpr auto value = glz::object("version", &T::Version, "keybinds", &T::Keybinds);
};

namespace
{
    struct OModHotkey
    {
        std::string Id;
        std::uint32_t Keycode;
        std::uint32_t Modifiers;
        WeaponConfig::OModCategory Category;
    };

    std::vector<OModHotkey> OModHotkeys;

    [[nodiscard]] std::uint32_t ReadModifiers()
    {
        std::uint32_t modifiers = 0;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            modifiers |= 0x1;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            modifiers |= 0x2;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)
            modifiers |= 0x4;
        return modifiers;
    }

    [[nodiscard]] std::optional<std::uint32_t> GetMcmKeycode(const RE::ButtonEvent& Event)
    {
        switch (Event.device.get())
        {
        case RE::INPUT_DEVICE::kKeyboard:
            return Event.QIDCode();
        case RE::INPUT_DEVICE::kMouse:
            return F4SE::InputMap::kMacro_MouseButtonOffset + Event.QIDCode();
        case RE::INPUT_DEVICE::kGamepad:
            return F4SE::InputMap::GamepadMaskToKeycode(Event.QIDCode());
        default:
            return std::nullopt;
        }
    }

    void DispatchOModHotkeys(const RE::ButtonEvent& Event)
    {
        if (const auto UI = RE::UI::GetSingleton(); UI && UI->GameIsPaused())
            return;

        const auto keycode = GetMcmKeycode(Event);
        if (!keycode || *keycode == 0)
            return;

        const auto modifiers = ReadModifiers();
        for (const auto& hotkey : OModHotkeys)
        {
            if (hotkey.Keycode == *keycode && hotkey.Modifiers == modifiers)
            {
                REX::INFO("OMod switch: C++ hotkey '{}' received", hotkey.Id);
                OModSwitch::CycleEquippedOMod(hotkey.Category);
            }
        }
    }

    class StopRequestHandler :
        public RE::PlayerInputHandler
    {
    public:
        explicit StopRequestHandler(RE::PlayerControlsData& Data) :
            PlayerInputHandler(Data)
        {
        }

        bool ShouldHandleEvent(const RE::InputEvent* Event) override
        {
            return Event && *Event->eventType == RE::INPUT_EVENT_TYPE::kButton;
        }

        void OnButtonEvent(const RE::ButtonEvent* Event) override
        {
            if (!Event)
                return;

            const auto& UserEvent = Event->QUserEvent();

            if (UserEvent == "SecondaryAttack"sv)
            {
                SightedReload::SetAimButton(Event->value != 0.0f);
                HoldBlock::SetBlockButton(Event->value != 0.0f);
            }

            ScopeSway::OnButton(*Event);

            if (!Event->QJustPressed())
                return;

            DispatchOModHotkeys(*Event);

            if (UserEvent == "PrimaryAttack"sv || UserEvent == "ReadyWeapon"sv)
                Reload::RequestStop();
        }
    };
}

namespace Hooks
{
    void ReloadOModKeybinds()
    {
        constexpr auto path = "Data/MCM/Settings/Keybinds.json";
        std::ifstream file{ path, std::ios::binary };
        if (!file)
        {
            OModHotkeys.clear();
            REX::INFO("OMod switch: MCM keybind file unavailable; direct hotkeys disabled");
            return;
        }

        const std::string buffer{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
        McmKeybindFile parsed;
        constexpr glz::opts options{ .error_on_unknown_keys = false };
        if (const auto error = glz::read<options>(parsed, buffer))
        {
            OModHotkeys.clear();
            REX::WARN("OMod switch: could not read MCM keybinds: {}", glz::format_error(error, buffer));
            return;
        }
        if (parsed.Version < 1)
        {
            OModHotkeys.clear();
            REX::WARN("OMod switch: unsupported MCM keybind storage version {}", parsed.Version);
            return;
        }

        std::vector<OModHotkey> loaded;
        for (const auto& keybind : parsed.Keybinds)
        {
            if (keybind.ModName != "ModernWeaponBehavior" || keybind.Keycode == 0)
                continue;

            std::optional<WeaponConfig::OModCategory> category;
            if (keybind.Id == "MWBMCMHKSwitchMuzzleOMod")
                category = WeaponConfig::OModCategory::Muzzle;
            else if (keybind.Id == "MWBMCMHKSwitchSightOMod")
                category = WeaponConfig::OModCategory::Sight;
            else if (keybind.Id == "MWBMCMHKSwitchAmmunitionOMod")
                category = WeaponConfig::OModCategory::Ammunition;
            else if (keybind.Id == "MWBMCMHKSwitchUnderbarrelOMod")
                category = WeaponConfig::OModCategory::Underbarrel;

            if (category)
                loaded.push_back({ keybind.Id, keybind.Keycode, keybind.Modifiers & 0x7, *category });
        }

        OModHotkeys = std::move(loaded);
        REX::INFO("OMod switch: loaded {} direct C++ MCM category hotkeys", OModHotkeys.size());
    }

    void InstallInputHandler()
    {
        ReloadOModKeybinds();

        const auto Controls = RE::PlayerControls::GetSingleton();

        if (!Controls)
        {
            REX::ERROR("PlayerControls not available; stopping a loop early is disabled.");
            return;
        }

        Controls->handlers.insert(Controls->handlers.begin(), new StopRequestHandler(Controls->data));
        REX::INFO("Input handler installed");
    }
}
