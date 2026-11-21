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

#include "DynamicLoop.h"
#include "Hooks/ClipGeneratorHook.h"
#include "Settings/Settings.h"

namespace
{
    constexpr std::string_view MarkPrefix = "MWB_dyn_"sv;
    constexpr std::string_view StartMark = "MWB_dyn_StartLoop"sv;
    constexpr std::string_view EndMark = "MWB_dyn_EndLoop"sv;

    constexpr float       MinimumLength = 0.01f;
    constexpr float       CrossTolerance = 0.0001f;
    constexpr std::size_t MaxClips = 64;
    constexpr std::uint32_t PlayerFormID = 0x14;
    constexpr std::uint64_t StaleFrames = 60;
    constexpr std::uint64_t DropFrames = 600;
    constexpr std::uint64_t PruneEvery = 60;

    struct LoopClip
    {
        const void*      Clip{ nullptr };
        const RE::Actor* Owner{ nullptr };
        std::uint32_t    FormID{ 0 };
        float            Start{ 0.0f };
        float            End{ 0.0f };
        float            Overshoot{ 0.0f };
        std::uint32_t    Passes{ 0 };
        std::uint64_t    LastFrame{ 0 };
        bool             Released{ false };
        bool             JumpPending{ false };
        bool             WrapPending{ false };
    };

    struct Marks
    {
        std::optional<float> Start;
        std::optional<float> End;
        bool                 Any{ false };
    };

    std::mutex               Lock;
    std::vector<LoopClip>    Clips;
    std::atomic<std::size_t> Count{ 0 };
    std::atomic<std::uint64_t> Frame{ 0 };

    [[nodiscard]] bool Live(const LoopClip& Tracked)
    {
        return Frame.load(std::memory_order_relaxed) - Tracked.LastFrame <= StaleFrames;
    }

    [[nodiscard]] bool Is(std::string_view Text, std::string_view Name)
    {
        return Text.size() == Name.size() && _strnicmp(Text.data(), Name.data(), Name.size()) == 0;
    }

    void Collect(void* Context, float Time, const char* Text)
    {
        auto& Found = *static_cast<Marks*>(Context);

        if (_strnicmp(Text, MarkPrefix.data(), MarkPrefix.size()) != 0)
            return;

        const std::string_view Name{ Text };
        Found.Any = true;

        if (Is(Name, StartMark))
            Found.Start = Found.Start ? (std::min)(*Found.Start, Time) : Time;
        else if (Is(Name, EndMark))
            Found.End = Found.End ? (std::max)(*Found.End, Time) : Time;
    }

    [[nodiscard]] LoopClip* Find(const void* Clip)
    {
        const auto Found = std::ranges::find(Clips, Clip, &LoopClip::Clip);

        return Found == Clips.end() ? nullptr : std::addressof(*Found);
    }

    void Forget(const void* Clip)
    {
        std::erase_if(Clips, [&](const LoopClip& Tracked) { return Tracked.Clip == Clip; });
        Count = Clips.size();
    }

    [[nodiscard]] RE::Actor* OrPlayer(RE::Actor* Actor)
    {
        return Actor ? Actor : RE::PlayerCharacter::GetSingleton();
    }

    void Wrap(void* Clip, LoopClip& Tracked)
    {
        const float Length = Tracked.End - Tracked.Start;
        const float Target = Tracked.Start + std::fmod((std::max)(Tracked.Overshoot, 0.0f), Length);

        Hooks::SetClipLocalTime(Clip, Target);
        Tracked.WrapPending = false;
        Tracked.Overshoot = 0.0f;

        if (++Tracked.Passes == 1 && Settings::VerboseLog())
            REX::INFO("MWB_dyn: {:08X} loops from {:.3f}s back to {:.3f}s", Tracked.FormID, Tracked.End, Tracked.Start);
    }
}

namespace DynamicLoop
{
    bool Tracking()
    {
        return Count.load(std::memory_order_relaxed) > 0;
    }

    void Tick()
    {
        const auto Now = Frame.fetch_add(1, std::memory_order_relaxed) + 1;

        if (!Tracking() || Now % PruneEvery != 0)
            return;

        const std::scoped_lock Guard{ Lock };

        std::erase_if(Clips, [&](const LoopClip& Tracked)
        {
            if (Now - Tracked.LastFrame <= DropFrames)
                return false;

            if (Settings::VerboseLog())
                REX::INFO("MWB_dyn: clip on {:08X} dropped, its graph stopped updating without ending it", Tracked.FormID);

            return true;
        });

        Count = Clips.size();
    }

    void OnClipActivated(const void* Clip, const void* Character)
    {
        Marks Found;
        Hooks::VisitClipAnnotations(Clip, Collect, &Found);

        if (Tracking())
        {
            const std::scoped_lock Guard{ Lock };
            Forget(Clip);
        }

        if (!Found.Any)
            return;

        const auto Owner = Hooks::ActorOfCharacter(Character);
        const auto FormID = Owner ? Owner->GetFormID() : 0u;

        if (!Found.Start || !Found.End || *Found.End - *Found.Start < MinimumLength)
        {
            REX::WARN("MWB_dyn: a clip on {:08X} needs MWB_dyn_StartLoop before MWB_dyn_EndLoop (start {}, end {}); it plays without the loop",
                FormID, Found.Start ? std::format("{:.3f}s", *Found.Start) : "missing"s, Found.End ? std::format("{:.3f}s", *Found.End) : "missing"s);
            return;
        }

        if (!Owner)
            return;

        {
            const std::scoped_lock Guard{ Lock };

            if (Clips.size() >= MaxClips)
                Clips.erase(Clips.begin());

            Clips.push_back({ .Clip = Clip, .Owner = Owner, .FormID = FormID, .Start = *Found.Start, .End = *Found.End, .LastFrame = Frame.load(std::memory_order_relaxed) });
            Count = Clips.size();
        }

        if (Settings::VerboseLog())
            REX::INFO("MWB_dyn: clip on {:08X} with a loop from {:.3f}s to {:.3f}s", FormID, *Found.Start, *Found.End);
    }

    float BeforeUpdate(void* Clip, float Delta)
    {
        const std::scoped_lock Guard{ Lock };

        const auto Tracked = Find(Clip);

        if (!Tracked)
            return Delta;

        Tracked->LastFrame = Frame.load(std::memory_order_relaxed);

        const float Local = Hooks::ClipLocalTime(Clip);

        if (Tracked->JumpPending)
        {
            Tracked->JumpPending = false;

            if (Local < Tracked->End)
            {
                Hooks::SetClipLocalTime(Clip, Tracked->End);

                if (Settings::VerboseLog())
                    REX::INFO("MWB_dyn: {:08X} skips from {:.3f}s to MWB_dyn_EndLoop at {:.3f}s", Tracked->FormID, Local, Tracked->End);
            }

            return Delta;
        }

        if (Tracked->Released)
            return Delta;

        const float Speed = Hooks::ClipPlaybackSpeed(Clip);

        if (Speed <= 0.0f || Delta <= 0.0f)
            return Delta;

        const float Step = Delta * Speed;

        if (Local >= Tracked->End - CrossTolerance || Local + Step < Tracked->End)
            return Delta;

        Tracked->Overshoot = Local + Step - Tracked->End;
        Tracked->WrapPending = true;

        return (Tracked->End - Local) / Speed;
    }

    void AfterUpdate(void* Clip)
    {
        const std::scoped_lock Guard{ Lock };

        const auto Tracked = Find(Clip);

        if (!Tracked || Tracked->Released)
            return;

        if (!Tracked->WrapPending)
        {
            const float Local = Hooks::ClipLocalTime(Clip);

            if (Local <= Tracked->End + CrossTolerance)
                return;

            Tracked->Overshoot = Local - Tracked->End;
        }

        Wrap(Clip, *Tracked);
    }

    void OnClipDeactivated(const void* Clip)
    {
        const std::scoped_lock Guard{ Lock };

        const auto Tracked = Find(Clip);

        if (!Tracked)
            return;

        if (Settings::VerboseLog())
            REX::INFO("MWB_dyn: clip on {:08X} ended after {} loop(s){}", Tracked->FormID, Tracked->Passes, Tracked->Released ? "" : " (cut while looping)");

        Forget(Clip);
    }

    std::uint32_t End(RE::Actor* Actor)
    {
        const auto Owner = OrPlayer(Actor);
        std::uint32_t Ended = 0;

        const std::scoped_lock Guard{ Lock };

        for (auto& Tracked : Clips)
        {
            if (Tracked.Owner != Owner || Tracked.Released || !Live(Tracked))
                continue;

            Tracked.Released = true;
            Tracked.JumpPending = true;
            Tracked.WrapPending = false;
            ++Ended;
        }

        if (Settings::VerboseLog())
            REX::INFO("MWB_dyn: EndDynamicLoop on {:08X}, {} loop(s) sent to MWB_dyn_EndLoop", Owner ? Owner->GetFormID() : PlayerFormID, Ended);

        return Ended;
    }

    std::uint32_t Release(RE::Actor* Actor)
    {
        const auto Owner = OrPlayer(Actor);
        std::uint32_t Released = 0;

        const std::scoped_lock Guard{ Lock };

        for (auto& Tracked : Clips)
        {
            if (Tracked.Owner != Owner || Tracked.Released || !Live(Tracked))
                continue;

            Tracked.Released = true;
            Tracked.WrapPending = false;
            ++Released;
        }

        if (Settings::VerboseLog())
            REX::INFO("MWB_dyn: ReleaseDynamicLoop on {:08X}, {} loop(s) play out", Owner ? Owner->GetFormID() : PlayerFormID, Released);

        return Released;
    }

    bool Looping(RE::Actor* Actor)
    {
        const auto Owner = OrPlayer(Actor);

        const std::scoped_lock Guard{ Lock };

        return std::ranges::any_of(Clips, [&](const LoopClip& Tracked) { return Tracked.Owner == Owner && !Tracked.Released && Live(Tracked); });
    }

    void Reset()
    {
        const std::scoped_lock Guard{ Lock };
        Clips.clear();
        Count = 0;
    }
}
