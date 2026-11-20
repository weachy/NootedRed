#pragma once

#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <NRed.hpp>

namespace VideoCommandPatch
{
    struct Plan
    {
        size_t branch{0};
        size_t success{0};
        size_t failure{0};
        bool   alreadyApplied{false};
    };

    // Both AMD drivers initialise context->commandError to -6, but their fetchResource failure
    // branch targets the completion block, which publishes a length and clears commandError.
    // Preserve the initial error and leave the caller's zero length untouched instead.
    //
    // Cross-version verified across macOS 14.3, 15.6, 15.8.1, 15.8.2, and 26.7.1 KCs:
    //  - initialError (`mov dword ptr [rdi+0x644], -6`) sits in [0x20, 0x60) (0x32 on 14.3, 0x35 on 15.x, 0x3E on 26.x).
    //  - `call fetchResource; mov edx, [rbp-XX]; test al, al; je <completion/error exit>` uses [rbp-0x2C] (0xD4) on 14/26
    //    and [rbp-0x34] (0xCC) on 15.x.
    //  - completion block differs only in 3 stack-frame displacement bytes (0x38/0x84/0x18 on 14/15 vs 0x30/0x9C/0x28 on 26).
    inline bool matchCompletion(const UInt8* const code)
    {
        static constexpr UInt8 kCompletionPattern[] = {
            0x41, 0x89, 0x96, 0xD0, 0x11, 0x00, 0x00,                         // mov [r14+0x11d0], edx
            0x48, 0x8B, 0x85, 0x00, 0xFF, 0xFF, 0xFF,                         // mov rax, [rbp-XX]
            0x8B, 0x4D, 0x00,                                                 // mov ecx, [rbp-YY]
            0x89, 0x08,                                                       // mov [rax], ecx
            0x41, 0xC7, 0x86, 0x44, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov dword ptr [r14+0x644], 0
            0x48, 0x81, 0xC4, 0x00, 0x03, 0x00, 0x00,                         // add rsp, 0x3XX
            0x5B, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3, // pop rbx, r12..r15, rbp; ret
        };
        static constexpr UInt8 kCompletionMask[] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0x00,
            0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        };
        static_assert(sizeof(kCompletionPattern) == 48 && sizeof(kCompletionMask) == 48, "Completion pattern size");
        for (size_t i = 0; i < sizeof(kCompletionPattern); ++i) {
            if ((code[i] & kCompletionMask[i]) != kCompletionPattern[i]) { return false; }
        }
        return true;
    }

    inline bool makePlan(const UInt8* const code, const size_t length, const mach_vm_address_t address,
                         const mach_vm_address_t fetchResource, Plan& result)
    {
        static constexpr UInt8 kInitialError[]   = {0xC7, 0x87, 0x44, 0x06, 0x00, 0x00, 0xFA, 0xFF, 0xFF, 0xFF};
        static constexpr size_t kCompletionStores = 30;
        if (code == nullptr || length < 0x60 || length > 0x580) { return false; }

        bool foundInitialError = false;
        for (size_t i = 0x20; i + sizeof(kInitialError) <= 0x60; ++i) {
            if (memcmp(code + i, kInitialError, sizeof(kInitialError)) == 0) {
                foundInitialError = true;
                break;
            }
        }
        if (!foundInitialError) { return false; }

        size_t success = 0;
        for (size_t i = 0x180; i + 48 <= length; ++i) {
            if (matchCompletion(code + i)) {
                if (success != 0) { return false; }
                success = i;
            }
        }
        if (success == 0) { return false; }
        const auto failure = success + kCompletionStores;

        size_t branch = 0;
        for (size_t i = 0x48; i + 16 <= length && i < 0x280; ++i) {
            if (code[i] != 0xE8) { continue; }
            SInt32 callRelative;
            memcpy(&callRelative, code + i + 1, sizeof(callRelative));
            const auto target =
                address + i + 5 + static_cast<mach_vm_address_t>(static_cast<SInt64>(callRelative));
            if (target != fetchResource) { continue; }
            // call fetchResource; mov edx, [rbp-XX]; test al, al; je <completion/error exit>
            if (code[i + 5] != 0x8B || code[i + 6] != 0x55 || code[i + 8] != 0x84 || code[i + 9] != 0xC0 ||
                code[i + 10] != 0x0F || code[i + 11] != 0x84 || branch != 0) {
                return false;
            }
            branch = i + 10;
        }
        if (branch == 0) { return false; }

        SInt32 relative;
        memcpy(&relative, code + branch + 2, sizeof(relative));
        const auto target = static_cast<SInt64>(branch + 6) + relative;
        if (target != static_cast<SInt64>(success) && target != static_cast<SInt64>(failure)) { return false; }

        result = {branch, success, failure, target == static_cast<SInt64>(failure)};
        return true;
    }

    inline bool install(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size,
                        const char* const processSymbol, const char* const fetchSymbol, const char* const property)
    {
        const auto process = patcher.solveSymbol(id, processSymbol, slide, size);
        const auto fetch   = patcher.solveSymbol(id, fetchSymbol, slide, size);
        if (process == 0 || fetch == 0) {
            patcher.clearError();
            return false;
        }
        if (process < slide || process - slide >= size) { return false; }
        const auto  available = size - (process - slide);
        const auto  length    = available < 0x580 ? available : 0x580;
        auto* const code      = reinterpret_cast<UInt8*>(process);
        Plan        plan;
        if (!makePlan(code, length, process, fetch, plan)) {
            SYSLOG("VideoCmd", "Unrecognised processCommand failure path: %s", processSymbol);
            return false;
        }
        if (!plan.alreadyApplied) {
            const auto relative = static_cast<SInt32>(plan.failure - (plan.branch + 6));
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return false; }
            memcpy(code + plan.branch + 2, &relative, sizeof(relative));
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        }
        NRed::singleton().setProp32(property, 1);
        SYSLOG("VideoCmd", "Guarded fetchResource failure: %s", processSymbol);
        return true;
    }
}    // namespace VideoCommandPatch
