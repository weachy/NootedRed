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
#include <Headers/kern_crypto.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <DeviceProfile.hpp>
#include <MediaHardware.h>
#include <GLResourceBudgetPatch.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatches.hpp>
#include <MediaPatchManifest.inc>
#include <VP9PluginAllowlist.inc>
#include <VCNBridge.hpp>
#include <libkern/OSAtomic.h>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_types.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/vnode.h>

static UserPatches moduleInstance;
extern "C" char osversion[];

namespace
{

    const char* const kBootArgNoGLFix          = "-NRedNoGLFix";
    const char* const kBootArgGLFixOff         = "-NRedGLFixOff";
    const char* const kBootArgGLFixResolveOnly = "-NRedGLFixResolveOnly";
    const char* const kBootArgGLFixResolveOff  = "-NRedGLFixResolveOff";
    const char* const kBootArgGLFixNoResolve   = "-NRedGLFixNoResolve";
    const char* const kBootArgNoVDA            = "-NRedNoVDA";
    const char* const kBootArgUserPatchTrace   = "-NRedUserPatchTrace";

    // The patch bytes and page images below are specific to one driver build, and the native backend hooks
    // code signing page validation. Enabled by default; pass -NRedNoGLFix to opt out.
    constexpr bool kGLFixDefaultEnabled = true;

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

    static const UInt8 kResolverMask32[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
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

bool UserPatches::mediaSupported() const
{
    return this->vdaEnabled && this->nativeBackendReady &&
           this->kernelIdentityMatches &&
           (osversion[0] == '\0' || strcmp(osversion, MediaPatchManifest::Build) == 0);
}

void UserPatches::init()
{
    if (this->cacheLock == nullptr) {
        this->cacheLock = IOSimpleLockAlloc();
    }
    this->traceEnabled = checkKernelArgument(kBootArgUserPatchTrace);
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
    this->vp9RegistryEnabled = this->vdaEnabled && checkKernelArgument("-NRedVP9");

    if (!this->enabled && !this->vdaEnabled) {
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
        SYSLOG("UserPatch", "VA/GVA page manifest requested (validated OS build %s)", MediaPatchManifest::Build);
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
}

void UserPatches::processPatcher(KernelPatcher& patcher)
{
    if (this->cacheLock == nullptr) {
        this->cacheLock = IOSimpleLockAlloc();
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

    if (glEnabled) {
        this->selectPatchSet();
    } else {
        this->resolverEnabled = false;
        this->signatures      = nullptr;
        this->signatureCount  = 0;
    }

    const auto vdaAllowed = this->vdaEnabled && profile != nullptr && profile->media.hasVCN0
                            && profile->media.vcnGeneration == DeviceProfile::kVcn2_2
                            && !NRed::singleton().hasExternalVideo();
    if (this->vdaEnabled && !vdaAllowed) {
        SYSLOG("UserPatch", "VDA patches requested but this profile does not enable the validated VCN 2.x target");
        this->vdaEnabled = false;
    }
    this->vp9RegistryEnabled = this->vp9RegistryEnabled && vdaAllowed
                               && getKernelVersion() == KernelVersion::Tahoe
                               && nredMediaUsesVCN2Bridge(NRed::singleton().getDeviceID());
    NRed::singleton().setProp32("NRedVP9RegistryEnabled", this->vp9RegistryEnabled ? 1 : 0);
    if (this->vp9RegistryEnabled) {
        SYSLOG("UserPatch", "Optional VP9 registry policy: allowing decoder cdhash %s", VP9PluginAllowlist::CDHash);
    }

    if (currentKernelVersion() >= MACOS_11) {
        if (!glEnabled && !this->vdaEnabled) {
            SYSLOG("UserPatch", "No user-space driver patches are configured, not installing any hooks");
            return;
        }

        const auto* const header = reinterpret_cast<const mach_header_64*>(
            patcher.solveSymbol(KernelPatcher::KernelID, "__mh_execute_header"));
        if (header != nullptr && header->sizeofcmds <= 0x10000) {
            this->kernelIdentityMatches = MediaPatchPlan::matchesKernelUUID(
                reinterpret_cast<const UInt8*>(header), sizeof(*header) + header->sizeofcmds,
                MediaPatchManifest::KernelUUID);
        }
        patcher.clearError();
        NRed::singleton().setProp32("NRedMediaKernelValidated", this->kernelIdentityMatches ? 1 : 0);
        this->nativeBackendReady = this->installNativePagePatcher(patcher);
        if (!this->nativeBackendReady) {
            SYSLOG("UserPatch", "Failed to install the page backend; hardware media remains disabled");
        }
        NRed::singleton().setProp32("NRedMediaPatchSupported", this->mediaSupported() ? 1 : 0);
        NRed::singleton().setProp32("NRedMediaPatchPagesExpected", arrsize(MediaPatchManifest::Pages));
        NRed::singleton().setProp32("NRedMediaPatchSitesExpected", MediaPatchManifest::RequiredSites);
        NRed::singleton().setProp32("NRedMediaPatchComplete", 0);
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

    NRed::singleton().setProp32("NRedUserPatchRevision", 9);
    SYSLOG("UserPatch", "Installed per-page backend (revision 9, VCN 2.2 media policy, HEVC bitstream repair and paired VP9, trace %s)",
           this->traceEnabled ? "first 128 writes" : "off");
    return true;
}

void UserPatches::tracePatch(const char* const kind, const size_t signatureIndex,
                             const UInt64 pageOffset, const size_t matchOffset)
{
    if (!this->traceEnabled) { return; }
    const auto event = OSIncrementAtomic(&this->traceCount);
    if (event < 128) {
        SYSLOG("UserPatch", "Write %d: %s signature %zu, file/cache page 0x%llX + 0x%zX",
               event + 1, kind, signatureIndex, static_cast<unsigned long long>(pageOffset), matchOffset);
    } else if (event == 128) {
        SYSLOG("UserPatch", "User patch trace limit reached; patching continues");
    }
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
    // Cached offsets are hints, not authority to modify a newly validated page.
    // In particular, the Metal gate and allow sites must be checked together by
    // the regular resolver below.
    if (vnodeKind != kVnodeGLDriver || !this->resolverEnabled || this->signatures == nullptr) {
        return;
    }
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
        bool contextMatches = false;
        for (size_t index = 0; index < 3 && index < this->signatureCount; ++index) {
            const auto& signature = this->signatures[index];
            if (signature.find != site.find || signature.replace != site.replace ||
                signature.replaceSize != site.size ||
                (signature.pageOffset != ~static_cast<UInt64>(0) && signature.pageOffset != pageOffset)) {
                continue;
            }
            size_t matchOffset = 0;
            contextMatches = matchSignature(bytes, signature, matchOffset) &&
                             matchOffset + signature.patchOffset == site.inPageOffset;
            if (contextMatches) { break; }
        }
        if (!contextMatches) { continue; }

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

void UserPatches::patchMediaPage(const UInt8* const bytes, const MediaPatchPlan::Page& page,
                                 const size_t pageIndex)
{
    auto& bridge = VCNBridge::singleton();
    if (!this->mediaSupported() || !bridge.userPatchesAllowed()) { return; }
    auto* const normalized = Buffer::create<UInt8>(kPageSize);
    if (normalized == nullptr) {
        bridge.failMedia(20);
        return;
    }
    bool needsWrite = false;
    bool valid = MediaPatchPlan::normalize(bytes, page, normalized, needsWrite);
    if (valid) {
        auto* const digest = Crypto::hash(normalized, kPageSize);
        if (digest == nullptr) {
            Buffer::deleter(normalized);
            bridge.failMedia(20);
            return;
        }
        valid = memcmp(digest, page.maskedSHA256, sizeof(page.maskedSHA256)) == 0;
        Buffer::deleter(digest);
    }
    Buffer::deleter(normalized);
    if (!valid) {
        SYSLOG("UserPatch", "Media page rejected: %s:0x%llX; disabling new media work",
               page.file, static_cast<unsigned long long>(page.offset));
        bridge.failMedia(21);
        return;
    }
    if (needsWrite) {
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            bridge.failMedia(22);
            return;
        }
        // Failure can be published on another thread while this page is being checked.
        // Do not undo existing pages or change any site until all checks succeeded.
        if (!bridge.userPatchesAllowed()) {
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
            return;
        }
        for (size_t i = 0; i < page.siteCount; ++i) {
            const auto& site = page.sites[i];
            memcpy(const_cast<UInt8*>(bytes) + site.offset, site.replace, site.size);
        }
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
        NRed::singleton().setProp32("NRedVCNUserApplied", applied);
        this->tracePatch("VA/GVA manifest", pageIndex, page.offset, page.sites[0].offset);
    }
    // Observed-page coverage is diagnostic only. Never skip a fresh clean page.
    const auto bit = UInt32{1} << pageIndex;
    UInt32 old;
    do { old = this->mediaPagesObserved; }
    while (!OSCompareAndSwap(old, old | bit, &this->mediaPagesObserved));
    NRed::singleton().setProp32("NRedMediaPatchPagesObserved", this->mediaPagesObserved);
    if (this->mediaPagesObserved == MediaPatchManifest::RequiredPageMask) {
        NRed::singleton().setProp32("NRedMediaPatchComplete", 1);
    }
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

    const bool isGLDriver = strcmp(vnodeName, "AMDRadeonX5000GLDriver") == 0;
    // Only real Intel cache basenames, not "dyld", substring matches or arbitrary suffixes.
    const auto cachePrefixSize = sizeof("dyld_shared_cache_x86_64h") - 1;
    const auto vnodeNameSize = strlen(vnodeName);
    const bool isSharedCache =
        (strcmp(vnodeName, "dyld_shared_cache_x86_64h") == 0) ||
        (vnodeNameSize == cachePrefixSize + 3 &&
         strncmp(vnodeName, "dyld_shared_cache_x86_64h.", cachePrefixSize + 1) == 0 &&
         vnodeName[cachePrefixSize + 1] >= '0' && vnodeName[cachePrefixSize + 1] <= '9' &&
         vnodeName[cachePrefixSize + 2] >= '0' && vnodeName[cachePrefixSize + 2] <= '9');
    const bool isVP9Cache = strcmp(vnodeName, "dyld_shared_cache_x86_64h") == 0;
    const MediaPatchPlan::Page* mediaPage = nullptr;
    size_t mediaPageIndex = 0;
    if (isSharedCache && this->mediaSupported()) {
        for (size_t i = 0; i < arrsize(MediaPatchManifest::Pages); ++i) {
            const auto& page = MediaPatchManifest::Pages[i];
            if (pageOffset == page.offset && strcmp(vnodeName, page.file) == 0) {
                mediaPage = &page;
                mediaPageIndex = i;
                break;
            }
        }
    }
    vnode_putname(vnodeName);

    if (isSharedCache && this->kernelIdentityMatches && this->vdaEnabled &&
        osversion[0] != '\0' && strcmp(osversion, MediaPatchManifest::Build) != 0) {
        VCNBridge::singleton().failMedia(23);
    }

    if (!isGLDriver && !isSharedCache) {
        return;
    }

    const auto* const bytes = static_cast<const UInt8*>(data);

    // Optional /Library VP9 plugin support. Keep CoreMedia's original Apple
    // signature policy and add exactly the paired decoder's cdhash. Unlike a
    // branch bypass, unrelated third-party plugins still fail validation.
    // The validated Tahoe cache file, file page, in-page offset and complete
    // 200-byte original requirement must all match. Reclaimed pages are
    // rechecked; telemetry never suppresses patching a fresh clean page.
    if (this->vp9RegistryEnabled && this->mediaSupported() &&
        VCNBridge::singleton().userPatchesAllowed() && isVP9Cache &&
        pageOffset == VP9PluginAllowlist::PageOffset &&
        memcmp(bytes + VP9PluginAllowlist::Offset, VP9PluginAllowlist::Original,
               sizeof(VP9PluginAllowlist::Original)) == 0) {
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
            SYSLOG("UserPatch", "Failed to enable writing for the optional VP9 registry policy");
            return;
        }
        memcpy(const_cast<UInt8*>(bytes) + VP9PluginAllowlist::Offset, VP9PluginAllowlist::Replacement,
               sizeof(VP9PluginAllowlist::Replacement));
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vp9RegistryApplied) + 1);
        NRed::singleton().setProp32("NRedVP9RegistryApplied", applied);
        this->tracePatch("VP9 registry policy", 0, pageOffset, VP9PluginAllowlist::Offset);
    }

    if (mediaPage != nullptr) {
        this->patchMediaPage(bytes, *mediaPage, mediaPageIndex);
        return;
    }

    // VA/GVA and VP9 pages have their own full-page validation and reapply
    // on every clean fault. GL cache hits still require a full signature match;
    // Metal gate/allow pairs are checked together in the regular resolver.
    this->reapplyCachedSites(isGLDriver ? kVnodeGLDriver : kVnodeSharedCache, pageOffset, bytes);

    if (!this->resolverEnabled || this->signatures == nullptr || this->signatureCount == 0) {
        return;
    }

    // Gate 2: GL patches ONLY apply to AMDRadeonX5000GLDriver; Metal patches ONLY apply to dyld shared cache.
    if (isGLDriver) {
        size_t budgetOffset = 0;
        if (GLResourceBudgetPatch::find(bytes, kPageSize, budgetOffset)) {
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                SYSLOG("UserPatch", "Failed to enable writing for the GL resource budget correction");
                return;
            }
            memcpy(const_cast<UInt8*>(bytes) + budgetOffset, GLResourceBudgetPatch::Replace,
                   sizeof(GLResourceBudgetPatch::Replace));
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
            this->tracePatch("GL resource budget", 0, pageOffset, budgetOffset);
            static volatile UInt32 budgetCounter{0};
            const auto applied = static_cast<UInt32>(OSIncrementAtomic(&budgetCounter) + 1);
            NRed::singleton().setProp32("NRedGLResourceBudgetApplied", applied);
        }
        for (size_t signatureIndex = 0; signatureIndex < 3 && signatureIndex < this->signatureCount; signatureIndex++) {
            const auto& signature = this->signatures[signatureIndex];
            if (signature.count == 0 ||
                (signature.pageOffset != ~static_cast<UInt64>(0) && signature.pageOffset != pageOffset)) {
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

                this->recordCachedSite(kVnodeGLDriver, pageOffset, matchOffset + signature.patchOffset,
                                       signature.find, signature.replace, signature.replaceSize);
                this->tracePatch("GL", signatureIndex, pageOffset, matchOffset + signature.patchOffset);
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
        if (this->signatures[0].count > 0 &&
            (this->signatures[0].pageOffset == ~static_cast<UInt64>(0) || this->signatures[0].pageOffset == pageOffset)) {
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
                            replacedCount++;
                        }
                    }
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    NRed::singleton().setProp32("NRedMTLHwInfoApplied", static_cast<UInt32>(replacedCount));
                    this->tracePatch("Metal HwInfo", 0, pageOffset, matchOffset + hwSig.patchOffset);
                } else {
                    SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (Metal HwInfo)");
                }
            }
        }

        // Gate 3: For Metal signatures in dyld shared cache, require co-occurrence of Gate and Allow
        // on the exact same 4KB page, within a sane relative distance (< 0x500 bytes).
        // The third GL signature occupies index 2; Metal Gate/Allow are 3/4.
        if (this->signatureCount >= 5) {
            const auto& gateSig = this->signatures[3];
            const auto& allowSig = this->signatures[4];

            if (gateSig.pageOffset == ~static_cast<UInt64>(0) && allowSig.pageOffset == ~static_cast<UInt64>(0)) {
                if (gateSig.count > 0 && allowSig.count > 0) {
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

                            this->tracePatch("Metal Gate", 3, pageOffset, gateMatch + gateSig.patchOffset);
                            this->tracePatch("Metal Allow", 4, pageOffset, allowMatch + allowSig.patchOffset);

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

    // Only patch pages that passed validation, so that a rejected page is never turned into a valid-looking one.
    if (validated != nullptr && *validated != 0) { singleton().patchSignaturePage(data, pageOffset, vnode); }
}

boolean_t UserPatches::wrapCsValidateRange(void* const vnode, void* const pager, const UInt64 pageOffset,
                                           const void* const data, const UInt64 dataSize, unsigned* const tainted)
{
    const auto result = FunctionCast(wrapCsValidateRange, singleton().orgCsValidateRange)(vnode, pager, pageOffset,
                                                                                          data, dataSize, tainted);

    if (result == 0 || data == nullptr) { return result; }

    const auto* const bytes = static_cast<const UInt8*>(data);
    for (size_t offset = 0; offset + kPageSize <= dataSize; offset += kPageSize) {
        singleton().patchSignaturePage(bytes + offset, pageOffset + offset, vnode);
    }

    return result;
}
