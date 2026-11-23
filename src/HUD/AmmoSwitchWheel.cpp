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

#include <RE/B/BSScaleformManager.h>
#include <RE/G/GameMenuBase.h>
#include <RE/H/HUDColorTypes.h>

#include <atomic>
#include <mutex>

namespace
{
    using Value = Scaleform::GFx::Value;

    constexpr auto Movie = "MWB_AmmoSwitchWheel";
    constexpr auto MenuName = "MWBAmmoSwitchWheelMenu"sv;

    enum class WheelState : std::uint8_t
    {
        Closed,
        Opening,
        Open,
        Closing
    };

    std::mutex OptionsLock;
    std::vector<OModSwitch::AmmoSwitchOption> PendingOptions;
    std::atomic<WheelState> CurrentWheelState{ WheelState::Closed };
    std::atomic<std::uint64_t> WheelGeneration{ 0 };
    std::atomic<std::uint64_t> StickGeneration{ 0 };
    std::atomic<float> StickX{ 0.0f };
    std::atomic<float> StickY{ 0.0f };
    Scaleform::Ptr<RE::IMenu> HiddenHUD;
    Scaleform::Ptr<Scaleform::GFx::Movie> HiddenHUDMovie;
    bool HUDMenuWasVisible{};
    bool HUDMovieWasVisible{};

    void HideHUD()
    {
        const auto ui = RE::UI::GetSingleton();
        HiddenHUD = ui ? ui->GetMenu("HUDMenu") : nullptr;
        if (!HiddenHUD)
            return;

        HUDMenuWasVisible = HiddenHUD->menuCanBeVisible;
        HiddenHUDMovie = HiddenHUD->uiMovie;
        HUDMovieWasVisible = HiddenHUDMovie && HiddenHUDMovie->GetVisible();
        HiddenHUD->menuCanBeVisible = false;
        if (HiddenHUDMovie)
            HiddenHUDMovie->SetVisible(false);
    }

    void RestoreHUD()
    {
        if (HiddenHUD)
            HiddenHUD->menuCanBeVisible = HUDMenuWasVisible;
        if (HiddenHUDMovie)
            HiddenHUDMovie->SetVisible(HUDMovieWasVisible);
        HiddenHUDMovie = nullptr;
        HiddenHUD = nullptr;
    }

    class AmmoSwitchMenu : public RE::GameMenuBase
    {
    public:
        AmmoSwitchMenu()
        {
            {
                const std::scoped_lock lock{ OptionsLock };
                options = PendingOptions;
            }
            menuFlags.set(RE::UI_MENU_FLAGS::kAllowSaving);
            menuFlags.set(RE::UI_MENU_FLAGS::kUsesMenuContext);
            menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
            // Match the working Favorites wheel. HUD-priority menus draw, but
            // do not reliably enter the native cursor/menu input route.
            depthPriority = RE::UI_DEPTH_PRIORITY::kStandard;
            inputContext = RE::UserEvents::INPUT_CONTEXT_ID::kQuickContainerMenu;

            const auto scaleform = RE::BSScaleformManager::GetSingleton();
            if (scaleform)
            {
                if (const auto translator = scaleform->GetTranslator())
                    translator->AddTranslationsMod("Modern Weapon Behavior");
            }
            if (!scaleform || !scaleform->LoadMovie(*this, uiMovie, Movie, "root1") || !uiMovie ||
                !menuObj.IsObject() || !menuObj.HasMember("SetOptions"))
            {
                REX::WARN("Ammo switch wheel: could not load Interface\\{}.swf", Movie);
                return;
            }

            if (filterHolder)
            {
                Value hintSource;
                if (filterHolder->GetMember("AmmoWheelHintSource", &hintSource) && hintSource.IsDisplayObject())
                {
                    SetUpButtonBar(*filterHolder, "AmmoWheelHintSource", RE::HUDColorTypes::kGameplayHUDColor);
                    REX::INFO("Ammo switch wheel: registered native global button bar hints");
                }
                else
                {
                    REX::WARN("Ammo switch wheel: native global button bar hint source is missing");
                }
            }

            Value names;
            Value counts;
            Value icons;
            Value damages;
            uiMovie->CreateArray(&names);
            uiMovie->CreateArray(&counts);
            uiMovie->CreateArray(&icons);
            uiMovie->CreateArray(&damages);
            std::int32_t currentIndex = -1;
            for (std::size_t i = 0; i < options.size(); ++i)
            {
                names.PushBack(Value{ options[i].Name.c_str() });
                counts.PushBack(Value{ static_cast<double>(options[i].Count) });
                icons.PushBack(Value{ options[i].Icon.c_str() });
                damages.PushBack(Value{ options[i].DamageInfo.c_str() });
                if (options[i].Current)
                    currentIndex = static_cast<std::int32_t>(i);
            }

            selectedIndex.store(currentIndex, std::memory_order_relaxed);
            const auto setOptionsSucceeded = menuObj.Invoke("SetOptions", std::array<Value, 5>{ std::move(names), std::move(counts), std::move(icons), std::move(damages), Value{ currentIndex } });
            REX::INFO("Ammo switch wheel: passed {} dynamic ammo sectors to Scaleform (GFx invoke return={})", options.size(), setOptionsSucceeded);
            optionDiagnosticsPending = true;
        }

        ~AmmoSwitchMenu() override
        {
            RestoreGameplayHandlers();
            if (uiMovie)
            {
                uiMovie->SetCaptureThread(Scaleform::GetCurrentThreadId());
                if (const auto heap = uiMovie->GetHeap())
                    heap->AssignToCurrentThread();
            }
            menuObj = nullptr;
        }

        void OnAddedToMenuStack() override
        {
            HideHUD();
            GameMenuBase::OnAddedToMenuStack();
            CurrentWheelState.store(WheelState::Open, std::memory_order_release);
            SetGameplayHandlersEnabled(false);
        }

        void OnRemovedFromMenuStack() override
        {
            RestoreGameplayHandlers();
            GameMenuBase::OnRemovedFromMenuStack();
            RestoreHUD();
            CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
        }

        void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
        {
            GameMenuBase::AdvanceMovie(a_timeDelta, a_time);

            if (!uiMovie || !menuObj.IsObject())
                return;

            if (optionDiagnosticsPending)
            {
                optionDiagnosticsPending = false;
                Value debug;
                if (menuObj.Invoke("DebugInfo", &debug) && debug.IsString())
                    REX::INFO("Ammo switch wheel: Scaleform {}", debug.GetString());
                else
                    REX::WARN("Ammo switch wheel: Scaleform DebugInfo call failed");
            }

            const auto generation = StickGeneration.load(std::memory_order_relaxed);
            if (generation != lastStickGeneration)
            {
                lastStickGeneration = generation;
                menuObj.Invoke("SetStickDirection", std::array<Value, 2>{
                    Value{ static_cast<double>(StickX.load(std::memory_order_relaxed)) },
                    Value{ static_cast<double>(StickY.load(std::memory_order_relaxed)) } });
            }

            Value selected;
            if (menuObj.Invoke("GetSelectedIndex", &selected))
            {
                std::optional<std::int32_t> index;
                switch (selected.GetType())
                {
                case Value::ValueType::kInt:
                    index = selected.GetInt();
                    break;
                case Value::ValueType::kUInt:
                    index = static_cast<std::int32_t>(selected.GetUInt());
                    break;
                case Value::ValueType::kNumber:
                    index = static_cast<std::int32_t>(selected.GetNumber());
                    break;
                default:
                    break;
                }

                if (index && *index >= 0 && static_cast<std::size_t>(*index) < options.size())
                {
                    const auto previous = selectedIndex.exchange(*index, std::memory_order_relaxed);
                    if (previous != *index)
                        REX::INFO("Ammo switch wheel: selection index {} -> {}", previous, *index);
                }
            }
        }

        [[nodiscard]] RE::TESAmmo* SelectedAmmo()
        {
            const auto index = SelectedIndex();
            if (index < 0 || static_cast<std::size_t>(index) >= options.size())
                return nullptr;
            return options[static_cast<std::size_t>(index)].Ammo;
        }

        [[nodiscard]] std::int32_t SelectedIndex() const
        {
            return selectedIndex.load(std::memory_order_relaxed);
        }

    private:
        void SetGameplayHandlersEnabled(bool a_enabled)
        {
            const auto controls = RE::PlayerControls::GetSingleton();
            if (!controls)
                return;

            const std::array<RE::PlayerInputHandler*, 4> handlers{
                controls->lookHandler,
                reinterpret_cast<RE::PlayerInputHandler*>(controls->togglePOVHandler),
                controls->attackHandler,
                controls->readyWeaponHandler
            };

            for (auto* handler : handlers)
            {
                if (!handler)
                    continue;
                if (!a_enabled && !gameplayHandlerStates.empty() &&
                    std::ranges::any_of(gameplayHandlerStates, [handler](const auto& state) { return state.first == handler; }))
                    continue;
                gameplayHandlerStates.emplace_back(handler, handler->inputEventHandlingEnabled);
                handler->inputEventHandlingEnabled = a_enabled;
            }
        }

        void RestoreGameplayHandlers()
        {
            for (const auto& [handler, wasEnabled] : gameplayHandlerStates)
            {
                if (handler)
                    handler->inputEventHandlingEnabled = wasEnabled;
            }
            gameplayHandlerStates.clear();
        }

        std::vector<OModSwitch::AmmoSwitchOption> options;
        std::vector<std::pair<RE::PlayerInputHandler*, bool>> gameplayHandlerStates;
        std::atomic<std::int32_t> selectedIndex{ -1 };
        std::uint64_t lastStickGeneration{ 0 };
        bool optionDiagnosticsPending{ false };
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
        auto state = CurrentWheelState.load(std::memory_order_acquire);
        for (;;)
        {
            if (state == WheelState::Open || state == WheelState::Opening)
                return true;
            if (state == WheelState::Closing)
                return false;
            if (CurrentWheelState.compare_exchange_weak(state, WheelState::Opening, std::memory_order_acq_rel))
                break;
        }

        const auto* tasks = F4SE::GetTaskInterface();
        if (!tasks)
        {
            CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
            return false;
        }

        const auto generation = WheelGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
        StickX.store(0.0f, std::memory_order_relaxed);
        StickY.store(0.0f, std::memory_order_relaxed);
        StickGeneration.fetch_add(1, std::memory_order_relaxed);
        tasks->AddTask([generation]() {
            if (generation != WheelGeneration.load(std::memory_order_relaxed) ||
                CurrentWheelState.load(std::memory_order_acquire) != WheelState::Opening)
            {
                auto expected = WheelState::Opening;
                CurrentWheelState.compare_exchange_strong(expected, WheelState::Closed, std::memory_order_acq_rel);
                return;
            }

            auto options = OModSwitch::GetEquippedAmmoSwitchOptions();
            if (options.size() < 2)
            {
                CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
                return;
            }

            {
                const std::scoped_lock lock{ OptionsLock };
                PendingOptions = std::move(options);
            }

            if (const auto queue = RE::UIMessageQueue::GetSingleton())
                queue->AddMessage(MenuName, RE::UI_MESSAGE_TYPE::kShow);
            else
                CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
        });
        return true;
    }

    bool IsOpen()
    {
        const auto state = CurrentWheelState.load(std::memory_order_acquire);
        if (state == WheelState::Open || state == WheelState::Closing)
            return true;
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
        auto expected = WheelState::Open;
        if (!CurrentWheelState.compare_exchange_strong(expected, WheelState::Closing, std::memory_order_acq_rel))
            return false;

        auto* menu = CurrentMenu();
        auto* ammo = menu ? menu->SelectedAmmo() : nullptr;
        if (ammo)
            REX::INFO("Ammo switch wheel: confirming selection index {} (ammo {:08X})", menu->SelectedIndex(), ammo->GetFormID());
        HideMenu();
        return ammo && OModSwitch::SwitchEquippedAmmoTo(ammo);
    }

    void Cancel()
    {
        auto state = CurrentWheelState.load(std::memory_order_acquire);
        for (;;)
        {
            if (state == WheelState::Closed || state == WheelState::Closing)
                return;
            if (CurrentWheelState.compare_exchange_weak(state, WheelState::Closing, std::memory_order_acq_rel))
                break;
        }

        if (state == WheelState::Opening)
        {
            WheelGeneration.fetch_add(1, std::memory_order_relaxed);
            CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
            return;
        }
        HideMenu();
    }
}
