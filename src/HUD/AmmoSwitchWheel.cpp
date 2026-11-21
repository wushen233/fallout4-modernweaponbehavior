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

#include "AmmoSwitchWheel.h"

#include "Core/OModSwitch.h"

#include <F4SE/API.h>

#include <atomic>
#include <mutex>

namespace
{
    using Value = Scaleform::GFx::Value;

    constexpr auto Movie = "MWB_AmmoSwitchWheel";
    constexpr auto MenuName = "MWBAmmoSwitchWheelMenu"sv;

    std::mutex OptionsLock;
    std::vector<OModSwitch::AmmoSwitchOption> PendingOptions;
    std::atomic<std::uint64_t> WheelGeneration{ 0 };
    std::atomic<std::uint64_t> StickGeneration{ 0 };
    std::atomic<float> StickX{ 0.0f };
    std::atomic<float> StickY{ 0.0f };

    class AmmoSwitchMenu : public RE::IMenu
    {
    public:
        AmmoSwitchMenu()
        {
            {
                const std::scoped_lock lock{ OptionsLock };
                options = PendingOptions;
            }
            menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving);
            menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
            menuFlags.set(RE::UI_MENU_FLAGS::kDontHideCursorWhenTopmost);
            depthPriority = RE::UI_DEPTH_PRIORITY::kHUD;
            inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kNone;

            const auto scaleform = RE::BSScaleformManager::GetSingleton();
            if (!scaleform || !scaleform->LoadMovie(*this, uiMovie, Movie, "root1") || !uiMovie ||
                !menuObj.IsObject() || !menuObj.HasMember("SetOptions"))
            {
                REX::WARN("Ammo switch wheel: could not load Interface\\{}.swf", Movie);
                return;
            }

            Value names;
            Value counts;
            uiMovie->CreateArray(&names);
            uiMovie->CreateArray(&counts);
            std::int32_t currentIndex = -1;
            for (std::size_t i = 0; i < options.size(); ++i)
            {
                names.PushBack(Value{ options[i].Name.c_str() });
                counts.PushBack(Value{ static_cast<double>(options[i].Count) });
                if (options[i].Current)
                    currentIndex = static_cast<std::int32_t>(i);
            }

            selectedIndex.store(currentIndex, std::memory_order_relaxed);
            menuObj.Invoke("SetOptions", std::array<Value, 3>{ std::move(names), std::move(counts), Value{ currentIndex } });
            REX::INFO("Ammo switch wheel: loaded {} dynamic ammo sectors", options.size());
        }

        ~AmmoSwitchMenu() override
        {
            if (uiMovie)
            {
                uiMovie->SetCaptureThread(Scaleform::GetCurrentThreadId());
                if (const auto heap = uiMovie->GetHeap())
                    heap->AssignToCurrentThread();
            }
            menuObj = nullptr;
        }

        void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
        {
            IMenu::AdvanceMovie(a_timeDelta, a_time);

            if (!uiMovie || !menuObj.IsObject())
                return;

            const auto generation = StickGeneration.load(std::memory_order_relaxed);
            if (generation != lastStickGeneration)
            {
                lastStickGeneration = generation;
                menuObj.Invoke("SetStickDirection", std::array<Value, 2>{
                    Value{ static_cast<double>(StickX.load(std::memory_order_relaxed)) },
                    Value{ static_cast<double>(StickY.load(std::memory_order_relaxed)) } });
            }

            Value selected;
            if (menuObj.Invoke("GetSelectedIndex", &selected) &&
                (selected.GetType() == Value::ValueType::kNumber || selected.GetType() == Value::ValueType::kInt))
                selectedIndex.store(static_cast<std::int32_t>(selected.GetNumber()), std::memory_order_relaxed);
        }

        [[nodiscard]] RE::TESAmmo* SelectedAmmo()
        {
            const auto index = selectedIndex.load(std::memory_order_relaxed);
            if (index < 0 || static_cast<std::size_t>(index) >= options.size())
                return nullptr;
            return options[static_cast<std::size_t>(index)].Ammo;
        }

    private:
        std::vector<OModSwitch::AmmoSwitchOption> options;
        std::atomic<std::int32_t> selectedIndex{ -1 };
        std::uint64_t lastStickGeneration{ 0 };
    };

    [[nodiscard]] AmmoSwitchMenu* CurrentMenu()
    {
        const auto ui = RE::UI::GetSingleton();
        if (!ui)
            return nullptr;
        const auto menu = ui->GetMenu(MenuName);
        return menu ? static_cast<AmmoSwitchMenu*>(menu.get()) : nullptr;
    }

    void HideMenu()
    {
        if (const auto queue = RE::UIMessageQueue::GetSingleton())
            queue->AddMessage(MenuName, RE::UI_MESSAGE_TYPE::kHide);
    }
}

namespace AmmoSwitchWheel
{
    void Initialize()
    {
        if (const auto ui = RE::UI::GetSingleton())
            ui->RegisterMenu(MenuName.data(), [](const RE::UIMessage&) -> RE::IMenu* { return new AmmoSwitchMenu(); });
    }

    bool Open()
    {
        if (IsOpen())
            return true;

        const auto* tasks = F4SE::GetTaskInterface();
        if (!tasks)
            return false;

        const auto generation = WheelGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
        StickX.store(0.0f, std::memory_order_relaxed);
        StickY.store(0.0f, std::memory_order_relaxed);
        StickGeneration.fetch_add(1, std::memory_order_relaxed);
        tasks->AddTask([generation]() {
            if (generation != WheelGeneration.load(std::memory_order_relaxed))
                return;

            auto options = OModSwitch::GetEquippedAmmoSwitchOptions();
            if (options.size() < 2)
                return;

            {
                const std::scoped_lock lock{ OptionsLock };
                PendingOptions = std::move(options);
            }

            if (const auto queue = RE::UIMessageQueue::GetSingleton())
                queue->AddMessage(MenuName, RE::UI_MESSAGE_TYPE::kShow);
        });
        return true;
    }

    bool IsOpen()
    {
        const auto ui = RE::UI::GetSingleton();
        return ui && ui->GetMenuOpen(MenuName);
    }

    void SetStickDirection(float a_x, float a_y)
    {
        StickX.store(a_x, std::memory_order_relaxed);
        StickY.store(a_y, std::memory_order_relaxed);
        StickGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    bool Confirm()
    {
        auto* menu = CurrentMenu();
        auto* ammo = menu ? menu->SelectedAmmo() : nullptr;
        HideMenu();
        return ammo && OModSwitch::SwitchEquippedAmmoTo(ammo);
    }

    void Cancel()
    {
        WheelGeneration.fetch_add(1, std::memory_order_relaxed);
        HideMenu();
    }
}
