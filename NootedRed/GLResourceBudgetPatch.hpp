#pragma once

#include <stddef.h>
#include <stdint.h>

namespace GLResourceBudgetPatch
{
    // AMDRsrcList_Init in the x86_64 X5000 GL driver reserves 512 MiB from the
    // total VRAM when calculating its resource-list budget (in KiB). At exactly
    // 512 MiB this leaves zero: AddRsrc returns 0xffff for every resident resource,
    // so a command can reference a scratch/shader allocation absent from the list
    // passed to kernel BatchPrepare. A GPU VA allocation alone is not residency.
    //
    // Keep at least half the VRAM available to a command's resource list:
    // budgetKiB = max(totalKiB - 512 * 1024, totalKiB / 2).
    // The result is identical to Apple's original policy at >= 1 GiB. The signed
    // comparison handles the subtraction underflow at smaller UMA sizes. The
    // supported APU memory sizes are far below the signed 2 TiB arithmetic limit.
    //
    // Replace the complete, exactly matched 50-byte leaf function. SSE stores
    // clear the same 40-byte structure; rsi is preserved at +0x10. Removing its
    // unused frame leaves room for the bounded calculation. ecx is caller-saved.
    // No branch target, relocation, stack access, or external call is introduced.
    static constexpr uint8_t Find[] = {
        0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x86, 0xD8, 0x12, 0x00,
        0x00, 0x8B, 0x40, 0x14, 0xC5, 0xF8, 0x57, 0xC0, 0xC5, 0xFC,
        0x11, 0x07, 0x48, 0xC7, 0x47, 0x20, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x77, 0x10, 0xC1, 0xE0, 0x0A, 0x05, 0x00, 0x00,
        0xF8, 0xFF, 0x89, 0x47, 0x20, 0x5D, 0xC5, 0xF8, 0x77, 0xC3,
    };
    static constexpr uint8_t Replace[] = {
        0x48, 0x8B, 0x86, 0xD8, 0x12, 0x00, 0x00, 0x8B, 0x40, 0x14,
        0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x07, 0x0F, 0x11, 0x47, 0x10,
        0x48, 0x83, 0x67, 0x20, 0x00, 0x48, 0x89, 0x77, 0x10, 0xC1,
        0xE0, 0x0A, 0x89, 0xC1, 0xD1, 0xE9, 0x2D, 0x00, 0x00, 0x08,
        0x00, 0x39, 0xC8, 0x0F, 0x4C, 0xC1, 0x89, 0x47, 0x20, 0xC3,
    };
    static_assert(sizeof(Find) == 50 && sizeof(Replace) == sizeof(Find), "GL budget leaf size");

    // Reject an ambiguous page, and never match across a validated page boundary.
    inline bool find(const void* data, size_t size, size_t& offset)
    {
        if (data == nullptr || size < sizeof(Find)) { return false; }
        const auto* bytes = static_cast<const uint8_t*>(data);
        bool found = false;
        for (size_t pos = 0; pos <= size - sizeof(Find); ++pos) {
            size_t index = 0;
            while (index < sizeof(Find) && bytes[pos + index] == Find[index]) { ++index; }
            if (index != sizeof(Find)) { continue; }
            if (found) { return false; }
            offset = pos;
            found = true;
        }
        return found;
    }
}
