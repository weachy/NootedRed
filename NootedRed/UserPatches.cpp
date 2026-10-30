// User-Space Driver Patches
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <DeviceProfile.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatches.hpp>
#include <TahoePatchData.hpp>
#include <UserSubmitGuard.hpp>
#include <VCNBridge.hpp>
#include <libkern/OSAtomic.h>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_types.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <kern/task.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/vnode.h>

static UserPatches moduleInstance;

extern "C" {
    int proc_pid(proc_t);
    void proc_name(int, char*, int);
    proc_t current_proc(void);
}

namespace
{

    const char* const kBootArgNoGLFix          = "-NRedNoGLFix";
    const char* const kBootArgGLFixOff         = "-NRedGLFixOff";
    const char* const kBootArgGLFixResolve     = "-NRedGLFixResolve";
    const char* const kBootArgGLFixResolveOnly = "-NRedGLFixResolveOnly";
    const char* const kBootArgGLFixResolveOff  = "-NRedGLFixResolveOff";
    const char* const kBootArgGLFixNoResolve   = "-NRedGLFixNoResolve";
    const char* const kBootArgNoVDA            = "-NRedNoVDA";
    const char* const kBootArgNoHEVC           = "-NRedNoHEVC";
    const char* const kBootArgNoSubmitGuard    = "-NRedNoSubmitGuard";
    const char* const kBootArgSubmitGuard      = "-NRedSubmitGuard";
    const char* const kBootArgNoLegacyBlend    = "-NRedNoLegacyBlend";
    const char* const kBootArgLegacyBlend      = "-NRedLegacyBlend";

    struct SharedRegionCheckArgs
    {
        user_addr_t startAddress;
    };
    static_assert(sizeof(SharedRegionCheckArgs) == 8);
    using SharedRegionCheck = int (*)(proc_t, SharedRegionCheckArgs*, int*);

    bool isFairPlayClient(const task_t task)
    {
        if (task == nullptr) { return false; }
        auto* entitlement = IOUserClient::copyClientEntitlement(task, "com.apple.private.fpsd.client");
        const bool client = (entitlement == kOSBooleanTrue);
        if (entitlement != nullptr) { entitlement->release(); }
        return client;
    }

    using GetTaskMap = vm_map_t (*)(task_t);
    using RegionRecurse = kern_return_t (*)(vm_map_t, mach_vm_address_t*, mach_vm_size_t*, natural_t*,
                                            vm_region_recurse_info_t, mach_msg_type_number_t*);
    using CopyIn = int (*)(const void*, void*, size_t);
    using CopyOut = int (*)(const void*, void*, size_t);

    GetTaskMap    getTaskMapFunc{nullptr};
    RegionRecurse queryRegionFunc{nullptr};
    CopyIn        copyInFunc{nullptr};
    CopyOut       copyOutFunc{nullptr};

    bool leafProtection(vm_map_t map, const mach_vm_address_t page, vm_prot_t& protection)
    {
        if (queryRegionFunc == nullptr) { return false; }
        natural_t depth{0};
        for (UInt32 attempt = 0; attempt < 8; ++attempt) {
            auto address = page;
            mach_vm_size_t size{0};
            vm_region_submap_short_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
            const auto result = queryRegionFunc(map, &address, &size, &depth,
                                                reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
            if (result != KERN_SUCCESS) { return false; }
            if (count < VM_REGION_SUBMAP_SHORT_INFO_COUNT_64 || address > page || page - address >= size
                || size - (page - address) < PAGE_SIZE) { return false; }
            if (!info.is_submap) { protection = info.protection; return true; }
            if (depth == ~0U) { break; }
            ++depth;
        }
        return false;
    }

    // The patch bytes and page images below are specific to one driver build, and the native backend hooks
    // code signing page validation. Enabled by default; pass -NRedNoGLFix to opt out.
    constexpr bool kGLFixDefaultEnabled = true;

    // Arbitrary non-zero section id for `UserPatcher::BinaryModPatch`; Lilu's ABI treats zero as disabled.
    constexpr UInt32 kLiluPatchSection = 1;

    // Exact patches for
    // `/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver`.
    //
    // TODO: Populate this table from analysis/gl-hwinfo.md, or call `UserPatches::singleton().registerGLDriverFix`
    // with a table in `NRed::init`.  Each entry needs:
    //  - `find`/`replace`/`size`: the bytes to replace (both backends),
    //  - `skip`/`count`: which occurrences to replace; `count` must be non-zero (both backends),
    //  - `segment`: the Mach segment/section holding the patch (Lilu backend only, `SegmentTextText` for `__text`),
    //  - `page`: the exact 4 KiB file page containing the first `find` (native backend only, macOS 11 and newer).
    //
    // A page image can be extracted with, for example:
    //  - `dd if="$kGLDriverPath" of=page.bin bs=4096 skip=$((0xdf000 / 0x1000)) count=1` and then `xxd -i page.bin`.
    //
    // Common GL Driver patch patterns (shared across macOS 14, 15, 26)
    // A1: `_gfx9_HwInfoInitData` hardcodes four shader engines in the Vega10 branch. Renoir has one
    // shader engine, so `rec+0x64 = rec+0x60 * rec+0x78 * rec+0x80` becomes 32 instead of 8 compute
    // units. Site: file offset 0xE081 = page 0xE000 + 0x81.
    static const UInt8 kGLDriverHwInfoFind[]    = {0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00};
    static const UInt8 kGLDriverHwInfoReplace[] = {0xC7, 0x47, 0x78, 0x01, 0x00, 0x00, 0x00};

    // A2: `_glrAMD_GFX9_UpdateHwCbDualQuadMode` must set `CB_COLOR_CONTROL.DISABLE_DUAL_QUAD` on
    // the iGPU. The original code computes the bit from `rec+0xA0` bit 1, which is set only for the
    // Vega12 branch (0x4F); Renoir takes the Vega10 branch (0x4D), so dual-source blending drops the
    // draw or resets the GPU. Force `or eax, 1` instead of `and eax, ~1`.
    // Site: file offset 0x8160 = page 0x8000 + 0x160; the same 3-byte sequence also occurs at
    // page offset 0x68, hence skip = 1.
    static const UInt8 kGLDriverDualQuadFind[]    = {0x83, 0xE0, 0xFE};
    static const UInt8 kGLDriverDualQuadReplace[] = {0x83, 0xC8, 0x01};

    // Metal counterpart: Gate pattern (jnz -> jmp) for macOS 14 & 15
    static const UInt8 kMTLGateFind[]    = {0x75, 0x0B};
    static const UInt8 kMTLGateReplace[] = {0xEB, 0x0B};

    // macOS 14 (Sonoma) Allow pattern: 0F 93 C0 (setae al) -> 31 C0 90 (xor eax,eax; nop)
    static const UInt8 kMTLAllowFind_Sonoma[]    = {0x0F, 0x93, 0xC0};
    static const UInt8 kMTLAllowReplace_Sonoma[] = {0x31, 0xC0, 0x90};

    // macOS 15 (Sequoia) Allow pattern: 0F 94 C0 (setz al) -> 31 C0 90 (xor eax,eax; nop)
    static const UInt8 kMTLAllowFind_Sequoia[]    = {0x0F, 0x94, 0xC0};
    static const UInt8 kMTLAllowReplace_Sequoia[] = {0x31, 0xC0, 0x90};

    // macOS 26 (Tahoe) Gate: 74 28 (je +0x28) -> EB 00 (jmp +0x00, fallthrough / no-op jump)
    static const UInt8 kMTLGateFind_Tahoe[]    = {0x74, 0x28};
    static const UInt8 kMTLGateReplace_Tahoe[] = {0xEB, 0x00};

    // macOS 26 (Tahoe) Allow pattern: 0F 94 C1 (sete cl) -> 31 C9 90 (xor ecx,ecx; nop)
    static const UInt8 kMTLAllowFind_Tahoe[]    = {0x0F, 0x94, 0xC1};
    static const UInt8 kMTLAllowReplace_Tahoe[] = {0x31, 0xC9, 0x90};

    #include "UserPatchesVentura.inc"
    #include "UserPatchesSonoma.inc"
    #include "UserPatchesSequoia.inc"
    #include "UserPatchesTahoe.inc"
    #include "VDASignatures.inc"

    static const UInt8 kResolverMask32[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const UInt8 kResolverMask32_RipMasked[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };

    // macOS 14 (Sonoma) resolver contexts.
    static const UInt8 kResolverHwCtx_Sonoma[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0xF4, 0x19, 0x0E, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Sonoma[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Sonoma[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x48, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x48, 0x8B, 0x9D, 0x78, 0xFE, 0xFF, 0xFF, 0xEB, 0x1D, 0x80, 0xBD, 0x80,
    };
    static const UInt8 kResolverAllowCtx_Sonoma[] = {
        0x95, 0xC1, 0x48, 0x8B, 0x9D, 0x78, 0xFE, 0xFF, 0xFF, 0x4A, 0x0F, 0xBA, 0x64, 0x33, 0x08, 0x22,
        0x0F, 0x93, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x70, 0xFE, 0xFF, 0xFF, 0x0F, 0xB6, 0xC0, 0x41,
    };
    // Mask stack displacements (bytes 5-8, 23-26) to wildcards for cross-minor Sonoma robustness
    static const UInt8 kResolverMask32_GateSonoma[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_AllowSonoma[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Sonoma[] = {
        {kResolverHwCtx_Sonoma, kResolverMask32_RipMasked, sizeof(kResolverHwCtx_Sonoma), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverDualCtx_Sonoma, kResolverMask32, sizeof(kResolverDualCtx_Sonoma), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverGateCtx_Sonoma, kResolverMask32_GateSonoma, sizeof(kResolverGateCtx_Sonoma), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverAllowCtx_Sonoma, kResolverMask32_AllowSonoma, sizeof(kResolverAllowCtx_Sonoma), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0)},
    };
    constexpr size_t kResolverSignatureCount_Sonoma = arrsize(kResolverSignatures_Sonoma);

    // macOS 15 (Sequoia) resolver contexts.
    static const UInt8 kResolverHwCtx[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x50, 0xFE, 0x0D, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x28, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x4C, 0x8B, 0x9D, 0x80, 0xFE, 0xFF, 0xFF, 0xEB, 0x2C, 0x83, 0xBD, 0x70,
    };
    static const UInt8 kResolverSetzCtx[] = {
        0xFF, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x49, 0x85, 0x54, 0x03, 0x08,
        0x0F, 0x94, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x88, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0xB5, 0x50,
    };
    static const UInt8 kResolverMask32_GateSequoia[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_SetzSequoia[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00,
    };

    // Resolver fallback for the 15.8.1 GL/Metal sites.
    static const UserPatches::SignaturePatch kResolverSignatures[] = {
        {kResolverHwCtx, kResolverMask32_RipMasked, sizeof(kResolverHwCtx), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverDualCtx, kResolverMask32, sizeof(kResolverDualCtx), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverGateCtx, kResolverMask32_GateSequoia, sizeof(kResolverGateCtx), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverSetzCtx, kResolverMask32_SetzSequoia, sizeof(kResolverSetzCtx), 16, kMTLAllowReplace_Sequoia,
         sizeof(kMTLAllowReplace_Sequoia), 0, 1, ~static_cast<UInt64>(0)},
    };
    constexpr size_t kResolverSignatureCount = arrsize(kResolverSignatures);

    // macOS 26 (Tahoe) resolver contexts.
    // The rip-relative displacement at bytes 27-30 is masked to wildcards for forward-compatibility across minor builds.
    static const UInt8 kResolverMask32_HwTahoe[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };

    static const UInt8 kResolverHwCtx_Tahoe[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x50, 0xF3, 0x0D, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Tahoe[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Tahoe[] = {
        0xFF, 0xFF, 0xF6, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x02, 0x48, 0x8B, 0x9D, 0x70, 0xFE, 0xFF, 0xFF,
        0x74, 0x28, 0x80, 0xBD, 0x80, 0xFE, 0xFF, 0xFF, 0x00, 0x0F, 0x95, 0xC0, 0x48, 0x8B, 0x8D, 0x78,
    };
    static const UInt8 kResolverAllowCtx_Tahoe[] = {
        0xFF, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x48, 0x85, 0x54, 0x0B, 0x08,
        0x0F, 0x94, 0xC1, 0x08, 0xC1, 0x0F, 0xB6, 0xC1, 0x41, 0x8B, 0x4C, 0x24, 0x28, 0x83, 0xE1, 0xFE,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Tahoe[] = {
        {kResolverHwCtx_Tahoe, kResolverMask32_HwTahoe, sizeof(kResolverHwCtx_Tahoe), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverDualCtx_Tahoe, kResolverMask32, sizeof(kResolverDualCtx_Tahoe), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverGateCtx_Tahoe, kResolverMask32, sizeof(kResolverGateCtx_Tahoe), 16, kMTLGateReplace_Tahoe,
         sizeof(kMTLGateReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverAllowCtx_Tahoe, kResolverMask32, sizeof(kResolverAllowCtx_Tahoe), 16, kMTLAllowReplace_Tahoe,
         sizeof(kMTLAllowReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0)},
    };
    constexpr size_t kResolverSignatureCount_Tahoe = arrsize(kResolverSignatures_Tahoe);

    // macOS 13 (Ventura) resolver contexts.
    static const UInt8 kResolverHwCtx_Ventura[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF9, 0x13, 0x77, 0x4D, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x06, 0x0B, 0x0F, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Ventura[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Ventura[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x40, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x4C, 0x8B, 0xA5, 0x70, 0xFE, 0xFF, 0xFF, 0xEB, 0x24, 0x80, 0xBD, 0x80,
    };
    static const UInt8 kResolverAllowCtx_Ventura[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x60, 0xFE, 0xFF, 0xFF, 0x49, 0x0F, 0xBA, 0x64, 0x04, 0x08, 0x22,
        0x0F, 0x93, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x50, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0x8D, 0x68,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Ventura[] = {
        {kResolverHwCtx_Ventura, kResolverMask32_RipMasked, sizeof(kResolverHwCtx_Ventura), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverDualCtx_Ventura, kResolverMask32, sizeof(kResolverDualCtx_Ventura), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverGateCtx_Ventura, kResolverMask32, sizeof(kResolverGateCtx_Ventura), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0)},
        {kResolverAllowCtx_Ventura, kResolverMask32, sizeof(kResolverAllowCtx_Ventura), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0)},
    };
    constexpr size_t kResolverSignatureCount_Ventura = arrsize(kResolverSignatures_Ventura);

    static const UserPatches::PagePatch kGLDriverPatches_Ventura[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Ventura,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Ventura,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Ventura,        kMTLGateFind,
         kMTLGateReplace,                           sizeof(kMTLGateFind),          0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Ventura,        kMTLAllowFind_Sonoma,
         kMTLAllowReplace_Sonoma,                   sizeof(kMTLAllowFind_Sonoma),  0, 1},
    };
    constexpr size_t kGLDriverPatchCount_Ventura = arrsize(kGLDriverPatches_Ventura);

    static const UserPatches::PagePatch kGLDriverPatches_Sonoma[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Sonoma,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Sonoma,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sonoma,        kMTLGateFind,
         kMTLGateReplace,                           sizeof(kMTLGateFind),          0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sonoma,        kMTLAllowFind_Sonoma,
         kMTLAllowReplace_Sonoma,                   sizeof(kMTLAllowFind_Sonoma),  0, 1},
    };
    constexpr size_t kGLDriverPatchCount_Sonoma = arrsize(kGLDriverPatches_Sonoma);

    static const UserPatches::PagePatch kGLDriverPatches_Sequoia[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Sequoia,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Sequoia,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sequoia,        kMTLGateFind,
         kMTLGateReplace,                           sizeof(kMTLGateFind),          0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Sequoia,        kMTLAllowFind_Sequoia,
         kMTLAllowReplace_Sequoia,                  sizeof(kMTLAllowFind_Sequoia), 0, 1},
    };
    constexpr size_t kGLDriverPatchCount_Sequoia = arrsize(kGLDriverPatches_Sequoia);

    static const UserPatches::PagePatch kGLDriverPatches_Tahoe[] = {
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverHwInfoPage_Tahoe,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kGLDriverDualQuadPage_Tahoe,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Tahoe,        kMTLGateFind_Tahoe,
         kMTLGateReplace_Tahoe,                     sizeof(kMTLGateFind_Tahoe),    0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kMTLControlPage_Tahoe,        kMTLAllowFind_Tahoe,
         kMTLAllowReplace_Tahoe,                    sizeof(kMTLAllowFind_Tahoe),   1, 1},
    };
    constexpr size_t kGLDriverPatchCount_Tahoe = arrsize(kGLDriverPatches_Tahoe);

    static const UserPatches::PagePatch* const kGLDriverPatches = kGLDriverPatches_Sequoia;
    constexpr size_t kGLDriverPatchCount = kGLDriverPatchCount_Sequoia;

    // macOS 26.7.1 (Darwin 25.6.0) GL driver.  The driver layout moved: _gfx9_HwInfoInitData is at
    // 0xDF80 and _glrAMD_GFX9_UpdateHwCbDualQuadMode at 0x819E, so the 15.8.1 page images cannot match.
    // Only the GL driver is covered here; the Metal 26.7.1 sites still need validation.
    static const UserPatches::PagePatch kTahoeGLDriverPatches[] = {
        {UserPatcher::FileSegment::SegmentTextText, kTahoeGLHwInfoPage,    kGLDriverHwInfoFind,
         kGLDriverHwInfoReplace,                    sizeof(kGLDriverHwInfoFind),   0, 1},
        {UserPatcher::FileSegment::SegmentTextText, kTahoeGLDualQuadPage,  kGLDriverDualQuadFind,
         kGLDriverDualQuadReplace,                  sizeof(kGLDriverDualQuadFind), 1, 1},
        // The Metal pipeline context moved the CB_COLOR_CONTROL boolean write to +0xE4; patch the
        // direct boolean assignment (and edx,~1 -> or edx,1) rather than the old 15.8.1 gate/setz pair.
        {UserPatcher::FileSegment::SegmentTextText, kTahoeMTLControlPage, kTahoeMTLDualQuadFind,
         kTahoeMTLDualQuadReplace,                  sizeof(kTahoeMTLDualQuadFind), 0, 1},
    };
    constexpr size_t kTahoeGLDriverPatchCount = arrsize(kTahoeGLDriverPatches);

    // Version/family-aware patch sets.  Add one entry per validated macOS build; the exact page images
    // still gate every site, so a set from another build can never modify a mismatched driver page.
    // `15.8.1` is Darwin 24.6.0 (macOS 15.8.1, build 24H32).
    static const UserPatches::PatchSet kGLDriverPatchSets[] = {
        // Family-specific entries first: all current gfx9 APUs share the same page data, but keeping
        // them separate lets a future family diverge without changing the selection code.
        {"15.x / Renoir-family", PenguinWizardry::KernelVersion(24, 0), PenguinWizardry::KernelVersion(24, 99),
         static_cast<UserPatches::ApuMask>(UserPatches::kApuRenoir | UserPatches::kApuRenoirE |
                                           UserPatches::kApuGreenSardine),
         kGLDriverPatches_Sequoia, kGLDriverPatchCount_Sequoia, kResolverSignatures, kResolverSignatureCount},
        {"15.x / Raven-family", PenguinWizardry::KernelVersion(24, 0), PenguinWizardry::KernelVersion(24, 99),
         static_cast<UserPatches::ApuMask>(UserPatches::kApuRaven | UserPatches::kApuRaven2 |
                                           UserPatches::kApuPicasso),
         kGLDriverPatches_Sequoia, kGLDriverPatchCount_Sequoia, kResolverSignatures, kResolverSignatureCount},
        // Generic fallback for the case where APU attributes are not populated yet.  It is deliberately
        // last so a known family always wins.
        {"15.x / gfx9 APU fallback", PenguinWizardry::KernelVersion(24, 0),
         PenguinWizardry::KernelVersion(24, 99), UserPatches::kApuAll, kGLDriverPatches_Sequoia,
         kGLDriverPatchCount_Sequoia, kResolverSignatures, kResolverSignatureCount},
        // macOS 26.x (Tahoe).
        {"26.x / Tahoe gfx9 APU", PenguinWizardry::KernelVersion(25, 0), PenguinWizardry::KernelVersion(25, 99),
         UserPatches::kApuAll, kGLDriverPatches_Tahoe, kGLDriverPatchCount_Tahoe,
         kResolverSignatures_Tahoe, kResolverSignatureCount_Tahoe},
        // macOS 14.x (Sonoma) fallback.
        {"14.x / Sonoma gfx9 APU", PenguinWizardry::KernelVersion(23, 0), PenguinWizardry::KernelVersion(23, 99),
         UserPatches::kApuAll, kGLDriverPatches_Sonoma, kGLDriverPatchCount_Sonoma,
         kResolverSignatures_Sonoma, kResolverSignatureCount_Sonoma},
        // macOS 13.x (Ventura) fallback.
        {"13.x / Ventura gfx9 APU", PenguinWizardry::KernelVersion(22, 0), PenguinWizardry::KernelVersion(22, 99),
         UserPatches::kApuAll, kGLDriverPatches_Ventura, kGLDriverPatchCount_Ventura,
         kResolverSignatures_Ventura, kResolverSignatureCount_Ventura},
    };
    constexpr size_t kGLDriverPatchSetCount = sizeof(kGLDriverPatchSets) / sizeof(kGLDriverPatchSets[0]);

}    // namespace

UserPatches& UserPatches::singleton() { return moduleInstance; }

void UserPatches::init()
{
    // `registerGLDriverFix` may already have been called; do not overwrite a custom table then.
    if (this->patches == nullptr && this->patchCount == 0 && this->patchSets == nullptr) {
        this->patchSets     = kGLDriverPatchSets;
        this->patchSetCount = kGLDriverPatchSetCount;
    }

    this->enabled = kGLFixDefaultEnabled;
    if (checkKernelArgument(kBootArgNoGLFix) || checkKernelArgument(kBootArgGLFixOff)) {
        this->enabled = false;
    }

    // The resolver is a GL-only fallback: it must never be active when the GL fix itself is off.
    this->resolverOnly    = this->enabled && checkKernelArgument(kBootArgGLFixResolveOnly);
    this->resolverEnabled = this->enabled
                            && !checkKernelArgument(kBootArgGLFixResolveOff)
                            && !checkKernelArgument(kBootArgGLFixNoResolve);

    this->vdaEnabled = !checkKernelArgument(kBootArgNoVDA);

    this->submitGuardEnabled = checkKernelArgument(kBootArgSubmitGuard) || !checkKernelArgument(kBootArgNoSubmitGuard);
    this->legacyBlendEnabled = checkKernelArgument(kBootArgLegacyBlend) || !checkKernelArgument(kBootArgNoLegacyBlend);

    if (!this->enabled && !this->vdaEnabled && !this->submitGuardEnabled && !this->legacyBlendEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    if (this->enabled) {
        SYSLOG("UserPatch", "Enabling user-space GL driver patches (experimental)");
        if (this->resolverEnabled) {
            SYSLOG("UserPatch", "Resolver fallback enabled%s", this->resolverOnly ? " (resolver-only QA mode)" : "");
        }
    }
    if (this->vdaEnabled) {
        if (getKernelVersion() == KernelVersion::Ventura) {
            this->vdaSignatures     = kVDAResolverSignatures_Ventura;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Ventura;
            this->gvaSignature      = &kGVAResolverSignature_Ventura;
        } else if (getKernelVersion() == KernelVersion::Sonoma) {
            this->vdaSignatures     = kVDAResolverSignatures_Sonoma;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Sonoma;
            this->gvaSignature      = &kGVAResolverSignature_Sonoma;
        } else if (getKernelVersion() >= KernelVersion::Tahoe) {
            this->vdaSignatures     = kVDAResolverSignatures_Modern;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Modern;
            this->gvaSignature      = &kGVAResolverSignature_Tahoe;
        } else {
            this->vdaSignatures     = kVDAResolverSignatures_Modern;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Modern;
            this->gvaSignature      = &kGVAResolverSignature_Sequoia;
        }
        for (auto& applied : this->vdaSignatureApplied) { applied = 0; }
        this->gvaSignatureApplied = 0;
        SYSLOG("UserPatch", "Enabling user-space VDA driver patches with dynamic Resolver fallback");
    }
    if (this->submitGuardEnabled) { SYSLOG("UserPatch", "Enabling user-space Metal SubmitGuard (free tail filler)"); }
    if (this->legacyBlendEnabled) { SYSLOG("UserPatch", "Enabling user-space Metal LegacyBlend (dual-quad fix)"); }
}

void UserPatches::registerGLDriverFix(const PagePatch* const patchTable, const size_t count)
{
    this->patches    = patchTable;
    this->patchCount = count;
    this->patchSets  = nullptr;
    this->patchSetCount = 0;
}

void UserPatches::registerPatchSets(const PatchSet* const sets, const size_t count)
{
    this->patchSets     = sets;
    this->patchSetCount = count;
    this->patches       = nullptr;
    this->patchCount    = 0;
}

auto UserPatches::currentApuMask() const -> ApuMask
{
    const auto& attributes = NRed::singleton().getAttributes();

    UInt8 mask = 0;
    if (attributes.isRaven2()) { mask |= kApuRaven2; }
    if (attributes.isPicasso()) { mask |= kApuPicasso; }
    if (attributes.isRenoir()) { mask |= kApuRenoir; }
    if (attributes.isRenoirE()) { mask |= kApuRenoirE; }
    if (attributes.isGreenSardine()) { mask |= kApuGreenSardine; }

    // NRed leaves the attributes empty for the base Raven (0x15DD); model that explicitly.  A zero
    // device ID means NRed has not populated its device information yet, so return an unknown mask and
    // let the selector use the generic fallback rather than guessing a family.
    if (mask == 0) {
        if (NRed::singleton().getDeviceID() == 0x15DD) { mask = kApuRaven; }
        else if (NRed::singleton().getDeviceID() == 0) { return static_cast<ApuMask>(0); }
    }

    return static_cast<ApuMask>(mask);
}

void UserPatches::selectPatchSet()
{
    if (this->patches != nullptr || this->patchCount != 0) { return; }
    if (this->patchSets == nullptr || this->patchSetCount == 0) { return; }

    const auto version = currentKernelVersion();
    const auto apus    = static_cast<UInt8>(this->currentApuMask());

    for (size_t index = 0; index < this->patchSetCount; index++) {
        const auto& set = this->patchSets[index];
        if (version < set.minVersion || version > set.maxVersion) { continue; }

        // An unknown APU (zero mask) may only use the generic fallback, never a family-specific set.
        if (apus == 0) {
            if (set.apus != UserPatches::kApuAll) { continue; }
        }
        else if ((static_cast<UInt8>(set.apus) & apus) == 0) {
            continue;
        }

        this->patches        = set.patches;
        this->patchCount     = set.count;
        this->signatures     = set.signatures;
        this->signatureCount = set.signatureCount;
        for (auto& applied : this->signatureApplied) { applied = 0; }
        SYSLOG("UserPatch", "Selected patch set \"%s\" for macOS %u.%u / APU mask 0x%02X", set.name,
               version.major(), version.minor(), apus);
        return;
    }

    SYSLOG("UserPatch", "No patch set matches macOS %u.%u / APU mask 0x%02X; trying major kernel version fallback",
           version.major(), version.minor(), apus);

    if (getKernelVersion() >= KernelVersion::Tahoe) {
        SYSLOG("UserPatch", "Fallback: selecting macOS 26 (Tahoe) GL/Metal patch set");
        this->patches        = kGLDriverPatches_Tahoe;
        this->patchCount     = kGLDriverPatchCount_Tahoe;
        this->signatures     = kResolverSignatures_Tahoe;
        this->signatureCount = kResolverSignatureCount_Tahoe;
        for (auto& applied : this->signatureApplied) { applied = 0; }
    } else if (getKernelVersion() >= KernelVersion::Sequoia) {
        SYSLOG("UserPatch", "Fallback: selecting macOS 15 (Sequoia) GL/Metal patch set");
        this->patches        = kGLDriverPatches_Sequoia;
        this->patchCount     = kGLDriverPatchCount_Sequoia;
        this->signatures     = kResolverSignatures;
        this->signatureCount = kResolverSignatureCount;
        for (auto& applied : this->signatureApplied) { applied = 0; }
    } else if (getKernelVersion() >= KernelVersion::Sonoma) {
        SYSLOG("UserPatch", "Fallback: selecting macOS 14 (Sonoma) GL/Metal patch set");
        this->patches        = kGLDriverPatches_Sonoma;
        this->patchCount     = kGLDriverPatchCount_Sonoma;
        this->signatures     = kResolverSignatures_Sonoma;
        this->signatureCount = kResolverSignatureCount_Sonoma;
        for (auto& applied : this->signatureApplied) { applied = 0; }
    } else {
        SYSLOG("UserPatch", "Fallback: selecting macOS 13 (Ventura) GL/Metal patch set");
        this->patches        = kGLDriverPatches_Ventura;
        this->patchCount     = kGLDriverPatchCount_Ventura;
        this->signatures     = kResolverSignatures_Ventura;
        this->signatureCount = kResolverSignatureCount_Ventura;
        for (auto& applied : this->signatureApplied) { applied = 0; }
    }
}

void UserPatches::registerVDAFix(const PagePatch* const patchTable, const size_t count)
{
    if (patchTable == nullptr || count == 0) { return; }

    if (this->vdaTableCount >= arrsize(this->vdaTables)) {
        SYSLOG("UserPatch", "Too many VDA patch tables, ignoring the extra one");
        return;
    }

    this->vdaTables[this->vdaTableCount++] = {patchTable, count};
}

void UserPatches::processPatcher(KernelPatcher& patcher)
{
    if (this->submitGuardEnabled || this->legacyBlendEnabled) {
        this->installSubmitGuard(patcher);
    }

    if (!this->enabled && !this->vdaEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    const auto* const profile = DeviceProfile::current();
    auto              glEnabled = this->enabled;
    if (profile != nullptr) {
        SYSLOG("UserPatch", "Device profile: %s (family %s, level %s)", profile->name,
               DeviceProfile::familyName(profile->family), DeviceProfile::levelName(profile->level));
        if (!profile->graphics.conditionalDualQuad) {
            SYSLOG("UserPatch", "Graphics policy disables the conditional dual-quad fix for this profile");
            glEnabled = false;
        }
    }

    // Pick the version/family-aware GL patch set before the native page list is built from it.
    if (glEnabled) { this->selectPatchSet(); }

    // The resolver is a fallback for the GL patch sites; honour the profile gate for it as well.
    if (!glEnabled) {
        this->resolverEnabled = false;
        this->signatures      = nullptr;
        this->signatureCount  = 0;
    }

    // Build the native page list from whichever tables are enabled.  The VDA tables only exist on macOS 11
    // and newer, where the native backend is the one being used.  The profile is the single source of truth
    // for the target: a VCN 2.x device whose profile enables the engine may receive the user-space VA tables
    // (the VCN 2.x profiles, with 0x1638 as the validated target).
    const auto vdaAllowed = this->vdaEnabled && profile != nullptr && profile->media.hasVCN0
                            && profile->media.vcnGeneration == DeviceProfile::kVcn2_2
                            && !NRed::singleton().hasExternalVideo();
    if (this->vdaEnabled && !vdaAllowed) {
        SYSLOG("UserPatch", "VDA patches requested but this profile does not enable the validated VCN 2.x target");
    }

    this->nativePatchCount = 0;
    for (auto& isVDA : this->nativePatchIsVDA) { isVDA = false; }

    if (currentKernelVersion() >= MACOS_11) {
        auto append = [this](const PagePatch* const table, const size_t count, const bool vda)
        {
            for (size_t i = 0; i < count && this->nativePatchCount < kMaxNativePatches; ++i) {
                this->nativePatchIsVDA[this->nativePatchCount] = vda;
                this->nativePatches[this->nativePatchCount++]   = table[i];
            }
        };

        if (glEnabled) { append(this->patches, this->patchCount, false); }
        if (vdaAllowed) {
            for (size_t i = 0; i < this->vdaTableCount; ++i) {
                append(this->vdaTables[i].table, this->vdaTables[i].count, true);
            }
        }

        if (this->nativePatchCount == 0) {
            SYSLOG("UserPatch", "No user-space driver patches are configured, not installing any hooks");
            return;
        }

        SYSLOG_COND(!this->installNativePagePatcher(patcher), "UserPatch",
                    "Failed to install the native page patch backend");
        return;
    }

    if (!glEnabled || this->patches == nullptr || this->patchCount == 0) {
        SYSLOG("UserPatch", "No user-space GL driver patches are configured, not installing any hooks");
        return;
    }

    SYSLOG_COND(!this->installUserPatcher(), "UserPatch", "Failed to register Lilu's user patches");
}

auto UserPatches::installUserPatcher() -> bool
{
    this->liluPatchCount = 0;

    for (size_t patchIndex = 0; patchIndex < this->patchCount; patchIndex++) {
        const auto& patch = this->patches[patchIndex];

        if (patch.find == nullptr || patch.replace == nullptr || patch.size == 0 || patch.count == 0) { continue; }

        if (this->liluPatchCount >= kMaxLiluPatches) {
            SYSLOG("UserPatch", "Too many patches, only the first %zu are used", kMaxLiluPatches);
            break;
        }

        auto& liluPatch   = this->liluPatches[this->liluPatchCount++];
        liluPatch.cpu     = CPU_TYPE_X86_64;
        liluPatch.flags   = 0;
        liluPatch.find    = patch.find;
        liluPatch.replace = patch.replace;
        liluPatch.size    = patch.size;
        liluPatch.skip    = patch.skip;
        liluPatch.count   = patch.count;
        liluPatch.segment = patch.segment;
        liluPatch.section = kLiluPatchSection;
    }

    if (this->liluPatchCount == 0) {
        SYSLOG("UserPatch", "No valid GL driver patches for Lilu's user patcher");
        return false;
    }

    this->liluModInfo.path      = kGLDriverPath;
    this->liluModInfo.patches   = this->liluPatches;
    this->liluModInfo.count     = this->liluPatchCount;
    this->liluModInfo.startTEXT = 0;
    this->liluModInfo.endTEXT   = 0;
    this->liluModInfo.startDATA = 0;
    this->liluModInfo.endDATA   = 0;

    // No process filter is registered: the patch is a page-content patch, and Lilu applies those from the
    // code signing validation hooks for every image that matches, while a process filter would additionally
    // inject the `__RESTRICT` segment into that process (see analysis/lilu-userpatch.md).  Lilu tolerates a
    // zero-sized process list: `onPath` only iterates over `procInfoSize` entries, and `registerPatches` only
    // derives its minimum path length from the list when it is not empty.
    const auto error = lilu.onProcLoad(nullptr, 0, nullptr, nullptr, &this->liluModInfo, 1);
    if (error != LiluAPI::Error::NoError) {
        SYSLOG("UserPatch", "Failed to register user patches with Lilu (%d)", static_cast<int>(error));
        return false;
    }

    DBGLOG("UserPatch", "Registered %zu page patch(es) for %s", this->liluPatchCount, kGLDriverPath);

    return true;
}

auto UserPatches::installNativePagePatcher(KernelPatcher& patcher) -> bool
{
    if (!this->buildNativePatches()) { return false; }

    KernelPatcher::RouteRequest pageRequest{"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &pageRequest, 1)) {
        SYSLOG("UserPatch", "Failed to route _cs_validate_page (%d)", static_cast<int>(patcher.getError()));
        patcher.clearError();
        return false;
    }

    // The dyld shared cache and Apple protected pagers validate through the range variant.  The GL driver is
    // not part of the shared cache, so this is only registered when available and never treated as fatal.
    KernelPatcher::RouteRequest rangeRequest{"_cs_validate_range", wrapCsValidateRange, this->orgCsValidateRange};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &rangeRequest, 1)) {
        DBGLOG("UserPatch", "Failed to route _cs_validate_range, continuing without it (%d)",
               static_cast<int>(patcher.getError()));
        patcher.clearError();
    }

    DBGLOG("UserPatch", "Installed the native page patch backend for %zu page(s)", this->nativePageCount);

    return true;
}

auto UserPatches::buildNativePatches() -> bool
{
    this->nativePageCount = 0;

    for (size_t patchIndex = 0; patchIndex < this->nativePatchCount; patchIndex++) {
        const auto& patch = this->nativePatches[patchIndex];

        if (patch.find == nullptr || patch.replace == nullptr || patch.size == 0 || patch.count == 0
            || patch.page == nullptr || patch.size > kMaxPatchSize || patch.skip > kMaxSitesPerPage
            || patch.count > kMaxSitesPerPage)
        {
            SYSLOG("UserPatch", "Patch %zu is invalid; refusing the native page table", patchIndex);
            return false;
        }

        // Locate the complete set of sites before creating a page, so a truncated record can never leave a
        // partially patched driver behind.
        size_t offsets[kMaxSitesPerPage]{};
        size_t matched = 0;
        size_t placed  = 0;
        for (size_t offset = 0; offset + patch.size <= kPageSize; offset++) {
            if (memcmp(patch.page + offset, patch.find, patch.size) != 0) { continue; }

            if (matched++ < patch.skip) { continue; }

            offsets[placed++] = offset;
            if (placed == patch.count) { break; }
        }

        if (placed != patch.count) {
            SYSLOG("UserPatch",
                   "Failed to locate all occurrences of patch %zu (%zu missing); refusing the native page table",
                   patchIndex, patch.count - placed);
            return false;
        }

        NativePage* page{nullptr};
        for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
            if (memcmp(this->nativePages[pageIndex].image, patch.page, kPageSize) != 0) { continue; }
            page = &this->nativePages[pageIndex];
            break;
        }

        if (page == nullptr) {
            if (this->nativePageCount >= kMaxNativePages) {
                SYSLOG("UserPatch",
                       "Too many distinct pages for the native backend (%zu); refusing the native page table",
                       kMaxNativePages);
                return false;
            }

            page = &this->nativePages[this->nativePageCount++];
            *page = NativePage{};
            page->image = patch.page;
            memcpy(&page->head, patch.page, sizeof(page->head));
        }

        if (page->siteCount + patch.count > kMaxSitesPerPage) {
            SYSLOG("UserPatch",
                   "Too many patch sites on a single page (%zu + %zu > %zu); refusing the native page table",
                   page->siteCount, patch.count, kMaxSitesPerPage);
            return false;
        }

        page->vda = page->vda || this->nativePatchIsVDA[patchIndex];
        for (size_t siteIndex = 0; siteIndex < patch.count; siteIndex++) {
            auto& site   = page->sites[page->siteCount++];
            site.offset  = offsets[siteIndex];
            site.find    = patch.find;
            site.replace = patch.replace;
            site.size    = patch.size;
        }
    }

    if (this->nativePageCount == 0) {
        SYSLOG("UserPatch", "No usable native page patches, each patch needs a 4096 byte page image and a valid site");
        return false;
    }

    if (this->vdaEnabled) {
        // Make a silent no-match after an OS update diagnosable: if the VCN bridge is ready yet
        // NRedVCNUserApplied stays below this count, the system no longer matches the generated data.
        size_t vdaPages = 0;
        for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
            vdaPages += this->nativePages[pageIndex].vda ? 1 : 0;
        }
        SYSLOG("UserPatch", "VDA: %zu user-space driver page(s) registered; compare NRedVCNUserApplied to confirm",
               vdaPages);
    }

    return true;
}

auto UserPatches::isPatchedPage(const NativePage& page, const UInt8* const data) const -> bool
{
    if (page.image == nullptr || data == nullptr) { return false; }

    for (size_t offset = 0; offset < kPageSize; offset++) {
        const NativeSite* site{nullptr};
        for (size_t siteIndex = 0; siteIndex < page.siteCount; siteIndex++) {
            const auto& candidate = page.sites[siteIndex];
            if (offset >= candidate.offset && offset < candidate.offset + candidate.size) {
                site = &candidate;
                break;
            }
        }

        if (site != nullptr) {
            if (data[offset] != site->replace[offset - site->offset]) { return false; }
        }
        else if (data[offset] != page.image[offset]) {
            return false;
        }
    }

    return true;
}

static bool matchSignature(const UInt8* const bytes, const UserPatches::SignaturePatch& sig, size_t& matchOffset, size_t startOffset = 0)
{
    if (sig.context == nullptr || sig.mask == nullptr || sig.replace == nullptr) { return false; }
    if (sig.contextSize == 0 || sig.contextSize > UserPatches::kPageSize) { return false; }
    if (sig.patchOffset + sig.replaceSize > sig.contextSize) { return false; }

    size_t skipped = 0;
    for (size_t offset = startOffset; offset + sig.contextSize <= UserPatches::kPageSize; offset++) {
        bool matches = true;
        for (size_t index = 0; index < sig.contextSize; index++) {
            if ((bytes[offset + index] & sig.mask[index]) != (sig.context[index] & sig.mask[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (sig.find != nullptr && sig.findSize > 0) {
                if (offset + sig.patchOffset + sig.findSize > UserPatches::kPageSize ||
                    memcmp(bytes + offset + sig.patchOffset, sig.find, sig.findSize) != 0) {
                    continue;
                }
            }

            if (skipped < sig.skip) {
                skipped++;
                continue;
            }
            matchOffset = offset;
            return true;
        }
    }
    return false;
}

void UserPatches::patchSignaturePage(const void* const data, const UInt64 pageOffset, void* const vnode)
{
    if (data == nullptr || vnode == nullptr) {
        return;
    }

    // Gate 1: Vnode verification.
    // Ensure the page strictly belongs to graphics/media drivers or the dyld shared cache.
    // Never scan or modify random user binaries or daemons (such as NotificationCenter).
    const char* const vnodeName = vnode_getname(reinterpret_cast<vnode_t>(vnode));
    if (vnodeName == nullptr) {
        return;
    }

    const bool isGLDriver    = (strstr(vnodeName, "AMDRadeonX5000GLDriver") != nullptr);
    const bool isVADriver    = (strstr(vnodeName, "AMDRadeonVADriver") != nullptr);
    const bool isAppleGVA    = (strstr(vnodeName, "AppleGVA") != nullptr);
    const bool isSharedCache = (strstr(vnodeName, "dyld_shared_cache") != nullptr) ||
                               (strstr(vnodeName, "dyld") != nullptr);
    vnode_putname(vnodeName);

    if (!isGLDriver && !isVADriver && !isAppleGVA && !isSharedCache) {
        return;
    }

    // Process exclusion defense: WindowServer and loginwindow NEVER decode video via VDA
    // and must never have their shared cache pages scanned or modified by VDA resolver.
    bool isWindowServerOrLogin = false;
    char procName[64]{};
    int pid = 0;
    if (isSharedCache) {
        proc_t proc = current_proc();
        pid = proc != nullptr ? proc_pid(proc) : 0;
        if (pid > 0) { proc_name(pid, procName, sizeof(procName)); }
        if (strcmp(procName, "WindowServer") == 0 || strcmp(procName, "loginwindow") == 0) {
            isWindowServerOrLogin = true;
        }
    }

    const auto* const bytes = static_cast<const UInt8*>(data);

    // VDA Dynamic Resolver path (AMDRadeonVADriver2: loose bundle or dyld_shared_cache)
    if (!isWindowServerOrLogin && (isVADriver || isSharedCache) && this->vdaEnabled && this->vdaSignatures != nullptr && this->vdaSignatureCount > 0) {
        if (VCNBridge::singleton().loadReady() && !isFairPlayClient(current_task())) {
            bool vdaAllApplied = true;
            for (size_t i = 0; i < this->vdaSignatureCount && i < kMaxVDASignatures; ++i) {
                if (this->vdaSignatureApplied[i] < this->vdaSignatures[i].count) {
                    vdaAllApplied = false;
                    break;
                }
            }

            if (!vdaAllApplied) {
                for (size_t sigIdx = 0; sigIdx < this->vdaSignatureCount; sigIdx++) {
                    const auto& sig = this->vdaSignatures[sigIdx];
                    if (sigIdx < kMaxVDASignatures && this->vdaSignatureApplied[sigIdx] >= sig.count) {
                        continue;
                    }

                    size_t matchOffset = 0;
                    size_t searchOffset = 0;
                    while (sigIdx < kMaxVDASignatures && this->vdaSignatureApplied[sigIdx] < sig.count &&
                           matchSignature(bytes, sig, matchOffset, searchOffset)) {
                        // Layer 4: Page boundary verification
                        if (matchOffset + sig.patchOffset + sig.replaceSize > kPageSize) {
                            SYSLOG("UserPatch", "Resolver: VDA patch out of page bounds (0x%zX + 0x%zX + 0x%zX)",
                                   matchOffset, sig.patchOffset, sig.replaceSize);
                            break;
                        }

                        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                            SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (VDA)");
                            return;
                        }

                        memcpy(const_cast<UInt8*>(bytes) + matchOffset + sig.patchOffset, sig.replace, sig.replaceSize);

                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                        SYSLOG("UserPatch", "Resolver patched VDA signature %zu (site %zu/%zu) at page offset 0x%zX",
                               sigIdx, this->vdaSignatureApplied[sigIdx] + 1, sig.count, matchOffset);
                        if (sigIdx < kMaxVDASignatures) {
                            this->vdaSignatureApplied[sigIdx]++;
                        }
                        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
                        NRed::singleton().setProp32("NRedVCNUserApplied", applied);

                        searchOffset = matchOffset + 1;
                    }
                }
            }
        }
        if (isVADriver) {
            return;
        }
    }

    // AppleGVA Dynamic Resolver path (dyld_shared_cache or standalone AppleGVA bundle)
    if (!isWindowServerOrLogin && (isAppleGVA || isSharedCache) && this->vdaEnabled && this->gvaSignature != nullptr &&
        this->gvaSignatureApplied < this->gvaSignature->count) {
        if (!isFairPlayClient(current_task())) {
            size_t matchOffset = 0;
            if (matchSignature(bytes, *this->gvaSignature, matchOffset)) {
                if (matchOffset + this->gvaSignature->patchOffset + this->gvaSignature->replaceSize <= kPageSize) {
                    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                        memcpy(const_cast<UInt8*>(bytes) + matchOffset + this->gvaSignature->patchOffset,
                               this->gvaSignature->replace, this->gvaSignature->replaceSize);
                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                        this->gvaSignatureApplied++;
                        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
                        NRed::singleton().setProp32("NRedVCNUserApplied", applied);
                        SYSLOG("UserPatch", "Resolver patched AppleGVA signature at page offset 0x%zX", matchOffset);
                    } else {
                        SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (AppleGVA)");
                    }
                }
            }
        }
    }

    if (!this->resolverEnabled || this->signatures == nullptr || this->signatureCount == 0) {
        return;
    }

    bool allApplied = true;
    for (size_t i = 0; i < this->signatureCount && i < kMaxSignatures; ++i) {
        if (this->signatureApplied[i] < this->signatures[i].count) {
            allApplied = false;
            break;
        }
    }
    if (allApplied) { return; }

    // Gate 2: GL patches ONLY apply to AMDRadeonX5000GLDriver; Metal patches ONLY apply to dyld shared cache.
    if (isGLDriver) {
        for (size_t signatureIndex = 0; signatureIndex < 2 && signatureIndex < this->signatureCount; signatureIndex++) {
            const auto& signature = this->signatures[signatureIndex];
            if (signatureIndex < kMaxSignatures && this->signatureApplied[signatureIndex] >= signature.count) {
                continue;
            }

            size_t matchOffset = 0;
            if (matchSignature(bytes, signature, matchOffset)) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                    SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (GL)");
                    return;
                }

                memcpy(const_cast<UInt8*>(bytes) + matchOffset + signature.patchOffset, signature.replace,
                       signature.replaceSize);

                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                SYSLOG("UserPatch", "Resolver patched GL signature %zu at page offset 0x%zX", signatureIndex, matchOffset);
                if (signatureIndex < kMaxSignatures) {
                    this->signatureApplied[signatureIndex]++;
                }
                static volatile UInt32 glResolverCounter{0};
                const auto glCount = static_cast<UInt32>(OSIncrementAtomic(&glResolverCounter) + 1);
                NRed::singleton().setProp32("NRedGLResolverApplied", glCount);
            }
        }
    } else if (isSharedCache) {
        // Gate 3: For Metal signatures in dyld shared cache, require co-occurrence of Gate and Allow
        // on the exact same 4KB page, within a sane relative distance (< 0x500 bytes).
        // Signatures index 2 & 3 in all tables (Sonoma, Sequoia, Tahoe) represent the Gate and Allow pair.
        if (this->signatureCount >= 4) {
            const auto& gateSig = this->signatures[2];
            const auto& allowSig = this->signatures[3];

            if (gateSig.pageOffset == ~static_cast<UInt64>(0) && allowSig.pageOffset == ~static_cast<UInt64>(0)) {
                const bool needGate = (2 >= kMaxSignatures || this->signatureApplied[2] < gateSig.count);
                const bool needAllow = (3 >= kMaxSignatures || this->signatureApplied[3] < allowSig.count);

                if (needGate && needAllow) {
                    size_t gateMatch = 0;
                    size_t allowMatch = 0;
                    if (matchSignature(bytes, gateSig, gateMatch) && matchSignature(bytes, allowSig, allowMatch)) {
                        long diff = static_cast<long>(allowMatch) - static_cast<long>(gateMatch);
                        if (diff < 0) { diff = -diff; }

                        if (diff < 0x500) {
                            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                                SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (Metal)");
                                return;
                            }

                            memcpy(const_cast<UInt8*>(bytes) + gateMatch + gateSig.patchOffset, gateSig.replace,
                                   gateSig.replaceSize);
                            memcpy(const_cast<UInt8*>(bytes) + allowMatch + allowSig.patchOffset, allowSig.replace,
                                   allowSig.replaceSize);

                            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                            SYSLOG("UserPatch", "Resolver matched and patched Metal Gate (0x%zX) and Allow (0x%zX) atomically",
                                   gateMatch, allowMatch);
                            if (2 < kMaxSignatures) { this->signatureApplied[2]++; }
                            if (3 < kMaxSignatures) { this->signatureApplied[3]++; }

                            static volatile UInt32 blendResolverCounter{0};
                            const auto count = static_cast<UInt32>(OSIncrementAtomic(&blendResolverCounter) + 1);
                            NRed::singleton().setProp32("NRedLegacyBlendApplied", count);
                            if (strcmp(procName, "WindowServer") == 0) {
                                NRed::singleton().setProp32("NRedLegacyBlendWindowServerPID", static_cast<UInt32>(pid));
                                NRed::singleton().setProp32("NRedLegacyBlendWindowServerApplied", 1);
                            }
                        }
                    }
                }
            }
        }
    }
}

void UserPatches::patchNativePage(const void* const data, const UInt64 pageOffset, void* const vnode)
{
    if (data == nullptr) { return; }

    // Gate 0: Vnode verification.
    // Ensure the page strictly belongs to supported graphics/media drivers or the dyld shared cache.
    // Random user applications or system daemons (such as NotificationCenter) are discarded immediately.
    if (vnode != nullptr) {
        const char* const vnodeName = vnode_getname(reinterpret_cast<vnode_t>(vnode));
        if (vnodeName != nullptr) {
            const bool isGLDriver = (strstr(vnodeName, "AMDRadeonX5000GLDriver") != nullptr);
            const bool isVADriver = (strstr(vnodeName, "AMDRadeonVADriver") != nullptr);
            const bool isAppleGVA = (strstr(vnodeName, "AppleGVA") != nullptr);
            const bool isSharedCache = (strstr(vnodeName, "dyld_shared_cache") != nullptr) ||
                                       (strstr(vnodeName, "dyld") != nullptr);
            vnode_putname(vnodeName);

            if (!isGLDriver && !isVADriver && !isAppleGVA && !isSharedCache) {
                return;
            }
        }
    }

    // Test mode: bypass the safe exact-page path and exercise the resolver.  It is intentionally separated
    // by a boot argument so it cannot be enabled by accident.
    if (this->resolverOnly) {
        this->patchSignaturePage(data, pageOffset, vnode);
        return;
    }

    for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
        const auto& page = this->nativePages[pageIndex];

        // Cheap first-word fingerprint before the full 4 KiB compare: the validation hook runs for every
        // code-signed page, so avoid 12 full-page memcmps for every page.
        UInt64 head{0};
        memcpy(&head, data, sizeof(head));
        if (head != page.head || memcmp(page.image, data, kPageSize) != 0) { continue; }

        // VDA patches are only applied when the VCN bridge actually prepared both drivers and loaded X6000.
        // If it failed (ABI mismatch, X6000 load failure), the stock VA/GVA driver stays in place and decode
        // falls back to software instead of trying to use a non-existing engine.
        if (page.vda && !VCNBridge::singleton().loadReady()) {
            DBGLOG("UserPatch", "VDA page matched but the VCN bridge is not ready; leaving it unpatched");
            continue;
        }

        if (page.vda && isFairPlayClient(current_task())) {
            DBGLOG("UserPatch", "Skipping VDA patches for FairPlay client task");
            continue;
        }

        // The page matches, so these checks can only fail when the table is inconsistent; never patch then.
        for (size_t siteIndex = 0; siteIndex < page.siteCount; siteIndex++) {
            const auto& site = page.sites[siteIndex];
            if (memcmp(static_cast<const UInt8*>(data) + site.offset, site.find, site.size) != 0) {
                SYSLOG("UserPatch", "Page matched but a patch site did not, skipping the page");
                return;
            }
        }

        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Failed to disable kernel write protection");
            return;
        }

        for (size_t siteIndex = 0; siteIndex < page.siteCount; siteIndex++) {
            const auto& site = page.sites[siteIndex];
            memcpy(const_cast<UInt8*>(static_cast<const UInt8*>(data)) + site.offset, site.replace, site.size);
        }

        if (MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Failed to restore kernel write protection");
        }

        DBGLOG("UserPatch", "Patched %zu site(s) in a user-space driver page", page.siteCount);

        if (page.vda) {
            const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
            NRed::singleton().setProp32("NRedVCNUserApplied", applied);
        }

        return;
    }

    // The page may already contain our replacement bytes from an earlier validation (the page cache is
    // shared).  Do not rescan it with the resolver; that would only produce false "signature not found"
    // errors and unnecessary work.
    for (size_t pageIndex = 0; pageIndex < this->nativePageCount; pageIndex++) {
        if (this->isPatchedPage(this->nativePages[pageIndex], static_cast<const UInt8*>(data))) { return; }
    }

    this->patchSignaturePage(data, pageOffset, vnode);
}

void UserPatches::wrapCsValidatePage(void* const vnode, void* const pager, const UInt64 pageOffset,
                                     const void* const data, int* const validated, int* const tainted, int* const nx)
{
    FunctionCast(wrapCsValidatePage, singleton().orgCsValidatePage)(vnode, pager, pageOffset, data, validated, tainted,
                                                                    nx);

    // Only patch pages that passed validation, so that a rejected page is never turned into a valid-looking one.
    if (validated != nullptr && *validated != 0) { singleton().patchNativePage(data, pageOffset, vnode); }
}

boolean_t UserPatches::wrapCsValidateRange(void* const vnode, void* const pager, const UInt64 pageOffset,
                                           const void* const data, const UInt64 dataSize, unsigned* const tainted)
{
    const auto result = FunctionCast(wrapCsValidateRange, singleton().orgCsValidateRange)(vnode, pager, pageOffset,
                                                                                          data, dataSize, tainted);

    if (result == 0 || data == nullptr) { return result; }

    const auto* const bytes = static_cast<const UInt8*>(data);
    for (size_t offset = 0; offset + kPageSize <= dataSize; offset += kPageSize) {
        singleton().patchNativePage(bytes + offset, pageOffset + offset, vnode);
    }

    return result;
}

int UserPatches::wrapSharedRegionCheck(proc_t process, void* args, int* returnValue)
{
    auto& patches = singleton();
    const auto result = FunctionCast(wrapSharedRegionCheck, patches.orgSharedRegionCheck)(process, args, returnValue);
    if (result == 0 && args != nullptr) {
        if (patches.submitGuardEnabled) {
            patches.applySubmitGuard(process, args);
        } else if (patches.legacyBlendEnabled) {
            mach_vm_offset_t base = UserSubmitGuardData::kDyldSharedCacheBase;
            const auto* rArgs = static_cast<const SharedRegionCheckArgs*>(args);
            if (rArgs->startAddress != 0 && copyInFunc != nullptr) {
                mach_vm_offset_t userBase{0};
                if (copyInFunc(reinterpret_cast<const void*>(rArgs->startAddress), &userBase, sizeof(userBase)) == 0 && userBase != 0) {
                    base = userBase;
                }
            }
            auto map = getTaskMapFunc ? getTaskMapFunc(current_task()) : nullptr;
            if (map != nullptr) {
                patches.applyLegacyBlend(map, base, process);
            }
        }
    }
    return result;
}

void UserPatches::applySubmitGuard(proc_t process, void* rawArgs)
{
    if (getTaskMapFunc == nullptr || copyInFunc == nullptr || copyOutFunc == nullptr) {
        return;
    }

    const auto kernelMajor = currentKernelVersion().major();
    const auto methodOffset = UserSubmitGuardData::getMethodOffset(kernelMajor);
    if (methodOffset == 0) {
        return;
    }

    auto map = getTaskMapFunc(current_task());
    if (map == nullptr) {
        return;
    }

    mach_vm_offset_t base = UserSubmitGuardData::kDyldSharedCacheBase;
    if (rawArgs != nullptr) {
        const auto* args = static_cast<const SharedRegionCheckArgs*>(rawArgs);
        if (args->startAddress != 0) {
            mach_vm_offset_t userBase{0};
            if (copyInFunc(reinterpret_cast<const void*>(args->startAddress), &userBase, sizeof(userBase)) == 0 && userBase != 0) {
                base = userBase;
            }
        }
    }

    const auto method = base + methodOffset;
    UInt8 entry[UserSubmitGuardData::MethodPatchSize];
    if (copyInFunc(reinterpret_cast<const void*>(method), entry, sizeof(entry)) != 0) {
        return;
    }

    static constexpr UInt8 patchedPrefix[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    if (memcmp(entry, patchedPrefix, sizeof(patchedPrefix)) == 0) {
        // Already patched in this process space.
        return;
    }

    if (memcmp(entry, UserSubmitGuardData::MethodOriginal, sizeof(entry)) != 0) {
        // Entry does not match expected prologue; safely avoid touching.
        return;
    }

    vm_address_t page{0};
    auto result = vm_allocate(map, &page, PAGE_SIZE, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS) {
        return;
    }

    UInt8 payload[sizeof(UserSubmitGuardData::Payload)];
    memcpy(payload, UserSubmitGuardData::Payload, sizeof(payload));
    const UInt64 continuation = method + UserSubmitGuardData::MethodPatchSize;
    memcpy(payload + UserSubmitGuardData::PayloadJumpOffset, &continuation, sizeof(continuation));

    if (copyOutFunc(payload, reinterpret_cast<void*>(page), sizeof(payload)) != 0) {
        vm_deallocate(map, page, PAGE_SIZE);
        return;
    }

    result = vm_protect(map, page, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (result != KERN_SUCCESS) {
        vm_deallocate(map, page, PAGE_SIZE);
        return;
    }

    UInt8 patch[UserSubmitGuardData::MethodPatchSize];
    memset(patch, 0x90, sizeof(patch));
    memcpy(patch, patchedPrefix, sizeof(patchedPrefix));
    const UInt64 target = page;
    memcpy(patch + 6, &target, sizeof(target));

    const UInt64 methodPage = method & ~static_cast<UInt64>(PAGE_SIZE - 1);
    vm_prot_t protection{VM_PROT_NONE};
    if (!leafProtection(map, methodPage, protection)
        || protection != (VM_PROT_READ | VM_PROT_EXECUTE)) {
        vm_deallocate(map, page, PAGE_SIZE);
        return;
    }

    result = vm_protect(map, methodPage, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (result != KERN_SUCCESS) {
        vm_deallocate(map, page, PAGE_SIZE);
        return;
    }

    const auto written = copyOutFunc(patch, reinterpret_cast<void*>(method), sizeof(patch));

    // Restore protections: first current, then maximum
    auto restored = vm_protect(map, methodPage, PAGE_SIZE, FALSE, protection);
    if (restored != KERN_SUCCESS) {
        restored = vm_protect(map, methodPage, PAGE_SIZE, FALSE, protection);
    }
    if (restored == KERN_SUCCESS) {
        const auto maxRestored = vm_protect(map, methodPage, PAGE_SIZE, TRUE, protection);
        if (maxRestored != KERN_SUCCESS) {
            restored = maxRestored;
            SYSLOG("UserPatch", "Failed to restore maximum protection for submit method page (0x%X)", maxRestored);
        }
    } else {
        SYSLOG("UserPatch", "Failed to restore current protection for submit method page (0x%X)", restored);
    }

    if (written == 0 && restored == KERN_SUCCESS) {
        static volatile UInt32 appliedCounter{0};
        const auto count = static_cast<UInt32>(OSIncrementAtomic(&appliedCounter) + 1);
        NRed::singleton().setProp32("NRedSubmitGuardApplied", count);

        const auto proc = (process != nullptr) ? process : current_proc();
        if (proc != nullptr) {
            const auto pid = proc_pid(proc);
            char name[32]{};
            if (pid > 0) { proc_name(pid, name, sizeof(name)); }
            name[sizeof(name) - 1] = '\0';
            if (strcmp(name, "WindowServer") == 0) {
                NRed::singleton().setProp32("NRedSubmitGuardWindowServerPID", static_cast<UInt32>(pid));
                NRed::singleton().setProp32("NRedSubmitGuardWindowServerApplied", 1);
            }
            DBGLOG("UserPatch", "SubmitGuard installed in %s (PID %d)", name, pid);
        }
    }

    // Also apply Metal LegacyBlend / dual-quad fix to this dyld shared cache instance
    if (this->legacyBlendEnabled) {
        this->applyLegacyBlend(map, base, process);
    }
}

namespace
{
    inline bool isAllowOpcode(UInt8 b0, UInt8 b1, UInt8 b2)
    {
        return (b0 == 0x31 && b1 == 0xC0 && b2 == 0x90) ||
               (b0 == 0x0F && (b1 == 0x93 || b1 == 0x94) && b2 == 0xC0);
    }

    inline bool isAllowAlreadyPatched(UInt8 b0, UInt8 b1, UInt8 b2)
    {
        return (b0 == 0xB0 && b1 == 0x01 && b2 == 0x90);
    }
}

void UserPatches::applyLegacyBlend(vm_map_t map, mach_vm_offset_t base, proc_t process)
{
    if (!this->legacyBlendEnabled) {
        return;
    }
    if (copyInFunc == nullptr || copyOutFunc == nullptr) {
        return;
    }

    const auto kernelMajor = currentKernelVersion().major();
    mach_vm_offset_t baseGateOffset = 0;
    mach_vm_offset_t baseAllowOffset = 0;
    UInt32 expectedDelta = 0;

    if (kernelMajor == 22) {
        // macOS 13 Ventura (Darwin 22)
        baseGateOffset = 0x3000BC24CULL;
        baseAllowOffset = 0x3000BC278ULL;
        expectedDelta = 0x2C;
    } else if (kernelMajor == 23) {
        // macOS 14 Sonoma (Darwin 23)
        baseGateOffset = 0x30354D1BCULL;
        baseAllowOffset = 0x30354D1DBULL;
        expectedDelta = 0x1F;
    } else if (kernelMajor == 24) {
        // macOS 15 Sequoia (Darwin 24)
        baseGateOffset = 0x3087B3164ULL;
        baseAllowOffset = 0x3087B3198ULL;
        expectedDelta = 0x34;
    } else if (kernelMajor == 25) {
        // macOS 26 Tahoe (Darwin 25)
        baseGateOffset = 0x310C77708ULL;
        baseAllowOffset = 0;
        expectedDelta = 0;
    } else {
        return;
    }

    mach_vm_offset_t resolvedGateOffset = 0;
    mach_vm_offset_t resolvedAllowOffset = 0;
    bool needsGatePatch = false;
    bool needsAllowPatch = false;
    UInt8 targetGate[2] = {0xEB, 0x0B};
    UInt8 targetAllow[3] = {0xB0, 0x01, 0x90};

    if (kernelMajor == 25) {
        targetGate[0] = 0xEB; targetGate[1] = 0x00;
    }

    // Step 1: Fast path - test nominal hardcoded offsets first
    bool fastPathSuccess = false;
    {
        UInt8 gateBytes[2]{};
        if (copyInFunc(reinterpret_cast<const void*>(base + baseGateOffset), gateBytes, sizeof(gateBytes)) == 0) {
            if (kernelMajor == 22 || kernelMajor == 23 || kernelMajor == 24) {
                if (gateBytes[0] == 0x75 && gateBytes[1] == 0x0B) {
                    UInt8 allowBytes[3]{};
                    if (copyInFunc(reinterpret_cast<const void*>(base + baseAllowOffset), allowBytes, sizeof(allowBytes)) == 0) {
                        if (isAllowOpcode(allowBytes[0], allowBytes[1], allowBytes[2])) {
                            resolvedGateOffset = baseGateOffset;
                            resolvedAllowOffset = baseAllowOffset;
                            needsGatePatch = true;
                            needsAllowPatch = true;
                            fastPathSuccess = true;
                        }
                    }
                } else if (gateBytes[0] == 0xEB && gateBytes[1] == 0x0B) {
                    UInt8 allowBytes[3]{};
                    if (copyInFunc(reinterpret_cast<const void*>(base + baseAllowOffset), allowBytes, sizeof(allowBytes)) == 0) {
                        if (isAllowAlreadyPatched(allowBytes[0], allowBytes[1], allowBytes[2])) {
                            return; // Already patched in this shared region instance
                        }
                    }
                }
            } else if (kernelMajor == 25) {
                if (gateBytes[0] == 0x74 && gateBytes[1] == 0x28) {
                    resolvedGateOffset = baseGateOffset;
                    needsGatePatch = true;
                    fastPathSuccess = true;
                } else if (gateBytes[0] == 0xEB && gateBytes[1] == 0x00) {
                    return; // Already patched
                }
            }
        }
    }

    // Step 2: Dynamic Window Search (if fast path didn't match nominal offsets, e.g. due to minor build shifts)
    if (!fastPathSuccess && (kernelMajor == 22 || kernelMajor == 23 || kernelMajor == 24 || kernelMajor == 25)) {
        constexpr size_t kRadius = 128;
        // Ensure kScanSize covers [-128, +128] scan window plus maximum delta and instruction length:
        // 256 + maxDelta (~68) + 3 bytes <= 384 bytes.
        constexpr size_t kScanSize = 256 + 128; // 384 bytes total
        UInt8 win[kScanSize]{};
        const mach_vm_address_t scanBase = base + baseGateOffset - kRadius;

        if (copyInFunc(reinterpret_cast<const void*>(scanBase), win, sizeof(win)) == 0) {
            size_t matchCount = 0;
            mach_vm_offset_t matchedGate = 0;
            mach_vm_offset_t matchedAllow = 0;
            bool matchedAlreadyPatched = false;

            if (kernelMajor == 22 || kernelMajor == 23 || kernelMajor == 24) {
                // Search within [-128, +128] of baseGateOffset
                for (size_t i = 0; i <= 256; ++i) {
                    const bool isGateCandidate = (win[i] == 0x75 && win[i + 1] == 0x0B);
                    const bool isPatchedCandidate = (win[i] == 0xEB && win[i + 1] == 0x0B);
                    if (!isGateCandidate && !isPatchedCandidate) {
                        continue;
                    }

                    // Check allow instruction in allowable delta range [expectedDelta - 16, expectedDelta + 16]
                    const size_t minDelta = (expectedDelta >= 16) ? (expectedDelta - 16) : 0;
                    const size_t maxDelta = expectedDelta + 16;
                    for (size_t d = minDelta; d <= maxDelta; ++d) {
                        const size_t j = i + d;
                        if (j + 3 > kScanSize) {
                            break;
                        }
                        if (isGateCandidate && isAllowOpcode(win[j], win[j + 1], win[j + 2])) {
                            matchedGate = (baseGateOffset - kRadius) + i;
                            matchedAllow = (baseGateOffset - kRadius) + j;
                            matchCount++;
                            break;
                        } else if (isPatchedCandidate && isAllowAlreadyPatched(win[j], win[j + 1], win[j + 2])) {
                            matchedAlreadyPatched = true;
                            matchCount++;
                            break;
                        }
                    }
                }
            } else if (kernelMajor == 25) {
                // macOS 26 Tahoe: single gate patch (74 28 -> EB 00), no allow site
                for (size_t i = 0; i <= 256; ++i) {
                    if (win[i] == 0x74 && win[i + 1] == 0x28) {
                        matchedGate = (baseGateOffset - kRadius) + i;
                        matchCount++;
                    } else if (win[i] == 0xEB && win[i + 1] == 0x00) {
                        matchedAlreadyPatched = true;
                        matchCount++;
                    }
                }
            }

            if (matchCount == 1) {
                if (matchedAlreadyPatched) {
                    return; // Already safely patched
                }
                resolvedGateOffset = matchedGate;
                resolvedAllowOffset = matchedAllow;
                needsGatePatch = true;
                if (resolvedAllowOffset != 0) {
                    needsAllowPatch = true;
                }
                DBGLOG("UserPatch", "LegacyBlend dynamically matched at gate +0x%llX, allow +0x%llX",
                       resolvedGateOffset, resolvedAllowOffset);
            } else {
                // 0 matches or ambiguous (>1 matches). Safely abort without touching dyld cache!
                return;
            }
        } else {
            return;
        }
    }

    // Step 3: Strict "All-or-Nothing" atomic check
    // If either site is missing or unverified, DO NOT touch the shared region!
    if (resolvedAllowOffset != 0) {
        if (!needsGatePatch || !needsAllowPatch) {
            return;
        }
    } else {
        if (!needsGatePatch) {
            return;
        }
    }

    // Step 4: Page protection & atomic write
    const mach_vm_address_t gateAddr = base + resolvedGateOffset;
    const mach_vm_address_t allowAddr = (resolvedAllowOffset != 0) ? (base + resolvedAllowOffset) : 0;

    const mach_vm_address_t gatePage = gateAddr & ~static_cast<mach_vm_address_t>(PAGE_SIZE - 1);
    const mach_vm_address_t allowPage = (allowAddr != 0) ? (allowAddr & ~static_cast<mach_vm_address_t>(PAGE_SIZE - 1)) : 0;

    vm_prot_t gateProt{VM_PROT_NONE};
    if (!leafProtection(map, gatePage, gateProt) || gateProt != (VM_PROT_READ | VM_PROT_EXECUTE)) {
        return;
    }

    vm_prot_t allowProt{VM_PROT_NONE};
    if (allowPage != 0 && allowPage != gatePage) {
        if (!leafProtection(map, allowPage, allowProt) || allowProt != (VM_PROT_READ | VM_PROT_EXECUTE)) {
            return;
        }
    }

    // Backup original gate bytes before writing for rollback on partial failure
    UInt8 orgGate[2]{};
    if (copyInFunc(reinterpret_cast<const void*>(gateAddr), orgGate, sizeof(orgGate)) != 0) {
        return;
    }

    if (vm_protect(map, gatePage, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        return;
    }
    if (allowPage != 0 && allowPage != gatePage) {
        if (vm_protect(map, allowPage, PAGE_SIZE, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
            auto rollback = vm_protect(map, gatePage, PAGE_SIZE, FALSE, gateProt);
            if (rollback == KERN_SUCCESS) {
                vm_protect(map, gatePage, PAGE_SIZE, TRUE, gateProt);
            }
            return;
        }
    }

    int writeErr = 0;
    writeErr = copyOutFunc(targetGate, reinterpret_cast<void*>(gateAddr), sizeof(targetGate));
    if (writeErr == 0 && allowAddr != 0) {
        const int allowErr = copyOutFunc(targetAllow, reinterpret_cast<void*>(allowAddr), sizeof(targetAllow));
        if (allowErr != 0) {
            // Rollback gate patch to maintain all-or-nothing consistency
            copyOutFunc(orgGate, reinterpret_cast<void*>(gateAddr), sizeof(orgGate));
            writeErr = allowErr;
            SYSLOG("UserPatch", "LegacyBlend allow patch failed (0x%X), rolled back gate patch", allowErr);
        }
    }

    auto restoredGate = vm_protect(map, gatePage, PAGE_SIZE, FALSE, gateProt);
    if (restoredGate != KERN_SUCCESS) {
        restoredGate = vm_protect(map, gatePage, PAGE_SIZE, FALSE, gateProt);
    }
    if (restoredGate == KERN_SUCCESS) {
        const auto maxGate = vm_protect(map, gatePage, PAGE_SIZE, TRUE, gateProt);
        if (maxGate != KERN_SUCCESS) {
            restoredGate = maxGate;
            SYSLOG("UserPatch", "Failed to restore maximum protection for gate page (0x%X)", maxGate);
        }
    } else {
        SYSLOG("UserPatch", "Failed to restore current protection for gate page (0x%X)", restoredGate);
    }

    kern_return_t restoredAllow = KERN_SUCCESS;
    if (allowPage != 0 && allowPage != gatePage) {
        restoredAllow = vm_protect(map, allowPage, PAGE_SIZE, FALSE, allowProt);
        if (restoredAllow != KERN_SUCCESS) {
            restoredAllow = vm_protect(map, allowPage, PAGE_SIZE, FALSE, allowProt);
        }
        if (restoredAllow == KERN_SUCCESS) {
            const auto maxAllow = vm_protect(map, allowPage, PAGE_SIZE, TRUE, allowProt);
            if (maxAllow != KERN_SUCCESS) {
                restoredAllow = maxAllow;
                SYSLOG("UserPatch", "Failed to restore maximum protection for allow page (0x%X)", maxAllow);
            }
        } else {
            SYSLOG("UserPatch", "Failed to restore current protection for allow page (0x%X)", restoredAllow);
        }
    }

    if (writeErr == 0 && restoredGate == KERN_SUCCESS && restoredAllow == KERN_SUCCESS) {
        static volatile UInt32 blendCounter{0};
        const auto count = static_cast<UInt32>(OSIncrementAtomic(&blendCounter) + 1);
        NRed::singleton().setProp32("NRedLegacyBlendApplied", count);

        const auto proc = (process != nullptr) ? process : current_proc();
        if (proc != nullptr) {
            const auto pid = proc_pid(proc);
            char name[32]{};
            if (pid > 0) { proc_name(pid, name, sizeof(name)); }
            name[sizeof(name) - 1] = '\0';
            if (strcmp(name, "WindowServer") == 0) {
                NRed::singleton().setProp32("NRedLegacyBlendWindowServerPID", static_cast<UInt32>(pid));
                NRed::singleton().setProp32("NRedLegacyBlendWindowServerApplied", 1);
            }
            DBGLOG("UserPatch", "LegacyBlend patched in %s (PID %d)", name, pid);
        }
    }
}

bool UserPatches::installSubmitGuard(KernelPatcher& patcher)
{
    const auto kernelMajor = currentKernelVersion().major();
    if (kernelMajor != 22 && kernelMajor != 23 && kernelMajor != 24 && kernelMajor != 25) {
        DBGLOG("UserPatch", "SubmitGuard not supported on Darwin %u", kernelMajor);
        return false;
    }

    getTaskMapFunc = reinterpret_cast<GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
    patcher.clearError();
    queryRegionFunc = reinterpret_cast<RegionRecurse>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_mach_vm_region_recurse"));
    patcher.clearError();
    copyInFunc = reinterpret_cast<CopyIn>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyin"));
    patcher.clearError();
    copyOutFunc = reinterpret_cast<CopyOut>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyout"));
    patcher.clearError();

    if (!getTaskMapFunc || !queryRegionFunc || !copyInFunc || !copyOutFunc) {
        SYSLOG("UserPatch", "Failed to resolve kernel symbols for SubmitGuard");
        return false;
    }

    KernelPatcher::RouteRequest request{"_shared_region_check_np", wrapSharedRegionCheck, this->orgSharedRegionCheck};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1)) {
        patcher.clearError();
        SYSLOG("UserPatch", "Failed to route _shared_region_check_np for SubmitGuard");
        return false;
    }

    NRed::singleton().setProp32("NRedSubmitGuardEnabled", 1);
    NRed::singleton().setProp32("NRedSubmitGuardVersion", 1);
    NRed::singleton().setProp32("NRedLegacyBlendEnabled", 1);
    NRed::singleton().setProp32("NRedLegacyBlendVersion", 1);
    SYSLOG("UserPatch", "SubmitGuard and LegacyBlend enabled for Darwin %u (offset 0x%llX)", kernelMajor,
           UserSubmitGuardData::getMethodOffset(kernelMajor));
    return true;
}
