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
#include <GLResourceBudgetPatch.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatchMemory.hpp>
#include <UserPatches.hpp>
#include <VDA.hpp>
#include <VCNBridge.hpp>
#include <libkern/OSAtomic.h>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_types.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/vnode.h>

extern "C" vm_map_t current_map(void);

static UserPatches moduleInstance;

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

    // macOS 14 (Sonoma) Allow pattern: 0F 93 C0 (setae al) -> B0 01 90 (mov al,1; nop)
    static const UInt8 kMTLAllowFind_Sonoma[]    = {0x0F, 0x93, 0xC0};
    static const UInt8 kMTLAllowReplace_Sonoma[] = {0xB0, 0x01, 0x90};

    // macOS 15 (Sequoia) Allow pattern: 0F 94 C0 (setz al) -> B0 01 90 (mov al,1; nop)
    static const UInt8 kMTLAllowFind_Sequoia[]    = {0x0F, 0x94, 0xC0};
    static const UInt8 kMTLAllowReplace_Sequoia[] = {0xB0, 0x01, 0x90};

    // macOS 26 (Tahoe) Gate: 74 28 (je +0x28) -> EB 00 (jmp +0x00, fallthrough / no-op jump)
    static const UInt8 kMTLGateFind_Tahoe[]    = {0x74, 0x28};
    static const UInt8 kMTLGateReplace_Tahoe[] = {0xEB, 0x00};

    // macOS 26 (Tahoe) Allow pattern: 0F 94 C1 (sete cl) -> B1 01 90 (mov cl,1; nop)
    static const UInt8 kMTLAllowFind_Tahoe[]    = {0x0F, 0x94, 0xC1};
    static const UInt8 kMTLAllowReplace_Tahoe[] = {0xB1, 0x01, 0x90};

    // OpenCL Kernel Dispatch patches (macOS 13 - 26)
    // Intercepts _gldExecuteKernel in AMDRadeonX5000GLDriver:
    // 1. When local_work_size == NULL (auto-calculator), clamps max threads per group to 64
    //    (1 wavefront/workgroup), preventing SPI scratch wave hang and improving CU occupancy
    //    on single-SE Renoir/Cezanne APUs.
    // 2. When local_work_size != NULL with local_work_size[0] > 64, redirects to auto-calculator
    //    ONLY for kernels with private_mem_size (0x59c) > 0, preserving explicit workgroup
    //    geometry for zero-scratch kernels (e.g. Background Blur).
    static const UInt8 kOpenCLDispatchFind_Sequoia[] = {
        0x48, 0x85, 0xDB, 0x4C, 0x89, 0xBD, 0x58, 0xFE, 0xFF, 0xFF, 0x74, 0x2F, 0x4C, 0x89, 0xB5, 0x78,
        0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0xBD, 0xD0, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0xB5, 0xD8, 0xFE, 0xFF,
        0xFF, 0x4C, 0x8B, 0xA5, 0xE0, 0xFE, 0xFF, 0xFF, 0x44, 0x8B, 0x8D, 0xA0, 0xFE, 0xFF, 0xFF, 0x48,
        0x8B, 0x9D, 0x68, 0xFE, 0xFF, 0xFF, 0xE9, 0x2D, 0x01, 0x00, 0x00, 0x8B, 0x8C, 0x31, 0xAC, 0x06,
        0x00, 0x00, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x95, 0xA0, 0xFE, 0xFF, 0xFF, 0x45,
        0x85, 0xD2, 0x48, 0x89, 0xBD, 0xA8, 0xFE, 0xFF, 0xFF, 0x74, 0x39
    };
    static const UInt8 kOpenCLDispatchReplace_Sequoia[] = {
        0x4C, 0x89, 0xBD, 0x58, 0xFE, 0xFF, 0xFF, 0x48, 0x85, 0xDB, 0x74, 0x35, 0x4C, 0x8B, 0x3B, 0x49,
        0x83, 0xFF, 0x40, 0x76, 0x0A, 0x83, 0xBC, 0x31, 0x9C, 0x05, 0x00, 0x00, 0x00, 0x75, 0x22, 0x4C,
        0x89, 0xB5, 0x78, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0x73, 0x08, 0x4C, 0x8B, 0x63, 0x10, 0x44, 0x8B,
        0x8D, 0xA0, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0x9D, 0x68, 0xFE, 0xFF, 0xFF, 0xE9, 0x27, 0x01, 0x00,
        0x00, 0xB9, 0x40, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x95, 0xA0,
        0xFE, 0xFF, 0xFF, 0x48, 0x89, 0xBD, 0xA8, 0xFE, 0xFF, 0xFF, 0x90
    };
    static_assert(sizeof(kOpenCLDispatchFind_Sequoia) == sizeof(kOpenCLDispatchReplace_Sequoia), "Sequoia OpenCL patch size mismatch");

    static const UInt8 kOpenCLDispatchFind_Sonoma[] = {
        0x48, 0x85, 0xDB, 0x4C, 0x89, 0x95, 0x70, 0xFE, 0xFF, 0xFF, 0x48, 0x89, 0x95, 0x78, 0xFE, 0xFF,
        0xFF, 0x74, 0x28, 0x4C, 0x8B, 0xBD, 0xD0, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0x85, 0xD8, 0xFE, 0xFF,
        0xFF, 0x48, 0x8B, 0x85, 0x20, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x85, 0xA0, 0xFE, 0xFF, 0xFF, 0x4C,
        0x8B, 0xB5, 0xE0, 0xFE, 0xFF, 0xFF, 0xE9, 0xFF, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x85, 0x20, 0xFF,
        0xFF, 0xFF, 0x48, 0x89, 0x85, 0xA0, 0xFE, 0xFF, 0xFF, 0x8B, 0x8C, 0x11, 0xAC, 0x06, 0x00, 0x00,
        0xBF, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x8D, 0xAC, 0xFE, 0xFF, 0xFF, 0x45, 0x85, 0xC9, 0x74,
        0x70
    };
    static const UInt8 kOpenCLDispatchReplace_Sonoma[] = {
        0x4C, 0x89, 0x95, 0x70, 0xFE, 0xFF, 0xFF, 0x48, 0x89, 0x95, 0x78, 0xFE, 0xFF, 0xFF, 0x48, 0x8B,
        0x85, 0x20, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x85, 0xA0, 0xFE, 0xFF, 0xFF, 0x48, 0x85, 0xDB, 0x74,
        0x20, 0x4C, 0x8B, 0x3B, 0x49, 0x83, 0xFF, 0x40, 0x76, 0x0A, 0x83, 0xBC, 0x11, 0x9C, 0x05, 0x00,
        0x00, 0x00, 0x75, 0x0D, 0x4C, 0x8B, 0x43, 0x08, 0x4C, 0x8B, 0x73, 0x10, 0xE9, 0xF9, 0x00, 0x00,
        0x00, 0xB9, 0x40, 0x00, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x8D, 0xAC, 0xFE,
        0xFF, 0xFF, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90
    };
    static_assert(sizeof(kOpenCLDispatchFind_Sonoma) == sizeof(kOpenCLDispatchReplace_Sonoma), "Sonoma OpenCL patch size mismatch");

    static const UInt8 kOpenCLDispatchFind_Tahoe[] = {
        0x48, 0x85, 0xDB, 0x48, 0x89, 0x95, 0xA0, 0xFE, 0xFF, 0xFF, 0x74, 0x21, 0x4C, 0x89, 0xB5, 0x78,
        0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0xA5, 0xD0, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0xB5, 0xD8, 0xFE, 0xFF,
        0xFF, 0x4C, 0x8B, 0xBD, 0xE0, 0xFE, 0xFF, 0xFF, 0xE9, 0x04, 0x01, 0x00, 0x00, 0x42, 0x8B, 0x8C,
        0x19, 0xAC, 0x06, 0x00, 0x00, 0x41, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x01, 0x00, 0x00,
        0x00, 0x83, 0xBD, 0x80, 0xFE, 0xFF, 0xFF, 0x00, 0x0F, 0x84, 0x86, 0x00, 0x00, 0x00
    };
    static const UInt8 kOpenCLDispatchReplace_Tahoe[] = {
        0x48, 0x89, 0x95, 0xA0, 0xFE, 0xFF, 0xFF, 0x48, 0x85, 0xDB, 0x74, 0x28, 0x4C, 0x8B, 0x23, 0x49,
        0x83, 0xFC, 0x40, 0x76, 0x0B, 0x42, 0x83, 0xBC, 0x19, 0x9C, 0x05, 0x00, 0x00, 0x00, 0x75, 0x14,
        0x4C, 0x89, 0xB5, 0x78, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0x73, 0x08, 0x4C, 0x8B, 0x7B, 0x10, 0xE9,
        0xFD, 0x00, 0x00, 0x00, 0xB9, 0x40, 0x00, 0x00, 0x00, 0x41, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x41,
        0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    static_assert(sizeof(kOpenCLDispatchFind_Tahoe) == sizeof(kOpenCLDispatchReplace_Tahoe), "Tahoe OpenCL patch size mismatch");

    static const UInt8 kOpenCLDispatchFind_Ventura[] = {
        0x4D, 0x85, 0xF6, 0x48, 0x89, 0xBD, 0x88, 0xFE, 0xFF, 0xFF, 0x74, 0x2F, 0x4C, 0x8B, 0xBD, 0xD0,
        0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0xB5, 0xD8, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0xB5, 0x18, 0xFF, 0xFF,
        0xFF, 0x4C, 0x8B, 0x95, 0x20, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x9D, 0xE0, 0xFE, 0xFF, 0xFF, 0x44,
        0x8B, 0x9D, 0xAC, 0xFE, 0xFF, 0xFF, 0xE9, 0xFB, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x95, 0x18, 0xFF,
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x20, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x85, 0x98, 0xFE, 0xFF, 0xFF,
        0x8B, 0x9C, 0x39, 0xAC, 0x06, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x9D, 0xAC,
        0xFE, 0xFF, 0xFF, 0x45, 0x85, 0xDB, 0x74, 0x66
    };
    static const UInt8 kOpenCLDispatchReplace_Ventura[] = {
        0x48, 0x89, 0xBD, 0x88, 0xFE, 0xFF, 0xFF, 0x4D, 0x85, 0xF6, 0x74, 0x35, 0x4D, 0x8B, 0x3E, 0x49,
        0x83, 0xFF, 0x40, 0x76, 0x0A, 0x83, 0xBC, 0x39, 0x9C, 0x05, 0x00, 0x00, 0x00, 0x75, 0x22, 0x49,
        0x8B, 0x5E, 0x10, 0x4D, 0x8B, 0x76, 0x08, 0x48, 0x8B, 0xB5, 0x18, 0xFF, 0xFF, 0xFF, 0x4C, 0x8B,
        0x95, 0x20, 0xFF, 0xFF, 0xFF, 0x44, 0x8B, 0x9D, 0xAC, 0xFE, 0xFF, 0xFF, 0xE9, 0xF5, 0x00, 0x00,
        0x00, 0x4C, 0x8B, 0x95, 0x18, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x20, 0xFF, 0xFF, 0xFF, 0x48,
        0x89, 0x85, 0x98, 0xFE, 0xFF, 0xFF, 0xBB, 0x40, 0x00, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00, 0x00,
        0x44, 0x8B, 0x9D, 0xAC, 0xFE, 0xFF, 0xFF, 0x90
    };
    static_assert(sizeof(kOpenCLDispatchFind_Ventura) == sizeof(kOpenCLDispatchReplace_Ventura), "Ventura OpenCL patch size mismatch");

    #include "VDASignatures.inc"

    static const UInt8 kResolverMask32[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const UInt8 kResolverMask32_RipMasked[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };
    static const UInt8 kResolverMaskFF[128] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
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
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Sonoma, kResolverMask32, sizeof(kResolverDualCtx_Sonoma), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kOpenCLDispatchFind_Sonoma, kResolverMaskFF, sizeof(kOpenCLDispatchFind_Sonoma), 0, kOpenCLDispatchReplace_Sonoma,
         sizeof(kOpenCLDispatchReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0), kOpenCLDispatchFind_Sonoma, sizeof(kOpenCLDispatchFind_Sonoma)},
        {kResolverGateCtx_Sonoma, kResolverMask32_GateSonoma, sizeof(kResolverGateCtx_Sonoma), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverAllowCtx_Sonoma, kResolverMask32_AllowSonoma, sizeof(kResolverAllowCtx_Sonoma), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sonoma, sizeof(kMTLAllowFind_Sonoma)},
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
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx, kResolverMask32, sizeof(kResolverDualCtx), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kOpenCLDispatchFind_Sequoia, kResolverMaskFF, sizeof(kOpenCLDispatchFind_Sequoia), 0, kOpenCLDispatchReplace_Sequoia,
         sizeof(kOpenCLDispatchReplace_Sequoia), 0, 1, ~static_cast<UInt64>(0), kOpenCLDispatchFind_Sequoia, sizeof(kOpenCLDispatchFind_Sequoia)},
        {kResolverGateCtx, kResolverMask32_GateSequoia, sizeof(kResolverGateCtx), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverSetzCtx, kResolverMask32_SetzSequoia, sizeof(kResolverSetzCtx), 16, kMTLAllowReplace_Sequoia,
         sizeof(kMTLAllowReplace_Sequoia), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sequoia, sizeof(kMTLAllowFind_Sequoia)},
    };
    constexpr size_t kResolverSignatureCount = arrsize(kResolverSignatures);

    // macOS 26 (Tahoe) resolver contexts.
    // The rip-relative displacement at bytes 27-30 is masked to wildcards for forward-compatibility across minor builds.
    static const UInt8 kResolverMask32_HwTahoe[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };

    static const UInt8 kResolverMask32_GateTahoe[] = {
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_AllowTahoe[] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
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
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Tahoe, kResolverMask32, sizeof(kResolverDualCtx_Tahoe), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kOpenCLDispatchFind_Tahoe, kResolverMaskFF, sizeof(kOpenCLDispatchFind_Tahoe), 0, kOpenCLDispatchReplace_Tahoe,
         sizeof(kOpenCLDispatchReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0), kOpenCLDispatchFind_Tahoe, sizeof(kOpenCLDispatchFind_Tahoe)},
        {kResolverGateCtx_Tahoe, kResolverMask32_GateTahoe, sizeof(kResolverGateCtx_Tahoe), 16, kMTLGateReplace_Tahoe,
         sizeof(kMTLGateReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind_Tahoe, sizeof(kMTLGateFind_Tahoe)},
        {kResolverAllowCtx_Tahoe, kResolverMask32_AllowTahoe, sizeof(kResolverAllowCtx_Tahoe), 16, kMTLAllowReplace_Tahoe,
         sizeof(kMTLAllowReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Tahoe, sizeof(kMTLAllowFind_Tahoe)},
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
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Ventura, kResolverMask32, sizeof(kResolverDualCtx_Ventura), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kOpenCLDispatchFind_Ventura, kResolverMaskFF, sizeof(kOpenCLDispatchFind_Ventura), 0, kOpenCLDispatchReplace_Ventura,
         sizeof(kOpenCLDispatchReplace_Ventura), 0, 1, ~static_cast<UInt64>(0), kOpenCLDispatchFind_Ventura, sizeof(kOpenCLDispatchFind_Ventura)},
        {kResolverGateCtx_Ventura, kResolverMask32_GateSonoma, sizeof(kResolverGateCtx_Ventura), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverAllowCtx_Ventura, kResolverMask32_AllowSonoma, sizeof(kResolverAllowCtx_Ventura), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sonoma, sizeof(kMTLAllowFind_Sonoma)},
    };
    constexpr size_t kResolverSignatureCount_Ventura = arrsize(kResolverSignatures_Ventura);

}    // namespace

UserPatches& UserPatches::singleton() { return moduleInstance; }

void UserPatches::init()
{
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

    if (!this->enabled && !this->vdaEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    if (this->cacheLock == nullptr) {
        this->cacheLock = IOSimpleLockAlloc();
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
    if (getKernelVersion() >= KernelVersion::Tahoe) {
        SYSLOG("UserPatch", "Selecting macOS 26 (Tahoe) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Tahoe;
        this->signatureCount = kResolverSignatureCount_Tahoe;
    } else if (getKernelVersion() >= KernelVersion::Sequoia) {
        SYSLOG("UserPatch", "Selecting macOS 15 (Sequoia) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures;
        this->signatureCount = kResolverSignatureCount;
    } else if (getKernelVersion() >= KernelVersion::Sonoma) {
        SYSLOG("UserPatch", "Selecting macOS 14 (Sonoma) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Sonoma;
        this->signatureCount = kResolverSignatureCount_Sonoma;
    } else {
        SYSLOG("UserPatch", "Selecting macOS 13 (Ventura) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Ventura;
        this->signatureCount = kResolverSignatureCount_Ventura;
    }
    for (auto& applied : this->signatureApplied) { applied = 0; }
    this->mtlHwInfoApplied        = false;
    this->glResourceBudgetApplied = false;
}

void UserPatches::processPatcher(KernelPatcher& patcher)
{
    if (!this->enabled && !this->vdaEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    if (this->cacheLock == nullptr) {
        this->cacheLock = IOSimpleLockAlloc();
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

    if (glEnabled) {
        this->selectPatchSet();
    } else {
        this->resolverEnabled = false;
        this->signatures      = nullptr;
        this->signatureCount  = 0;
    }

    const auto vcn1Decode = VDA::singleton().isVcn1DecodeRequested();
    const auto vcn2 = profile != nullptr && profile->media.hasVCN0
                      && profile->media.vcnGeneration == DeviceProfile::kVcn2_2;
    const auto vdaAllowed = this->vdaEnabled && (vcn2 || vcn1Decode)
                            && !NRed::singleton().hasExternalVideo();
    if (this->vdaEnabled && !vdaAllowed) {
        SYSLOG("UserPatch", "VDA patches requested but this device is outside the supported media policy");
        this->vdaEnabled = false;
    }
    if (currentKernelVersion() >= MACOS_11) {
        if (!glEnabled && !this->vdaEnabled) {
            SYSLOG("UserPatch", "No user-space driver patches are configured, not installing any hooks");
            return;
        }

        this->nativeBackendReady = this->installNativePagePatcher(patcher);
        SYSLOG_COND(!this->nativeBackendReady, "UserPatch",
                    "Failed to install the dynamic signature patch backend");
        if (this->nativeBackendReady && this->vdaEnabled && vcn1Decode) {
            this->nativeBackendReady = this->installRaven2PrivatePatcher(patcher);
            SYSLOG_COND(!this->nativeBackendReady, "UserPatch",
                        "Failed to install Raven2 private shared-cache patcher");
        }
        return;
    }
}

auto UserPatches::installNativePagePatcher(KernelPatcher& patcher) -> bool
{
    KernelPatcher::RouteRequest pageRequest{"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &pageRequest, 1)) {
        SYSLOG("UserPatch", "Failed to route _cs_validate_page (%d)", static_cast<int>(patcher.getError()));
        patcher.clearError();
        return false;
    }

    KernelPatcher::RouteRequest rangeRequest{"_cs_validate_range", wrapCsValidateRange, this->orgCsValidateRange};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &rangeRequest, 1)) {
        DBGLOG("UserPatch", "Failed to route _cs_validate_range, continuing without it (%d)",
               static_cast<int>(patcher.getError()));
        patcher.clearError();
    }

    SYSLOG("UserPatch", "Installed the dynamic signature patch backend");
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

void UserPatches::recordCachedSite(const VnodeKind vnodeKind, const UInt64 pageOffset, const size_t inPageOffset,
                                   const UInt8* const find, const UInt8* const replace, const size_t size)
{
    if (vnodeKind == kVnodeNone || find == nullptr || replace == nullptr || size == 0 || size > 0xFF ||
        inPageOffset + size > kPageSize) {
        return;
    }

    if (this->cacheLock != nullptr) { IOSimpleLockLock(this->cacheLock); }

    const size_t count = static_cast<size_t>(this->cachedSiteCount);
    for (size_t i = 0; i < count; ++i) {
        const auto& existing = this->cachedSites[i];
        if (existing.vnodeKind == vnodeKind && existing.pageOffset == pageOffset &&
            existing.inPageOffset == static_cast<UInt16>(inPageOffset)) {
            if (this->cacheLock != nullptr) { IOSimpleLockUnlock(this->cacheLock); }
            return;
        }
    }

    if (count < kMaxCachedSites) {
        this->cachedSites[count] = {
            pageOffset,
            find,
            replace,
            static_cast<UInt16>(inPageOffset),
            static_cast<UInt8>(size),
            vnodeKind,
        };
        __atomic_store_n(&this->cachedSiteCount, static_cast<SInt32>(count + 1), __ATOMIC_RELEASE);
    } else {
        SYSLOG("UserPatch", "Resolver: cached patch site table full (%zu)", kMaxCachedSites);
    }

    if (this->cacheLock != nullptr) { IOSimpleLockUnlock(this->cacheLock); }
}

void UserPatches::reapplyCachedSites(const VnodeKind vnodeKind, const UInt64 pageOffset, const UInt8* const bytes)
{
    const size_t count = static_cast<size_t>(__atomic_load_n(&this->cachedSiteCount, __ATOMIC_ACQUIRE));
    for (size_t i = 0; i < count; ++i) {
        const auto& site = this->cachedSites[i];
        if (site.vnodeKind != vnodeKind || site.pageOffset != pageOffset) {
            continue;
        }
        if (static_cast<size_t>(site.inPageOffset) + site.size > kPageSize) {
            continue;
        }
        if (memcmp(bytes + site.inPageOffset, site.find, site.size) != 0) {
            continue;
        }

        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (re-apply)");
            return;
        }
        memcpy(const_cast<UInt8*>(bytes) + site.inPageOffset, site.replace, site.size);
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

        SYSLOG("UserPatch", "Resolver re-applied cached patch (kind=%u, page=0x%llX, off=0x%X)",
               static_cast<unsigned>(vnodeKind), pageOffset, static_cast<unsigned>(site.inPageOffset));
    }
}

// On 23J631 all five Vcn2DecCommand register writers occupy this single VA driver cache page.
// Validate the complete group before rewriting any members. VCN1 uses the absolute SOC15 register
// index (Raven2 segment 1 at 0x7E00 plus the VCN1 register offset).
static bool patchRaven2DecodeRegisters(UInt8* const bytes)
{
    static constexpr struct RegisterPatch
    {
        size_t instructionOffset;
        UInt16 vcn2Index;
        UInt16 raven2Index;
    } patches[] = {
        {0xD5F, 0x0504, 0x81C4}, // writeUvdGpcomVcpuData0
        {0xD89, 0x0505, 0x81C5}, // writeUvdGpcomVcpuData1
        {0xDB3, 0x0503, 0x81C3}, // writeUvdGpcomVcpuCmd
        {0xDDC, 0x0506, 0x81C6}, // writeUvdEngineStart
        {0xE08, 0x053F, 0x81FF}, // writeUvdNoOp
    };
    for (const auto& patch : patches) {
        const auto* const instruction = bytes + patch.instructionOffset;
        const auto index = static_cast<UInt16>(instruction[1] | (static_cast<UInt16>(instruction[2]) << 8));
        if (instruction[-3] != 0x48 || instruction[-2] != 0x8B || instruction[-1] != 0x07
            || instruction[0] != 0xBE
            || (index != patch.vcn2Index && index != patch.raven2Index)
            || instruction[3] != 0 || instruction[4] != 0
            || instruction[5] != 0xFF || instruction[6] != 0x50 || instruction[7] != 0x20) {
            return false;
        }
    }

    for (const auto& patch : patches) {
        auto* const immediate = bytes + patch.instructionOffset + 1;
        immediate[0] = static_cast<UInt8>(patch.raven2Index);
        immediate[1] = static_cast<UInt8>(patch.raven2Index >> 8);
    }
    return true;
}

static constexpr UInt64 kRaven2EncoderPitchPage = 0x33E4000;
static constexpr UInt64 kRaven2EncoderPacketPage = 0x33E5000;
static constexpr UInt64 kRaven2EncoderAvcStatusPage = 0x33E7000;
static constexpr UInt64 kRaven2EncoderHevcPacketPage = 0x33E6000;
static constexpr UInt64 kRaven2EncoderHevcStatusPage = 0x33E8000;
static bool patchRaven2EncoderInputOutput(UInt8* const bytes)
{
    // Vcn2Encoder::setEncodeParams on 23J631. Use the imported texture's
    // byte pitch before the existing code derives both plane pitches.
    static_assert(sizeof(kVDAFind_SourcePitch) == sizeof(kVDAReplace_SourcePitch));
    auto* const pitch = bytes + 0x526;

    // Vcn2Encoder::setBitstreamBufParams initializes the mode and first
    // address word with one store. VCN1 Mesa uses linear bitstream output.
    static constexpr UInt8 circular[] = {0x48, 0xC7, 0x02, 0x01, 0x00, 0x00, 0x00};
    static constexpr UInt8 linear[] = {0x48, 0xC7, 0x02, 0x00, 0x00, 0x00, 0x00};
    auto* const bitstreamMode = bytes + 0x40A;
    const bool pitchApplied = memcmp(pitch, kVDAReplace_SourcePitch, sizeof(kVDAReplace_SourcePitch)) == 0;
    const bool modeApplied = memcmp(bitstreamMode, linear, sizeof(linear)) == 0;
    if ((!pitchApplied && memcmp(pitch, kVDAFind_SourcePitch, sizeof(kVDAFind_SourcePitch)) != 0)
        || (!modeApplied && memcmp(bitstreamMode, circular, sizeof(circular)) != 0)) { return false; }
    if (pitchApplied && modeApplied) { return true; }
    memcpy(pitch, kVDAReplace_SourcePitch, sizeof(kVDAReplace_SourcePitch));
    memcpy(bitstreamMode, linear, sizeof(linear));
    return true;
}

struct Raven2EncoderPacketSite
{
    UInt16 offset;
    UInt8 size;
    UInt8 original[10];
    UInt8 replacement[10];
};

template <size_t N>
static bool patchRaven2EncoderSites(UInt8* const bytes, const Raven2EncoderPacketSite (&sites)[N])
{
    bool alreadyApplied = true;
    for (const auto& site : sites) {
        const auto* const target = bytes + site.offset;
        if (memcmp(target, site.original, site.size) == 0) {
            alreadyApplied = false;
        }
        else if (memcmp(target, site.replacement, site.size) != 0) {
            return false;
        }
    }
    if (alreadyApplied) { return true; }
    for (const auto& site : sites) {
        memcpy(bytes + site.offset, site.replacement, site.size);
    }
    return true;
}

static bool patchRaven2EncoderPackets(UInt8* const bytes)
{
    // Sonoma 23J631 VA cache .03, page 0x33E5000. Keep the native
    // RENCODE session operations. The Raven firmware also handles the
    // legacy ring-test operations in a separate parser.
    static constexpr Raven2EncoderPacketSite sites[] = {
        {0xA8B, 10, {0xBE, 0x0B, 0x00, 0x00, 0x00, 0xBA, 0xC0, 0x00, 0x00, 0x00}, {0xBE, 0x0A, 0x00, 0x00, 0x00, 0xBA, 0xC0, 0x00, 0x00, 0x00}}, // addSliceHeaderPacket
        {0xB2B, 10, {0xBE, 0x10, 0x00, 0x00, 0x00, 0xBA, 0x0C, 0x00, 0x00, 0x00}, {0xBE, 0x0C, 0x00, 0x00, 0x00, 0xBA, 0x0C, 0x00, 0x00, 0x00}}, // addIntraRefreshPacket
        {0xB70, 10, {0xBE, 0x11, 0x00, 0x00, 0x00, 0xBA, 0x58, 0x02, 0x00, 0x00}, {0xBE, 0x0D, 0x00, 0x00, 0x00, 0xBA, 0x4C, 0x02, 0x00, 0x00}}, // addContextBufferPacket
        {0xCFD, 10, {0xBE, 0x0F, 0x00, 0x00, 0x00, 0xBA, 0x2C, 0x00, 0x00, 0x00}, {0xBE, 0x0B, 0x00, 0x00, 0x00, 0xBA, 0x2C, 0x00, 0x00, 0x00}}, // addEncodePacket
        {0xC2A, 10, {0xBE, 0x15, 0x00, 0x00, 0x00, 0xBA, 0x14, 0x00, 0x00, 0x00}, {0xBE, 0x10, 0x00, 0x00, 0x00, 0xBA, 0x14, 0x00, 0x00, 0x00}}, // addFeedbackBufferPacket
        {0xC90, 10, {0xBE, 0x12, 0x00, 0x00, 0x00, 0xBA, 0x14, 0x00, 0x00, 0x00}, {0xBE, 0x0E, 0x00, 0x00, 0x00, 0xBA, 0x14, 0x00, 0x00, 0x00}}, // addBitstreamBufferPacket
        {0xA9C,  3, {0x55, 0x48, 0x89}, {0x31, 0xC0, 0xC3}}, // addInputFormatPacket
        {0xAB8,  3, {0x55, 0x48, 0x89}, {0x31, 0xC0, 0xC3}}, // addOutputFormatPacket
        // The optional VCN2 EFC packet collides with VCN1's bitstream buffer ID.
        {0xAD4,  8, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x30}, {0x31, 0xC0, 0xC3, 0xE5, 0x48, 0x83, 0xEC, 0x30}}, // addEfcPacket
    };
    return patchRaven2EncoderSites(bytes, sites);
}

static bool patchRaven2EncoderHevcPackets(UInt8* const bytes)
{
    // VCN1 uses the first six deblock words. Skip the Apple HW-specific
    // instruction packet in both the inlined and standalone writers.
    static constexpr Raven2EncoderPacketSite sites[] = {
        {0x4A2, 10, {0x41, 0x80, 0x7E, 0x1A, 0x00, 0x74, 0x1C, 0x48, 0x8D, 0x8B},
                    {0x41, 0x80, 0x7E, 0x1A, 0x00, 0xEB, 0x1C, 0x48, 0x8D, 0x8B}},
        {0x4D6, 10, {0xBE, 0x03, 0x00, 0x10, 0x00, 0xBA, 0x1C, 0x00, 0x00, 0x00},
                    {0xBE, 0x03, 0x00, 0x10, 0x00, 0xBA, 0x18, 0x00, 0x00, 0x00}},
        {0x6BC, 10, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8D, 0x8F, 0xD4, 0x08, 0x00},
                    {0x31, 0xC0, 0xC3, 0xE5, 0x48, 0x8D, 0x8F, 0xD4, 0x08, 0x00}},
        {0x6E3, 10, {0xBE, 0x03, 0x00, 0x10, 0x00, 0xBA, 0x1C, 0x00, 0x00, 0x00},
                    {0xBE, 0x03, 0x00, 0x10, 0x00, 0xBA, 0x18, 0x00, 0x00, 0x00}},
    };
    return patchRaven2EncoderSites(bytes, sites);
}

static bool patchRaven2EncoderStatus(UInt8* const bytes, const size_t offset)
{
    // AVDVcn2AvcEncDataProcessor and AVDVcn2HevcEncDataProcessor use the same
    // feedback indexing sequence in ProcessEncodeStatus on Sonoma 23J631.
    // Index the feedback ring using the same 48-byte record size sent in
    // setFeedbackBufParams. Keep the larger host allocation as padding.
    constexpr auto& original = kVDACtx_StatusStride_Modern;
    constexpr auto& replacement = kVDAReplace_StatusStride_Modern;
    constexpr size_t strideOffset = 3;
    constexpr size_t afterStride = strideOffset + sizeof(replacement);
    constexpr size_t suffixSize = sizeof(original) - afterStride;
    auto* const site = bytes + offset;
    auto* const stride = site + strideOffset;
    if (memcmp(site, original, strideOffset) != 0
        || memcmp(site + afterStride, original + afterStride, suffixSize) != 0) { return false; }
    if (memcmp(stride, replacement, sizeof(replacement)) == 0) { return true; }
    if (memcmp(stride, original + strideOffset, sizeof(replacement)) != 0) { return false; }
    memcpy(stride, replacement, sizeof(replacement));
    return true;
}

static bool patchRaven2DecodePage(UInt8* const bytes, const UInt64 pageOffset, const bool gvaCache)
{
    // File offsets from the installed 23J631 x86_64h subcaches. All sites on one page
    // must match before any writes to the private buffer.
    static constexpr struct Site
    {
        UInt64                 fileOffset;
        const UserPatches::SignaturePatch* signature;
    } sites[] = {
        {0x2A2D3C, &kGVAResolverSignature_Sonoma},
        {0x339D546, &kVDAResolverSignatures_Sonoma[0]}, // VA accelerator family
        {0x339ED9C, &kVDAResolverSignatures_Sonoma[1]}, // AddrLib interface
        {0x33A64C7, &kVDAResolverSignatures_Sonoma[6]}, // GFX9 video processors
        {0x33A6589, &kVDAResolverSignatures_Sonoma[6]},
        {0x33A660B, &kVDAResolverSignatures_Sonoma[6]},
        {0x33A6710, &kVDAResolverSignatures_Sonoma[5]}, // GFX9 graphics engine and blit
        {0x33A67A9, &kVDAResolverSignatures_Sonoma[5]},
        {0x33FFC15, &kVDAResolverSignatures_Sonoma[2]}, // AddrLib creation
        {0x34154F0, &kVDAResolverSignatures_Sonoma[3]}, // GFX9 chip family
    };
    static_assert(arrsize(sites) <= 32);

    if (!gvaCache) {
        switch (pageOffset) {
            case 0x33E3000: return patchRaven2DecodeRegisters(bytes);
            case kRaven2EncoderPitchPage: return patchRaven2EncoderInputOutput(bytes);
            case kRaven2EncoderPacketPage: return patchRaven2EncoderPackets(bytes);
            case kRaven2EncoderHevcPacketPage: return patchRaven2EncoderHevcPackets(bytes);
            case kRaven2EncoderAvcStatusPage: return patchRaven2EncoderStatus(bytes, 0xA1);
            case kRaven2EncoderHevcStatusPage: return patchRaven2EncoderStatus(bytes, 0x279);
        }
    }

    bool found = false;
    UInt32 pending = 0;
    for (size_t i = 0; i < arrsize(sites); ++i) {
        const auto& site = sites[i];
        const auto offset = site.fileOffset % UserPatches::kPageSize;
        const bool matchingCache = site.signature == &kGVAResolverSignature_Sonoma ? gvaCache : !gvaCache;
        if (!matchingCache || site.fileOffset - offset != pageOffset) {
            continue;
        }

        found = true;
        const auto& sig = *site.signature;
        const auto* const target = bytes + offset;
        if (memcmp(target, sig.replace, sig.replaceSize) == 0) { continue; }
        size_t matchOffset = 0;
        const auto contextOffset = offset - sig.patchOffset;
        if (!matchSignature(bytes, sig, matchOffset, contextOffset) || matchOffset != contextOffset) {
            return false;
        }
        pending |= UInt32{1} << i;
    }
    for (size_t i = 0; i < arrsize(sites); ++i) {
        if ((pending & (UInt32{1} << i)) == 0) { continue; }
        const auto& site = sites[i];
        memcpy(bytes + site.fileOffset % UserPatches::kPageSize,
               site.signature->replace, site.signature->replaceSize);
    }

    return found;
}

namespace
{
    // Sonoma 23J631 x86_64h cache UUIDs and RX mapping deltas from their headers.
    constexpr UInt64 kRaven2GVAOffset = 0x200000000;
    constexpr UInt64 kRaven2VAOffset  = 0x300000000;
    constexpr UInt8 kRaven2CacheUUID[] = {0x4C, 0x14, 0x41, 0xFD, 0x8A, 0x54, 0x3D, 0xC2,
                                         0x86, 0x3D, 0x49, 0x11, 0x0B, 0x94, 0x6A, 0x25};
    constexpr UInt8 kRaven2GVAUUID[] = {0x14, 0xCB, 0xFF, 0xB6, 0x1F, 0x33, 0x33, 0x9C,
                                        0x80, 0xC3, 0x74, 0xC7, 0xB0, 0x65, 0xDB, 0xAD};
    constexpr UInt8 kRaven2VAUUID[] = {0x51, 0x8E, 0x60, 0x50, 0x16, 0x9E, 0x34, 0xDD,
                                       0x9A, 0x0F, 0x2F, 0x32, 0xC3, 0xAF, 0xB8, 0x78};

    enum class Raven2PageUse : UInt8 { Decode, Encode, AvcEncode, HevcEncode, Metal };
    struct Raven2PageTarget
    {
        UInt64 subcache;
        UInt64 fileOffset;
        Raven2PageUse use;
    };

    constexpr Raven2PageTarget kRaven2PageTargets[] = {
        {kRaven2GVAOffset, 0x2A2000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x339D000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x339E000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x33A6000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x33E3000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x33FF000, Raven2PageUse::Decode},
        {kRaven2VAOffset, 0x3415000, Raven2PageUse::Decode},
        {kRaven2VAOffset, kRaven2EncoderPitchPage, Raven2PageUse::Encode},
        {kRaven2VAOffset, kRaven2EncoderPacketPage, Raven2PageUse::Encode},
        {kRaven2VAOffset, kRaven2EncoderAvcStatusPage, Raven2PageUse::AvcEncode},
        {kRaven2VAOffset, kRaven2EncoderHevcPacketPage, Raven2PageUse::HevcEncode},
        {kRaven2VAOffset, kRaven2EncoderHevcStatusPage, Raven2PageUse::HevcEncode},
        {kRaven2VAOffset, 0x354D000, Raven2PageUse::Metal},
        {kRaven2VAOffset, 0x358A000, Raven2PageUse::Metal},
    };

    struct Raven2PrivatePage
    {
        UserPatchMemory::Page vm;
        vm_prot_t maximum;
        UInt8 original[PAGE_SIZE];
        UInt8 patched[PAGE_SIZE];
        bool writeAttempted;
    };

    struct Raven2MapState
    {
        vm_map_t map;
        vm_map_t readyMap;
    };

    struct Raven2SharedRegionArgs { user_addr_t startAddress; };
    using Raven2SharedRegionCheck = int (*)(proc_t, Raven2SharedRegionArgs*, int*);
    using Raven2MapDestroy = void (*)(vm_map_t);
    using Raven2GetTaskMap = vm_map_t (*)(task_t);
    using Raven2CopyIn = int (*)(user_addr_t, void*, size_t);
    using Raven2CopyOut = int (*)(const void*, user_addr_t, size_t);

    static Raven2SharedRegionCheck raven2OriginalSharedRegionCheck{nullptr};
    static Raven2MapDestroy raven2OriginalMapDestroy{nullptr};
    static Raven2GetTaskMap raven2GetTaskMap{nullptr};
    static Raven2CopyIn raven2CopyIn{nullptr};
    static Raven2CopyOut raven2CopyOut{nullptr};
    static UserPatchMemory::RegionQuery raven2QueryRegion{nullptr};
    static IOLock* raven2MapLock{nullptr};
    static Raven2MapState raven2Maps[1024]{};
    static bool raven2MetalEnabled{false};
    static UInt64 raven2VerifiedCacheBase{0};

    void reportRaven2VMError(const kern_return_t result)
    {
        NRed::singleton().setProp32("NRedRaven2PrivateVMError", static_cast<UInt32>(result));
    }

    vm_map_t raven2CurrentMap()
    {
        const auto map = current_map();
        return map != nullptr && raven2GetTaskMap(current_task()) == map ? map : nullptr;
    }

    bool matchesRaven2Cache(const UInt64 address, const UInt8 (&uuid)[16])
    {
        UInt8 header[0x68];
        return raven2CopyIn(address, header, sizeof(header)) == 0
            && memcmp(header, "dyld_v1 ", 8) == 0
            && memcmp(header + 0x58, uuid, sizeof(uuid)) == 0;
    }

    bool prepareRaven2MetalPage(UInt8* const bytes, const UInt64 fileOffset)
    {
        if (fileOffset == 0x358A000) {
            constexpr size_t offsets[] = {0xBF3, 0xC3E, 0xC7B};
            for (const auto offset : offsets) {
                if (memcmp(bytes + offset, kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)) != 0
                    && memcmp(bytes + offset, kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace)) != 0) {
                    return false;
                }
            }
            for (const auto offset : offsets) {
                memcpy(bytes + offset, kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace));
            }
            return true;
        }

        constexpr size_t offsets[] = {0x1BC, 0x1E1};
        for (size_t i = 0; i < arrsize(offsets); ++i) {
            const auto& sig = kResolverSignatures_Sonoma[i + 3];
            const auto offset = offsets[i];
            if (memcmp(bytes + offset, sig.replace, sig.replaceSize) == 0) { continue; }
            const auto contextOffset = offset - sig.patchOffset;
            size_t match;
            if (!matchSignature(bytes, sig, match, contextOffset) || match != contextOffset) { return false; }
            memcpy(bytes + offset, sig.replace, sig.replaceSize);
        }
        return true;
    }

    bool restoreRaven2Page(vm_map_t map, Raven2PrivatePage& entry)
    {
        auto& page = entry.vm;
        if (!page.writable && !page.maximumPending) { return true; }

        // Restore the task's original maximum, which can differ from its current RX protection.
        if (page.writable) {
            auto result = vm_protect(map, page.address, PAGE_SIZE, FALSE, page.protection);
            if (result != KERN_SUCCESS) {
                reportRaven2VMError(result);
                result = vm_protect(map, page.address, PAGE_SIZE, FALSE, page.protection);
            }
            if (result != KERN_SUCCESS) {
                reportRaven2VMError(result);
                return false;
            }
            page.writable = false;
            page.maximumPending = true;
        }

        const auto result = vm_protect(map, page.address, PAGE_SIZE, TRUE, entry.maximum);
        if (result != KERN_SUCCESS) {
            reportRaven2VMError(result);
            return false;
        }
        page.maximumPending = false;
        return true;
    }

    bool applyRaven2PrivatePages(const vm_map_t map, Raven2PrivatePage* const pages, const size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            auto& page = pages[i];
            if (memcmp(page.original, page.patched, PAGE_SIZE) == 0) { continue; }
            const auto changed = UserPatchMemory::makeWritable(map, page.vm, reportRaven2VMError);
            if (!changed) { return false; }
            page.writeAttempted = true;
            if (raven2CopyOut(page.patched, page.vm.address, PAGE_SIZE) != 0) { return false; }
        }

        // Publish admission only after every modified page has returned to executable protection.
        for (size_t i = 0; i < count; ++i) {
            if (!restoreRaven2Page(map, pages[i])) { return false; }
        }
        return true;
    }

    bool restoreRaven2OriginalPages(const vm_map_t map, Raven2PrivatePage* const pages, const size_t count)
    {
        bool restored = true;
        for (size_t i = 0; i < count; ++i) {
            auto& page = pages[i];
            if (page.writeAttempted &&
                (!UserPatchMemory::makeWritable(map, page.vm, reportRaven2VMError)
                 || raven2CopyOut(page.original, page.vm.address, PAGE_SIZE) != 0)) {
                restored = false;
            }
            if (!restoreRaven2Page(map, page)) { restored = false; }
        }
        return restored;
    }

    enum class Raven2PatchResult { Ready, Rejected, RestoreFailed };

    Raven2PatchResult patchRaven2PrivateCache(const vm_map_t map, const UInt64 base, const bool verifyOnly = false)
    {
        if (base == 0 || base >= (1ULL << 47) - kRaven2VAOffset - 0x4000000
            || !matchesRaven2Cache(base, kRaven2CacheUUID)
            || !matchesRaven2Cache(base + kRaven2GVAOffset, kRaven2GVAUUID)
            || !matchesRaven2Cache(base + kRaven2VAOffset, kRaven2VAUUID)) {
            return Raven2PatchResult::Rejected;
        }

        auto* const pages = IONewZero(Raven2PrivatePage, arrsize(kRaven2PageTargets));
        if (pages == nullptr) { return Raven2PatchResult::Rejected; }

        const auto& vda = VDA::singleton();
        const bool avc = vda.isRaven2AVCEncodeTestRequested();
        const bool hevc = vda.isRaven2HEVCEncodeTestRequested();
        size_t count = 0;
        bool valid = true;
        for (const auto& target : kRaven2PageTargets) {
            if ((target.use == Raven2PageUse::Encode && !avc && !hevc)
                || (target.use == Raven2PageUse::AvcEncode && !avc)
                || (target.use == Raven2PageUse::HevcEncode && !hevc)
                || (target.use == Raven2PageUse::Metal && !raven2MetalEnabled)) {
                continue;
            }
            auto& page = pages[count++];
            page.vm.address = base + target.subcache + target.fileOffset;
            auto& protection = page.vm.protection;
            if (!UserPatchMemory::leafProtection(map, page.vm.address, protection,
                                                raven2QueryRegion, reportRaven2VMError, &page.maximum)
                || protection != (VM_PROT_READ | VM_PROT_EXECUTE)
                || raven2CopyIn(page.vm.address, page.original, PAGE_SIZE) != 0) {
                valid = false;
                break;
            }
            memcpy(page.patched, page.original, PAGE_SIZE);
            valid = target.use == Raven2PageUse::Metal
                ? prepareRaven2MetalPage(page.patched, target.fileOffset)
                : patchRaven2DecodePage(page.patched, target.fileOffset, target.subcache == kRaven2GVAOffset);
            if (verifyOnly && memcmp(page.original, page.patched, PAGE_SIZE) != 0) { valid = false; }
            if (!valid) { break; }
        }

        auto result = Raven2PatchResult::Rejected;
        if (valid) {
            if (verifyOnly || applyRaven2PrivatePages(map, pages, count)) {
                result = Raven2PatchResult::Ready;
            } else if (!restoreRaven2OriginalPages(map, pages, count)) {
                result = Raven2PatchResult::RestoreFailed;
            }
        }
        IODelete(pages, Raven2PrivatePage, arrsize(kRaven2PageTargets));
        return result;
    }

    Raven2MapState* reserveRaven2Map(const vm_map_t map)
    {
        IOLockLock(raven2MapLock);
        Raven2MapState* empty = nullptr;
        for (auto& state : raven2Maps) {
            if (state.map == map) {
                IOLockUnlock(raven2MapLock);
                return nullptr;
            }
            if (empty == nullptr && state.map == nullptr) { empty = &state; }
        }
        if (empty != nullptr) { empty->map = map; }
        IOLockUnlock(raven2MapLock);
        return empty;
    }

    void finishRaven2Map(Raven2MapState& state, bool ready)
    {
        IOLockLock(raven2MapLock);
        if (ready) {
            __atomic_store_n(&state.readyMap, state.map, __ATOMIC_RELEASE);
        } else {
            state.map = nullptr;
        }
        IOLockUnlock(raven2MapLock);
    }

    int wrapRaven2SharedRegionCheck(const proc_t process, Raven2SharedRegionArgs* const args, int* const retval)
    {
        const auto result = raven2OriginalSharedRegionCheck(process, args, retval);
        if (result != 0 || args == nullptr) { return result; }
        const auto map = raven2CurrentMap();
        UInt64 base;
        if (map == nullptr || raven2CopyIn(args->startAddress, &base, sizeof(base)) != 0) { return result; }

        auto* const state = reserveRaven2Map(map);
        if (state == nullptr) { return result; }
        const auto patched = patchRaven2PrivateCache(map, base);
        const bool ready = patched == Raven2PatchResult::Ready;
        finishRaven2Map(*state, ready);

        auto& nred = NRed::singleton();
        if (ready) {
            __atomic_store_n(&raven2VerifiedCacheBase, base, __ATOMIC_RELEASE);
            const auto& vda = VDA::singleton();
            nred.setProp32("NRedVCNUserApplied", 10);
            nred.setProp32("NRedRaven2DecodeRegistersPatched", 1);
            if (raven2MetalEnabled) {
                nred.setProp32("NRedMTLHwInfoApplied", 3);
                nred.setProp32("NRedLegacyBlendApplied", 1);
            }
            if (vda.isRaven2AVCEncodeTestRequested() || vda.isRaven2HEVCEncodeTestRequested()) {
                nred.setProp32("NRedRaven2EncodePageApplied", 1);
            }
            if (vda.isRaven2AVCEncodeTestRequested()) { nred.setProp32("NRedRaven2AvcFeedbackStride", 0x30); }
            if (vda.isRaven2HEVCEncodeTestRequested()) { nred.setProp32("NRedRaven2HevcFeedbackStride", 0x30); }
        }
        return patched == Raven2PatchResult::RestoreFailed ? EFAULT : result;
    }

    void wrapRaven2MapDestroy(const vm_map_t map)
    {
        IOLockLock(raven2MapLock);
        for (auto& state : raven2Maps) {
            if (state.map == map) {
                __atomic_store_n(&state.readyMap, nullptr, __ATOMIC_RELEASE);
                state.map = nullptr;
                break;
            }
        }
        IOLockUnlock(raven2MapLock);
        raven2OriginalMapDestroy(map);
    }
} // namespace

bool UserPatches::installRaven2PrivatePatcher(KernelPatcher& patcher)
{
    raven2GetTaskMap = reinterpret_cast<Raven2GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
    raven2QueryRegion = reinterpret_cast<UserPatchMemory::RegionQuery>(
        patcher.solveSymbol(KernelPatcher::KernelID, "_mach_vm_region_recurse"));
    raven2CopyIn = reinterpret_cast<Raven2CopyIn>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyin"));
    raven2CopyOut = reinterpret_cast<Raven2CopyOut>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyout"));
    if (!raven2GetTaskMap || !raven2QueryRegion || !raven2CopyIn || !raven2CopyOut) {
        patcher.clearError();
        return false;
    }

    raven2MapLock = IOLockAlloc();
    if (raven2MapLock == nullptr) { return false; }
    raven2MetalEnabled = this->resolverEnabled && this->signatureCount >= 5;

    KernelPatcher::RouteRequest routes[] = {
        {"_vm_map_destroy", wrapRaven2MapDestroy, raven2OriginalMapDestroy},
        {"_shared_region_check_np", wrapRaven2SharedRegionCheck, raven2OriginalSharedRegionCheck},
    };
    if (!patcher.routeMultiple(KernelPatcher::KernelID, routes, arrsize(routes))) {
        patcher.clearError();
        return false;
    }
    NRed::singleton().setProp32("NRedRaven2PrivateCachePatcher", 1);
    return true;
}

bool UserPatches::raven2MediaPagesReady() const
{
    if (raven2GetTaskMap == nullptr) { return false; }
    const auto map = raven2CurrentMap();
    if (map == nullptr) { return false; }

    // The current task holds the map alive. Map destruction clears readyMap before
    // the address can be reused. This lookup must be safe under the accelerator lock.
    for (const auto& state : raven2Maps) {
        if (__atomic_load_n(&state.readyMap, __ATOMIC_ACQUIRE) == map) { return true; }
    }
    return false;
}

bool UserPatches::prepareRaven2MediaPages() const
{
    if (raven2MapLock == nullptr) { return false; }
    if (raven2MediaPagesReady()) { return true; }
    const auto map = raven2CurrentMap();
    if (map == nullptr) { return false; }
    // A child can inherit already private pages without running dyld startup.
    // Verify them before the native context creation entry point enters lock_busy.
    const auto base = __atomic_load_n(&raven2VerifiedCacheBase, __ATOMIC_ACQUIRE);
    if (base == 0) { return false; }
    auto* const state = reserveRaven2Map(map);
    if (state == nullptr) { return raven2MediaPagesReady(); }
    const bool ready = patchRaven2PrivateCache(map, base, true) == Raven2PatchResult::Ready;
    finishRaven2Map(*state, ready);
    return ready;
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

    VnodeKind vnodeKind = kVnodeNone;
    if (isGLDriver) {
        vnodeKind = kVnodeGLDriver;
    } else if (isVADriver) {
        vnodeKind = kVnodeVADriver;
    } else if (isAppleGVA) {
        vnodeKind = kVnodeAppleGVA;
    } else if (isSharedCache) {
        vnodeKind = kVnodeSharedCache;
    } else {
        return;
    }

    const auto* const bytes = static_cast<const UInt8*>(data);
    const bool vcn1Decode = VDA::singleton().isVcn1DecodeRequested();
    // For opted-in VCN1, media and Metal shared-cache edits live in private task mappings.
    // Keep the upstream loose OpenGL driver patch path, including its resource budget fix.
    if (vcn1Decode && isSharedCache) { return; }

    // O(1) fast path for previously discovered sites whose clean UBC pages were evicted under
    // memory pressure and re-faulted from disk. Must run before the `allApplied` early-outs.
    this->reapplyCachedSites(vnodeKind, pageOffset, bytes);

    // VDA Dynamic Resolver path (AMDRadeonVADriver2: embedded in dyld_shared_cache or loose bundle)
    // Note: _cs_validate_page is invoked by XNU only once per physical page when the page enters the
    // global shared-cache vm_object. Do not gate on per-process identity (e.g. WindowServer or BootCache)
    // or accelerator start timing, as skipping a page during its first fault caches it unpatched globally.
    if ((isVADriver || isSharedCache) && this->vdaEnabled && !vcn1Decode && this->vdaSignatures != nullptr &&
        this->vdaSignatureCount > 0) {
        {
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

                        this->recordCachedSite(vnodeKind, pageOffset, matchOffset + sig.patchOffset, sig.find,
                                               sig.replace, sig.replaceSize);

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
        if (isVADriver && !isSharedCache) {
            return;
        }
    }

    // AppleGVA Dynamic Resolver path (embedded in dyld_shared_cache or standalone AppleGVA bundle)
    if ((isAppleGVA || isSharedCache) && this->vdaEnabled && !vcn1Decode && this->gvaSignature != nullptr &&
        this->gvaSignatureApplied < this->gvaSignature->count) {
        {
            size_t matchOffset = 0;
            if (matchSignature(bytes, *this->gvaSignature, matchOffset)) {
                if (matchOffset + this->gvaSignature->patchOffset + this->gvaSignature->replaceSize <= kPageSize) {
                    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                        memcpy(const_cast<UInt8*>(bytes) + matchOffset + this->gvaSignature->patchOffset,
                               this->gvaSignature->replace, this->gvaSignature->replaceSize);
                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                        this->recordCachedSite(vnodeKind, pageOffset, matchOffset + this->gvaSignature->patchOffset,
                                               this->gvaSignature->find, this->gvaSignature->replace,
                                               this->gvaSignature->replaceSize);
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
        if (isAppleGVA && !isSharedCache) {
            return;
        }
    }

    if (!this->resolverEnabled || this->signatures == nullptr || this->signatureCount == 0) {
        return;
    }

    bool allApplied = this->mtlHwInfoApplied && this->glResourceBudgetApplied;
    for (size_t i = 0; i < this->signatureCount && i < kMaxSignatures; ++i) {
        if (this->signatureApplied[i] < this->signatures[i].count) {
            allApplied = false;
            break;
        }
    }
    if (allApplied) { return; }

    // Gate 2: GL patches ONLY apply to AMDRadeonX5000GLDriver; Metal patches ONLY apply to dyld shared cache.
    if (isGLDriver) {
        if (!this->glResourceBudgetApplied) {
            size_t budgetOffset = 0;
            if (GLResourceBudgetPatch::find(bytes, kPageSize, budgetOffset)) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                    SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (GL budget)");
                    return;
                }
                memcpy(const_cast<UInt8*>(bytes) + budgetOffset, GLResourceBudgetPatch::Replace,
                       sizeof(GLResourceBudgetPatch::Replace));
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                this->recordCachedSite(vnodeKind, pageOffset, budgetOffset, GLResourceBudgetPatch::Find,
                                       GLResourceBudgetPatch::Replace, sizeof(GLResourceBudgetPatch::Replace));
                this->glResourceBudgetApplied = true;
                NRed::singleton().setProp32("NRedGLResourceBudgetApplied", 1);
                SYSLOG("UserPatch", "Resolver patched GL resource budget at page offset 0x%zX", budgetOffset);
            }
        }
        for (size_t signatureIndex = 0; signatureIndex < 3 && signatureIndex < this->signatureCount; signatureIndex++) {
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

                this->recordCachedSite(vnodeKind, pageOffset, matchOffset + signature.patchOffset, signature.find,
                                       signature.replace, signature.replaceSize);

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
        // Gate 2.5: Metal HwInfo patch in dyld shared cache.
        // AMDRadeonX5000MTLDriver lives inside dyld shared cache. We must patch
        // gfx9_HwInfoInitData so numShaderEngines is 1 instead of 4, preventing
        // VideoToolbox / QuickLook compute wavefront hangs on AMD APUs.
        if (this->signatureCount > 0 && !this->mtlHwInfoApplied) {
            const auto& hwSig = this->signatures[0];
            size_t matchOffset = 0;
            if (matchSignature(bytes, hwSig, matchOffset)) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                    // In gfx9_HwInfoInitData, all 3 ASIC branches for Vega (r8d <= 0x13, <= 0x27, <= 0x31)
                    // lie within this exact same 4KB page. Replace all 3 occurrences of movl $4, 0x78(%rdi)
                    // with movl $1, 0x78(%rdi).
                    size_t replacedCount = 0;
                    for (size_t off = 0; off + sizeof(kGLDriverHwInfoFind) <= kPageSize; ++off) {
                        if (memcmp(bytes + off, kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)) == 0) {
                            memcpy(const_cast<UInt8*>(bytes) + off, kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace));
                            this->recordCachedSite(vnodeKind, pageOffset, off, kGLDriverHwInfoFind,
                                                   kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace));
                            replacedCount++;
                        }
                    }
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    this->mtlHwInfoApplied = true;
                    NRed::singleton().setProp32("NRedMTLHwInfoApplied", static_cast<UInt32>(replacedCount));
                    SYSLOG("UserPatch", "Resolver patched Metal HwInfo (%zu branches 4->1) at page match 0x%zX",
                           replacedCount, matchOffset);
                } else {
                    SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (Metal HwInfo)");
                }
            }
        }

        // Gate 3: For Metal signatures in dyld shared cache, require co-occurrence of Gate and Allow
        // on the exact same 4KB page, within a sane relative distance (< 0x500 bytes).
        // Signatures index 3 & 4 in all tables represent the Gate and Allow pair.
        if (this->signatureCount >= 5) {
            const auto& gateSig = this->signatures[3];
            const auto& allowSig = this->signatures[4];

            if (gateSig.pageOffset == ~static_cast<UInt64>(0) && allowSig.pageOffset == ~static_cast<UInt64>(0)) {
                const bool needGate = (3 >= kMaxSignatures || this->signatureApplied[3] < gateSig.count);
                const bool needAllow = (4 >= kMaxSignatures || this->signatureApplied[4] < allowSig.count);

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

                            this->recordCachedSite(vnodeKind, pageOffset, gateMatch + gateSig.patchOffset,
                                                   gateSig.find, gateSig.replace, gateSig.replaceSize);
                            this->recordCachedSite(vnodeKind, pageOffset, allowMatch + allowSig.patchOffset,
                                                   allowSig.find, allowSig.replace, allowSig.replaceSize);

                            SYSLOG("UserPatch", "Resolver matched and patched Metal Gate (0x%zX) and Allow (0x%zX) atomically",
                                   gateMatch, allowMatch);
                            if (3 < kMaxSignatures) { this->signatureApplied[3]++; }
                            if (4 < kMaxSignatures) { this->signatureApplied[4]++; }

                            static volatile UInt32 blendResolverCounter{0};
                            const auto count = static_cast<UInt32>(OSIncrementAtomic(&blendResolverCounter) + 1);
                            NRed::singleton().setProp32("NRedLegacyBlendApplied", count);
                        }
                    }
                }
            }
        }
    }
}

void UserPatches::wrapCsValidatePage(void* const vnode, void* const pager, const UInt64 pageOffset,
                                     const void* const data, int* const validated, int* const tainted, int* const nx)
{
    FunctionCast(wrapCsValidatePage, singleton().orgCsValidatePage)(vnode, pager, pageOffset, data, validated, tainted,
                                                                    nx);

    if (validated == nullptr || *validated == 0) { return; }
    if (VDA::singleton().isVcn1DecodeRequested() &&
        (tainted == nullptr || *tainted != 0 || nx == nullptr || *nx != 0)) { return; }
    singleton().patchSignaturePage(data, pageOffset, vnode);
}

boolean_t UserPatches::wrapCsValidateRange(void* const vnode, void* const pager, const UInt64 pageOffset,
                                           const void* const data, const UInt64 dataSize, unsigned* const tainted)
{
    const auto result = FunctionCast(wrapCsValidateRange, singleton().orgCsValidateRange)(vnode, pager, pageOffset,
                                                                                          data, dataSize, tainted);

    if (result == 0 || data == nullptr) { return result; }
    if (VDA::singleton().isVcn1DecodeRequested() && (tainted == nullptr || *tainted != 0)) { return result; }

    const auto* const bytes = static_cast<const UInt8*>(data);
    for (size_t offset = 0; offset + kPageSize <= dataSize; offset += kPageSize) {
        singleton().patchSignaturePage(bytes + offset, pageOffset + offset, vnode);
    }

    return result;
}
