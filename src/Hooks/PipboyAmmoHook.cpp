/*
    This file is part of Modern Weapon Behavior.
    Modern Weapon Behavior is free software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.
    This file is additionally covered by the Modding Exception and GPL-3.0
    Linking Exception described in EXCEPTIONS.
*/

#include "PipboyAmmoHook.h"

#include "Core/OModSwitch.h"

#include <F4SE/API.h>
#include <xbyak/xbyak.h>

#include <cstring>

namespace PipboyAmmoHook
{
    namespace
    {
        struct Runtime
        {
            REL::Version Version;
            std::uintptr_t CanEquipRva;
            std::uintptr_t GeneralMessageCallRva;
            std::uintptr_t ModMessageCallRva;
            std::uintptr_t SkipRva;
            std::uintptr_t MessageGetterRva;
            std::ptrdiff_t ItemOffset;
            std::ptrdiff_t TargetOffset;
            bool TargetInR14;
            std::array<std::uint8_t, 5> GeneralCallBytes;
            std::array<std::uint8_t, 5> ModCallBytes;
        };

        constexpr std::array Runtimes{
            Runtime{ F4SE::RUNTIME_1_10_163, 0xE1CD00, 0xE1C91F, 0xE1C930, 0xE1CB5E, 0xAE1E90,
                -0x41, 0x57, false, { 0xE8, 0x6C, 0x55, 0xCC, 0xFF }, { 0xE8, 0x5B, 0x55, 0xCC, 0xFF } },
            Runtime{ F4SE::RUNTIME_1_11_240, 0xCE6F90, 0xCE6B5F, 0xCE6B70, 0xCE6D4B, 0xA37270,
                -0x19, 0, true, { 0xE8, 0x0C, 0x07, 0xD5, 0xFF }, { 0xE8, 0xFB, 0x06, 0xD5, 0xFF } },
        };

        using MessageGetter = const char* (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

        const char* HandleCannotEquip(
            RE::TESObjectREFR* a_target,
            RE::TESForm* a_item,
            std::uint32_t a_unequip,
            std::uint32_t a_type,
            std::uintptr_t a_messageArg1,
            std::uintptr_t a_messageArg2,
            std::uintptr_t a_messageArg3,
            std::uintptr_t a_messageArg4,
            MessageGetter a_messageGetter)
        {
            static_cast<void>(a_type);
            if (a_target == RE::PlayerCharacter::GetSingleton() && a_item &&
                a_item->formType == RE::ENUM_FORM_ID::kAMMO && a_unequip == 0 &&
                OModSwitch::SwitchEquippedAmmoFromPipboy(static_cast<RE::TESAmmo*>(a_item)))
            {
                return nullptr;
            }

            return a_messageGetter(a_messageArg1, a_messageArg2, a_messageArg3, a_messageArg4);
        }

        std::uintptr_t RelativeCallTarget(std::uintptr_t a_site)
        {
            const auto displacement = *reinterpret_cast<const std::int32_t*>(a_site + 1);
            return a_site + 5 + displacement;
        }

        std::uintptr_t RelativeJumpTarget(std::uintptr_t a_site)
        {
            const auto jump = a_site + 5;
            if (*reinterpret_cast<const std::uint8_t*>(jump) != 0xE9)
                return 0;
            const auto displacement = *reinterpret_cast<const std::int32_t*>(jump + 1);
            return jump + 5 + displacement;
        }

        class CannotEquipCode : public Xbyak::CodeGenerator
        {
        public:
            CannotEquipCode(const Runtime& a_runtime, std::uintptr_t a_base) :
                Xbyak::CodeGenerator(4096)
            {
                Generate(a_runtime, a_base);
            }

            CannotEquipCode(void* a_buffer, const Runtime& a_runtime, std::uintptr_t a_base) :
                Xbyak::CodeGenerator(4096, a_buffer)
            {
                Generate(a_runtime, a_base);
            }

        private:
            void Generate(const Runtime& a_runtime, std::uintptr_t a_base)
            {
                Xbyak::Label hook, skipBranch, skipTarget, generalBranch, generalTarget, modTarget;

                mov(edi, eax);
                sub(rsp, 0x50);
                mov(qword[rsp + 0x20], rcx);
                mov(qword[rsp + 0x28], rdx);
                mov(qword[rsp + 0x30], r8);
                mov(qword[rsp + 0x38], r9);
                mov(rax, a_base + a_runtime.MessageGetterRva);
                mov(qword[rsp + 0x40], rax);

                if (a_runtime.TargetInR14)
                {
                    mov(rcx, r14);
                    mov(rdx, qword[rbp + a_runtime.ItemOffset]);
                }
                else
                {
                    mov(rcx, qword[rbp + a_runtime.TargetOffset]);
                    mov(rdx, qword[rbp + a_runtime.ItemOffset]);
                }
                mov(r8d, ebx);
                mov(r9d, edi);
                call(ptr[rip + hook]);
                test(rax, rax);
                jz(skipBranch);

                mov(rcx, rax);
                mov(rdx, qword[rsp + 0x28]);
                mov(r8d, dword[rsp + 0x30]);
                mov(r9b, byte[rsp + 0x38]);
                add(rsp, 0x50);
                cmp(edi, 2);
                jne(generalBranch);
                jmp(ptr[rip + modTarget]);
                L(generalBranch);
                jmp(ptr[rip + generalTarget]);

                L(skipBranch);
                add(rsp, 0x50);
                jmp(ptr[rip + skipTarget]);

                L(hook);
                dq(reinterpret_cast<std::uintptr_t>(&HandleCannotEquip));
                L(generalTarget);
                dq(a_base + a_runtime.GeneralMessageCallRva + 5);
                L(modTarget);
                dq(a_base + a_runtime.ModMessageCallRva + 5);
                L(skipTarget);
                dq(a_base + a_runtime.SkipRva);
            }
        };
    }

    void Initialize()
    {
        const auto module = REX::FModule::GetExecutingModule();
        const auto version = module.GetFileVersion();
        const auto runtime = std::ranges::find(Runtimes, version, &Runtime::Version);
        if (runtime == Runtimes.end())
            return;

        const auto base = module.GetBaseAddress();
        const auto generalSite = base + runtime->GeneralMessageCallRva;
        const auto modSite = base + runtime->ModMessageCallRva;
        if (REL::Relocation<std::uintptr_t>{ RE::ID::ActorEquipManager::CanEquip }.address() != base + runtime->CanEquipRva ||
            std::memcmp(reinterpret_cast<const void*>(generalSite), runtime->GeneralCallBytes.data(), runtime->GeneralCallBytes.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(modSite), runtime->ModCallBytes.data(), runtime->ModCallBytes.size()) != 0 ||
            RelativeCallTarget(generalSite) != base + runtime->MessageGetterRva ||
            RelativeCallTarget(modSite) != base + runtime->MessageGetterRva ||
            RelativeJumpTarget(generalSite) != base + runtime->SkipRva ||
            RelativeJumpTarget(modSite) != base + runtime->SkipRva)
        {
            REX::WARN("Pip-Boy ammo activation: native error-message call sites did not match; click-to-switch is disabled");
            return;
        }

        CannotEquipCode sizingCode(*runtime, base);
        constexpr std::size_t BranchStubSize = 14 * 2;
        const auto codeSize = sizingCode.getSize();
        static REL::Trampoline trampoline{ "PipboyAmmoHook" };
        trampoline.create(codeSize + BranchStubSize);
        auto* buffer = trampoline.allocate(codeSize);
        CannotEquipCode code(buffer, *runtime, base);
        const auto branch = reinterpret_cast<std::uintptr_t>(buffer);
        trampoline.write_jmp<5>(generalSite, branch);
        trampoline.write_jmp<5>(modSite, branch);
        REX::INFO("Pip-Boy ammo activation: click-to-switch hook enabled for runtime {} ({} bytes allocated)",
            version.string(), codeSize + BranchStubSize);
    }
}
