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
        bool alreadyApplied{false};
    };

    // Both AMD drivers initialise context->commandError to -6, but their fetchResource failure
    // branch targets the completion block, which publishes a length and clears commandError.
    // Preserve the initial error and leave the caller's zero length untouched instead.
    //
    // Validate the complete relevant control flow before changing a displacement. In particular,
    // an arbitrary "add rsp" near a branch target is not enough to identify the error exit.
    inline bool makePlan(const UInt8* const code, const size_t length,
                         const mach_vm_address_t address, const mach_vm_address_t fetchResource,
                         Plan& result)
    {
        static const UInt8 initialError[] = {0xC7, 0x87, 0x44, 0x06, 0x00, 0x00, 0xFA, 0xFF, 0xFF, 0xFF};
        static const UInt8 completion[] = {
            0x41, 0x89, 0x96, 0xD0, 0x11, 0x00, 0x00,
            0x48, 0x8B, 0x85, 0x30, 0xFF, 0xFF, 0xFF,
            0x8B, 0x4D, 0x9C, 0x89, 0x08,
            0x41, 0xC7, 0x86, 0x44, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x81, 0xC4, 0x28, 0x03, 0x00, 0x00,
            0x5B, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3,
        };
        static constexpr size_t completionStores = 30;
        if (code == nullptr || length < 0x48 || length > 0x580 ||
            memcmp(code + 0x3E, initialError, sizeof(initialError)) != 0) {
            return false;
        }

        size_t success = 0;
        for (size_t i = 0x180; i + sizeof(completion) <= length; ++i) {
            if (memcmp(code + i, completion, sizeof(completion)) == 0) {
                if (success != 0) { return false; }
                success = i;
            }
        }
        if (success == 0) { return false; }
        const auto failure = success + completionStores;

        size_t branch = 0;
        for (size_t i = 0x48; i + 16 <= length && i < 0x280; ++i) {
            if (code[i] != 0xE8) { continue; }
            SInt32 callRelative;
            memcpy(&callRelative, code + i + 1, sizeof(callRelative));
            const auto target = address + i + 5 +
                static_cast<mach_vm_address_t>(static_cast<SInt64>(callRelative));
            if (target != fetchResource) { continue; }
            // call fetchResource; mov edx,[rbp-0x2c]; test al,al; je <completion/error exit>
            static const UInt8 check[] = {0x8B, 0x55, 0xD4, 0x84, 0xC0, 0x0F, 0x84};
            if (memcmp(code + i + 5, check, sizeof(check)) != 0 || branch != 0) { return false; }
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

    inline bool install(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                        const size_t size, const char* const processSymbol,
                        const char* const fetchSymbol, const char* const property)
    {
        const auto process = patcher.solveSymbol(id, processSymbol, slide, size);
        const auto fetch = patcher.solveSymbol(id, fetchSymbol, slide, size);
        if (process == 0 || fetch == 0) {
            patcher.clearError();
            return false;
        }
        if (process < slide || process - slide >= size) { return false; }
        const auto available = size - (process - slide);
        const auto length = available < 0x580 ? available : 0x580;
        auto* const code = reinterpret_cast<UInt8*>(process);
        Plan plan;
        if (!makePlan(code, length, process, fetch, plan)) {
            SYSLOG("VideoCmd", "Unrecognised processCommand failure path: %s", processSymbol);
            return false;
        }
        if (!plan.alreadyApplied) {
            const auto relative = static_cast<SInt32>(plan.failure - (plan.branch + 6));
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                return false;
            }
            memcpy(code + plan.branch + 2, &relative, sizeof(relative));
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        }
        NRed::singleton().setProp32(property, 1);
        SYSLOG("VideoCmd", "Guarded fetchResource failure: %s", processSymbol);
        return true;
    }
}
