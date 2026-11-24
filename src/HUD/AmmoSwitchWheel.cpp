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
#include "Settings/Settings.h"

#include <F4SE/API.h>

#include <RE/B/BSScaleformManager.h>
#include <RE/Fallout.h>
#include <RE/G/GameMenuBase.h>
#include <RE/H/HUDColorTypes.h>
#include <RE/U/UI.h>
#include <RE/U/UIShaderColors.h>

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
    std::atomic<std::int32_t> PendingSelectionDelta{ 0 };
    std::atomic<bool> PendingWheelConfirm{ false };
    Scaleform::Ptr<RE::IMenu> HiddenHUD;
    Scaleform::Ptr<Scaleform::GFx::Movie> HiddenHUDMovie;
    bool HUDMenuWasVisible{};
    bool HUDMovieWasVisible{};

    using ButtonBarCache = bool (*)(RE::GameMenuBase*);
    ButtonBarCache OriginalButtonBarCache{};

    [[nodiscard]] bool IsWheelActive()
    {
        const auto state = CurrentWheelState.load(std::memory_order_acquire);
        return state == WheelState::Opening || state == WheelState::Open || state == WheelState::Closing;
    }

    bool NormalizeButtonBarForCache(RE::GameMenuBase& menu)
    {
        struct Change
        {
            RE::BSGFxShaderFXTarget* target;
            RE::UIShaderColors original;
        };

        std::vector<Change> changes;
        auto visit = [&](auto&& self, RE::BSGFxShaderFXTarget* target) -> void {
            if (!target)
                return;

            Value name;
            target->GetMember("name", &name);
            const std::string_view targetName = name.IsString() && name.GetString() ? name.GetString() : "";
            if ((targetName == "ButtonHintBar_mc" || targetName == "ButtonBarHolder_mc") &&
                std::ranges::none_of(changes, [target](const auto& change) { return change.target == target; }))
            {
                changes.push_back({ target, target->shaderFX });
                target->shaderFX.colorMultipliers.r = 1.0F;
                target->shaderFX.colorMultipliers.g = 1.0F;
                target->shaderFX.colorMultipliers.b = 1.0F;
                target->shaderFX.colorBrightness = 1.0F;
            }

            for (auto* child : target->shaderFXObjects)
                self(self, child);
        };

        for (auto* target : menu.shaderFXObjects)
            visit(visit, target);
        visit(visit, menu.filterHolder.get());
        visit(visit, menu.buttonHintBar.get());

        if (!changes.empty()) {
            static std::once_flag logged;
            std::call_once(logged, [&] {
                REX::INFO("Ammo switch wheel: normalized {} global button bar cache target(s)", changes.size());
            });
        }

        const bool result = OriginalButtonBarCache ? OriginalButtonBarCache(&menu) : false;

        for (const auto& change : changes)
            change.target->shaderFX = change.original;

        return result;
    }

    bool CacheButtonBar(RE::GameMenuBase* menu)
    {
        if (!menu || !IsWheelActive())
            return OriginalButtonBarCache ? OriginalButtonBarCache(menu) : false;

        return NormalizeButtonBarForCache(*menu);
    }

    void InstallButtonBarHook()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            REL::Relocation<std::uintptr_t> buttonBar{ RE::VTABLE::ButtonBarMenu[0] };
            OriginalButtonBarCache = reinterpret_cast<ButtonBarCache>(buttonBar.write_vfunc(0x10, &CacheButtonBar));
            REX::INFO("Ammo switch wheel: global button bar cache hook installed");
        });
    }

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

            // Unlike FavoritesMenu, this standalone movie does not receive a
            // native filterHolder during LoadMovie. Keep the same target
            // shape by creating the root target before SetUpButtonBar; this
            // is the path that makes the shared ButtonBarMenu visible.
            if (!filterHolder && uiMovie)
            {
                filterHolder = std::make_unique<RE::BSGFxShaderFXTarget>(*uiMovie, "root1");
                filterHolder->CreateAndSetFiltersToHUD(RE::HUDColorTypes::kGameplayHUDColor);
                REX::INFO("Ammo switch wheel: created native button hint target fallback");
            }

            if (!EnsureNativeWheelShaderTarget())
                REX::WARN("Ammo switch wheel: nativeContent target is missing; native wheel shadow is disabled");

            EnsureNativeButtonBar();

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
            const auto setOptionsSucceeded = menuObj.Invoke("SetOptions", std::array<Value, 6>{ std::move(names), std::move(counts), std::move(icons), std::move(damages), Value{ currentIndex }, Value{ Settings::AmmoSwitchWheelToggleMode() } });
            REX::INFO("Ammo switch wheel: passed {} dynamic ammo sectors to Scaleform (GFx invoke return={})", options.size(), setOptionsSucceeded);
            optionDiagnosticsPending = true;
        }

        ~AmmoSwitchMenu() override
        {
            RestoreGameplayHandlers();
            RestoreGlobalButtonBar();
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
            EnsureNativeButtonBar();
            NormalizeGlobalButtonBar();
            CurrentWheelState.store(WheelState::Open, std::memory_order_release);
            SetGameplayHandlersEnabled(false);
        }

        void OnRemovedFromMenuStack() override
        {
            RestoreGameplayHandlers();
            RestoreGlobalButtonBar();
            GameMenuBase::OnRemovedFromMenuStack();
            RestoreHUD();
            CurrentWheelState.store(WheelState::Closed, std::memory_order_release);
        }

        void AdvanceMovie(float a_timeDelta, std::uint64_t a_time) override
        {
            GameMenuBase::AdvanceMovie(a_timeDelta, a_time);
            EnsureNativeButtonBar();
            EnsureNativeWheelShaderTarget();
            NormalizeGlobalButtonBar();

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

            const auto selectionDelta = PendingSelectionDelta.exchange(0, std::memory_order_acq_rel);
            if (selectionDelta != 0)
                menuObj.Invoke("MoveSelection", std::array<Value, 1>{ Value{ selectionDelta } });

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

            Value clickConfirmed;
            const bool mouseConfirmed = menuObj.Invoke("ConsumeClickConfirm", &clickConfirmed) &&
                clickConfirmed.IsBoolean() && clickConfirmed.GetBoolean();
            const bool keyboardConfirmed = PendingWheelConfirm.exchange(false, std::memory_order_acq_rel);
            if (mouseConfirmed || keyboardConfirmed)
            {
                REX::INFO("Ammo switch wheel: {} confirmation requested",
                    mouseConfirmed ? "mouse" : "keyboard");
                if (!AmmoSwitchWheel::Confirm())
                    AmmoSwitchWheel::Cancel();
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
        struct ButtonBarColorState
        {
            RE::BSGFxShaderFXTarget* target;
            float r;
            float g;
            float b;
            float brightness;
        };

        void NormalizeGlobalButtonBar()
        {
            if (!buttonHintBar)
                return;
            const auto ui = RE::UI::GetSingleton();
            if (!ui)
                return;
            const auto buttonBar = ui->GetMenu("ButtonBarMenu");
            if (!buttonBar)
                return;

            auto* menu = static_cast<RE::GameMenuBase*>(buttonBar.get());
            bool changed = false;
            auto normalize = [&](auto&& self, RE::BSGFxShaderFXTarget* target) -> void {
                if (!target)
                    return;

                Value name;
                target->GetMember("name", &name);
                const std::string_view targetName = name.IsString() && name.GetString() ? name.GetString() : "";
                if (targetName == "ButtonHintBar_mc" || targetName == "ButtonBarHolder_mc")
                {
                    if (std::ranges::none_of(globalButtonBarColors, [target](const auto& state) { return state.target == target; }))
                    {
                        globalButtonBarColors.push_back({
                            target,
                            target->shaderFX.colorMultipliers.r,
                            target->shaderFX.colorMultipliers.g,
                            target->shaderFX.colorMultipliers.b,
                            target->shaderFX.colorBrightness
                        });
                        REX::INFO("Ammo switch wheel: normalized global button bar target {}", targetName);
                    }

                    if (target->shaderFX.colorMultipliers.r != 1.0F ||
                        target->shaderFX.colorMultipliers.g != 1.0F ||
                        target->shaderFX.colorMultipliers.b != 1.0F ||
                        target->shaderFX.colorBrightness != 1.0F)
                    {
                        target->shaderFX.colorMultipliers.r = 1.0F;
                        target->shaderFX.colorMultipliers.g = 1.0F;
                        target->shaderFX.colorMultipliers.b = 1.0F;
                        target->shaderFX.colorBrightness = 1.0F;
                        changed = true;
                    }
                }

                for (auto* child : target->shaderFXObjects)
                    self(self, child);
            };

            for (auto* target : menu->shaderFXObjects)
                normalize(normalize, target);
            normalize(normalize, menu->filterHolder.get());
            normalize(normalize, menu->buttonHintBar.get());

            if (changed)
            {
                // Changing shaderFX after the stock cache has run does not
                // recolor already cached text quads. Re-enter the complete
                // ButtonBarMenu cache chain once so the normalized receiver
                // is used to build the native bar, just like FavoritesMenu.
                menu->CacheShaderFXQuadsForRenderer_Impl();
            }
        }

        void RestoreGlobalButtonBar()
        {
            const auto ui = RE::UI::GetSingleton();
            const auto buttonBar = ui ? ui->GetMenu("ButtonBarMenu") : nullptr;
            auto* menu = buttonBar ? static_cast<RE::GameMenuBase*>(buttonBar.get()) : nullptr;
            auto isCurrent = [&](auto&& self, RE::BSGFxShaderFXTarget* target, RE::BSGFxShaderFXTarget* wanted) -> bool {
                if (!target)
                    return false;
                if (target == wanted)
                    return true;
                return std::ranges::any_of(target->shaderFXObjects, [&](auto* child) { return self(self, child, wanted); });
            };

            for (const auto& state : globalButtonBarColors)
            {
                if (!state.target || !menu)
                    continue;
                const bool current = isCurrent(isCurrent, menu->filterHolder.get(), state.target) ||
                    isCurrent(isCurrent, menu->buttonHintBar.get(), state.target) ||
                    std::ranges::any_of(menu->shaderFXObjects, [&](auto* target) { return isCurrent(isCurrent, target, state.target); });
                if (!current)
                    continue;
                state.target->shaderFX.colorMultipliers.r = state.r;
                state.target->shaderFX.colorMultipliers.g = state.g;
                state.target->shaderFX.colorMultipliers.b = state.b;
                state.target->shaderFX.colorBrightness = state.brightness;
            }
            globalButtonBarColors.clear();
        }

        void EnsureNativeButtonBar()
        {
            // FavoritesMenu registers its data source only after the native
            // GameMenuBase stack setup has created filterHolder. Creating a
            // root1 fallback in the constructor takes a different shader
            // path and tints the shared ButtonBarMenu receiver incorrectly.
            if (buttonHintBar || !filterHolder)
                return;

            Value hintSource;
            if (filterHolder->GetMember("AmmoWheelHintSource", &hintSource) && hintSource.IsDisplayObject())
            {
                // Match FavoritesMenu's global registration. The cache hook
                // below temporarily normalizes the shared receiver while
                // ButtonBarMenu builds its native quads.
                SetUpButtonBar(*filterHolder, "AmmoWheelHintSource", RE::HUDColorTypes::kGameplayHUDColor);
                REX::INFO("Ammo switch wheel: registered native global button bar hints");
            }
            else
            {
                REX::WARN("Ammo switch wheel: native global button bar hint source is missing");
            }
        }

        bool EnsureNativeWheelShaderTarget()
        {
            if (wheelShaderTarget || !uiMovie || !menuObj.IsObject())
                return wheelShaderTarget != nullptr;

            Value nativeContent;
            if (!menuObj.GetMember("nativeContent", &nativeContent) || !nativeContent.IsDisplayObject())
                return false;

            wheelShaderTarget = std::make_unique<RE::BSGFxShaderFXTarget>(*uiMovie, "root1.nativeContent");
            // Keep the wheel's authored gold/cream colors while enabling the
            // engine's alpha-based shadow descriptor for its graphics and text.
            // The global button bar remains outside this target.
            wheelShaderTarget->CreateAndSetFiltersToColor(255, 255, 255, 1.0F);
            wheelShaderTarget->shaderFX.enabledStates.set(RE::UIShaderColors::Flags::kUseAlphaForDropshadow);
            shaderFXObjects.push_back(wheelShaderTarget.get());
            REX::INFO("Ammo switch wheel: native wheel shader target registered, shaderFXObjects={}, alphaDropShadow={}",
                shaderFXObjects.size(),
                wheelShaderTarget->shaderFX.enabledStates.all(RE::UIShaderColors::Flags::kUseAlphaForDropshadow));
            return true;
        }

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
        std::vector<ButtonBarColorState> globalButtonBarColors;
        std::unique_ptr<RE::BSGFxShaderFXTarget> wheelShaderTarget;
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
        InstallButtonBarHook();
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
        PendingSelectionDelta.store(0, std::memory_order_relaxed);
        PendingWheelConfirm.store(false, std::memory_order_relaxed);
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

    void MoveSelection(int a_delta)
    {
        if (a_delta != 0 && IsOpen())
            PendingSelectionDelta.fetch_add(a_delta, std::memory_order_release);
    }

    void RequestConfirm()
    {
        if (IsOpen())
            PendingWheelConfirm.store(true, std::memory_order_release);
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
