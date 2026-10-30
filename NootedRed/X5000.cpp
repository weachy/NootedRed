// AMDRadeonX5000 Patches
//
// Copyright (C) 2022-2026  ChefKiss.
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

#include <AMDGFX9DCN1Display.hpp>
#include <AMDGFX9DCN2Display.hpp>
#include <AMDGFX9DCNDisplay.hpp>
#include <GPUDriversAMD/Accel/HWDisplay.hpp>
#include <GPUDriversAMD/Accel/HWEngine.hpp>
#include <GPUDriversAMD/AddrLib.hpp>
#include <GPUDriversAMD/FB/Attributes.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <X5000.hpp>
#include <DeviceProfile.hpp>
#include <VCNBridge.hpp>
#include <VDA.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSObject.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kChannelTypesPattern[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                             0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};

static const UInt8 kHwlConvertChipFamilyPattern[] = {0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F};

// Make for loop stop after one SDMA engine.
static const UInt8 kStartHWEnginesOriginal[] = {0x40, 0x83, 0xF0, 0x02};
static const UInt8 kStartHWEnginesMask[]     = {0xF0, 0xFF, 0xF0, 0xFF};
static const UInt8 kStartHWEnginesPatched[]  = {0x40, 0x83, 0xF0, 0x01};

// The check in `Addr::Lib::Create` on <=10.15 and 13.4+ is `familyId == 0x8D` instead of `familyId - 0x8D < 2`.
// Change the 0x8D (AI) to 0x8E (RV).
static const UInt8 kAddrLibCreateOriginal[] = {0x41, 0x81, 0x7D, 0x08, 0x8D, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched[]  = {0x41, 0x81, 0x7D, 0x08, 0x8E, 0x00, 0x00, 0x00};

// For some reason Navi (`0x8F`) was added in 14.4 here. Lazy copy pasting?
static const UInt8 kAddrLibCreateOriginal1404[] = {0x41, 0x8B, 0x46, 0x08, 0x3D, 0x8F, 0x00, 0x00, 0x00, 0x74, 0x00,
                                                   0x3D, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreateOriginalMask1404[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatched1404[]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0x8E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAddrLibCreatePatchedMask1404[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                      0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Catalina only. Change loop condition to skip SDMA1_HP.
static const UInt8 kCreateAccelChannelsOriginal[] = {0x8D, 0x44, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched[]  = {0x8D, 0x44, 0x09, 0x01};

// Ditto, Mojave.
static const UInt8 kCreateAccelChannelsOriginal10_14[] = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x02};
static const UInt8 kCreateAccelChannelsPatched10_14[]  = {0x8D, 0x04, 0x09, 0x8D, 0x4C, 0x09, 0x01};

namespace
{
    // Fixes an unsigned 32-bit underflow in IOAccelMemoryAllocator2::allocPages
    // when non-contiguous allocation (`isContiguousOnly == false`) encounters a
    // small free hole whose aligned start `(holeStart + align - 1) & ~(align - 1)`
    // exceeds `holeEnd`.
    void patchIOAccelAllocPages(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                const size_t size)
    {
        const auto allocNonContig =
            patcher.solveSymbol(id,
                                "__ZN26AMDRadeonX5000_AMDHWMemory21allocateNonContiguousEP16AMDMemoryElementyyyybby15_"
                                "eOP_ORIGINATOR9_eOP_TYPE",
                                slide, size);
        if (allocNonContig == 0) {
            patcher.clearError();
            return;
        }

        const auto* const fnBytes    = reinterpret_cast<const UInt8*>(allocNonContig);
        mach_vm_address_t allocPages = 0;
        for (size_t i = 0; i < 0x110; ++i) {
            // Match `callq allocPages (e8 rel32); addq $0x10, %rsp (48 83 c4 10); testb %al, %al (84 c0)`.
            // Across macOS 13-26, allocPages is the only 8-argument call in allocateNonContiguous(yyyy)
            // that pops 16 bytes of stack arguments and tests %al.
            if (fnBytes[i] == 0xE8 && fnBytes[i + 5] == 0x48 && fnBytes[i + 6] == 0x83 && fnBytes[i + 7] == 0xC4 &&
                fnBytes[i + 8] == 0x10 && fnBytes[i + 9] == 0x84 && fnBytes[i + 10] == 0xC0) {
                SInt32 rel = 0;
                memcpy(&rel, fnBytes + i + 1, sizeof(rel));
                allocPages = allocNonContig + i + 5 + static_cast<mach_vm_address_t>(static_cast<SInt64>(rel));
                break;
            }
        }
        if (allocPages == 0) { return; }

        auto* const targetBytes = reinterpret_cast<UInt8*>(allocPages);
        if (targetBytes[0] != 0x55 || targetBytes[1] != 0x48 || targetBytes[2] != 0x89 || targetBytes[3] != 0xE5) {
            return;
        }

        for (size_t off = 0x40; off < 0x160; ++off) {
            // Match across macOS 13-26:
            //   +0: 41 29 C9              subl %ecx, %r9d
            //   +3: 44 23 4D <disp8>      andl <disp8>(%rbp), %r9d
            //   +7: 0F 84/8E <rel32>      je / jle <skip_hole>
            //  +13: 4D 8D 34 09           leaq (%r9,%rcx), %r14
            if (targetBytes[off] == 0x41 && targetBytes[off + 1] == 0x29 && targetBytes[off + 2] == 0xC9 &&
                targetBytes[off + 3] == 0x44 && targetBytes[off + 4] == 0x23 && targetBytes[off + 5] == 0x4D &&
                targetBytes[off + 7] == 0x0F && (targetBytes[off + 8] == 0x84 || targetBytes[off + 8] == 0x8E) &&
                targetBytes[off + 13] == 0x4D && targetBytes[off + 14] == 0x8D && targetBytes[off + 15] == 0x34 &&
                targetBytes[off + 16] == 0x09) {
                if (targetBytes[off + 8] == 0x84 &&
                    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                    targetBytes[off + 8] = 0x8E;
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    SYSLOG("X5000", "Patched IOAccelMemoryAllocator2::allocPages non-contiguous hole underflow");
                }
                return;
            }
        }
    }

    // Fixes AMDAccelVideoContext::processCommand jumping to the loop-completion
    // success block (`this->0x644 = 0`) instead of the error epilogue (`this->0x644 = -6`)
    // when `fetchResource()` fails to prepare a video buffer.
    void patchVideoProcessCommand(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                  const size_t size, const char* const procCmdSym, const char* const fetchResSym)
    {
        const auto procCmd  = patcher.solveSymbol(id, procCmdSym, slide, size);
        const auto fetchRes = patcher.solveSymbol(id, fetchResSym, slide, size);
        if (procCmd == 0 || fetchRes == 0) {
            patcher.clearError();
            return;
        }

        auto* const fnBytes = reinterpret_cast<UInt8*>(procCmd);
        for (size_t i = 0; i < 0x280; ++i) {
            if (fnBytes[i] != 0xE8) { continue; }
            SInt32 callRel = 0;
            memcpy(&callRel, fnBytes + i + 1, sizeof(callRel));
            if (procCmd + i + 5 + static_cast<mach_vm_address_t>(static_cast<SInt64>(callRel)) != fetchRes) {
                continue;
            }

            for (size_t j = i + 5; j < i + 20; ++j) {
                if (fnBytes[j] == 0x0F && fnBytes[j + 1] == 0x84) {
                    SInt32 jeRel = 0;
                    memcpy(&jeRel, fnBytes + j + 2, sizeof(jeRel));
                    const ptrdiff_t jeTargetOff = static_cast<ptrdiff_t>(j) + 6 + jeRel;
                    if (jeTargetOff <= 0 || jeTargetOff >= 0x580) { return; }

                    for (size_t m = static_cast<size_t>(jeTargetOff); m < static_cast<size_t>(jeTargetOff) + 0x30;
                         ++m) {
                        if (fnBytes[m] == 0x48 && fnBytes[m + 1] == 0x81 && fnBytes[m + 2] == 0xC4) {
                            const auto newRel = static_cast<SInt32>(m - (j + 6));
                            if (newRel != jeRel &&
                                MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                                memcpy(fnBytes + j + 2, &newRel, sizeof(newRel));
                                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                                SYSLOG("X5000",
                                       "Patched AMDAccelVideoContext::processCommand fetchResource failure exit");
                            }
                            return;
                        }
                    }
                    return;
                }
            }
            return;
        }
    }

    // Fixes AMDSWScheduler::advance dereferencing a null peer channel pointer
    // on single-GPU APUs (which have no crossfire peer GPU: 0x68(%r12) == NULL),
    // causing a kernel page fault at CR2: 0x48.
    void patchSWSchedulerAdvance(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                                 const size_t size)
    {
        const auto advanceSym =
            patcher.solveSymbol(id,
                                "__ZN29AMDRadeonX5000_AMDSWScheduler7advanceERNS_12HwChannelCtxE",
                                slide, size);
        if (advanceSym == 0) {
            patcher.clearError();
            return;
        }

        auto* const fnBytes = reinterpret_cast<UInt8*>(advanceSym);
        // Across macOS 15 - 26:
        // Match:
        //   49 8b 7c 24 68       movq 0x68(%r12), %rdi
        //   48 83 7f 48 00       cmpq $0x0, 0x48(%rdi)   <-- crashes when %rdi (peer channel) is null on APU
        // We replace "cmpq $0x0, 0x48(%rdi)" (48 83 7f 48 00) with
        // "testq %rdi, %rdi; nop; nop"       (48 85 ff 90 90) so null %rdi safely takes the jump.
        for (size_t i = 0x200; i < 0x380; ++i) {
            if (fnBytes[i] == 0x49 && fnBytes[i + 1] == 0x8B && fnBytes[i + 2] == 0x7C &&
                fnBytes[i + 3] == 0x24 && fnBytes[i + 4] == 0x68 &&
                fnBytes[i + 5] == 0x48 && fnBytes[i + 6] == 0x83 && fnBytes[i + 7] == 0x7F &&
                fnBytes[i + 8] == 0x48 && fnBytes[i + 9] == 0x00) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                    fnBytes[i + 5] = 0x48;
                    fnBytes[i + 6] = 0x85;
                    fnBytes[i + 7] = 0xFF;
                    fnBytes[i + 8] = 0x90;
                    fnBytes[i + 9] = 0x90;
                    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                    SYSLOG("X5000", "Patched AMDSWScheduler::advance null peer channel dereference");
                }
                return;
            }
        }
    }
}    // namespace

static X5000 moduleInstance;

X5000& X5000::singleton() { return moduleInstance; }

X5000::X5000()
{
    if (currentKernelVersion() <= MACOS_10_14_X) {
        this->pm4EngineField               = 0x330;
        this->sdma0EngineField             = 0x338;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->familyTypeField              = 0x29C;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x180;
    }
    else if (currentKernelVersion().majorMatches(MACOS_10_15)) {
        this->pm4EngineField               = 0x348;
        this->sdma0EngineField             = 0x350;
        this->supportedDisplayCountField   = 0x2C;
        this->seCountField                 = 0x58;
        this->shCountField                 = 0x5C;
        this->hwMaxCUsField                = 0x80;
        this->hasUVD0Field                 = 0x90;
        this->hasVCEField                  = 0x92;
        this->hasVCN0Field                 = 0x93;
        this->hasSDMAPagingQueueField      = 0xA4;
        this->hasGetAllClockLimitsField    = 0xA3;
        this->dccDisplayableSupportField   = 0xA5;
        this->familyTypeField              = 0x2B4;
        this->chipSettingsField            = 0x5B18;
        this->hwChannelHWInterfaceField    = 0x18;
        this->hwChannelSubmitCommandBuffer = 0x188;
    }
    else {
        this->pm4EngineField    = 0x3B8;
        this->sdma0EngineField  = 0x3C0;
        this->familyTypeField   = 0x308;
        this->chipSettingsField = 0x5B10;

        if (currentKernelVersion() <= MACOS_12_X) {
            this->supportedDisplayCountField   = 0x2C;
            this->seCountField                 = 0x5C;
            this->shCountField                 = 0x64;
            this->hwMaxCUsField                = 0x98;
            this->hasUVD0Field                 = 0xAC;
            this->hasVCEField                  = 0xAE;
            this->hasVCN0Field                 = 0xAF;
            this->hasSDMAPagingQueueField      = 0xC0;
            this->hasGetAllClockLimitsField    = 0xBF;
            this->dccDisplayableSupportField   = 0xC1;
            this->hwChannelHWInterfaceField    = 0x18;
            this->hwChannelSubmitCommandBuffer = 0x190;
        }
        else {
            this->supportedDisplayCountField = 0x34;
            this->seCountField               = 0x64;
            this->shCountField               = 0x6C;
            this->hwMaxCUsField              = 0xA0;
            this->hasUVD0Field               = 0xB4;
            this->hasVCEField                = 0xB6;
            this->hasVCN0Field               = 0xB7;
            this->hasSDMAPagingQueueField    = 0xBF;
            this->hasGetAllClockLimitsField  = 0xBE;
            this->dccDisplayableSupportField = 0xC0;
            this->hwChannelHWInterfaceField  = 0x20;
            if (currentKernelVersion() >= MACOS_14_4) { this->hwChannelSubmitCommandBuffer = 0x188; }
            else {
                this->hwChannelSubmitCommandBuffer = 0x190;
            }
        }
    }
}

void X5000::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (kextRadeonX5000.loadIndex != id) { return; }

    NRed::singleton().hwLateInit();

    UInt32*           orgChannelTypes;
    mach_vm_address_t orgStartHWEngines;
    void*             pm4ComputeChannelVT;

    PenguinWizardry::PatternSolveRequest solveRequests[] = {
        {currentKernelVersion() <= MACOS_10_15_X ?
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator22getAdditionalQueueListEPPK18_"
             "AMDQueueSpecifierE27additionalQueueList_Default" :
             "__ZZN37AMDRadeonX5000_AMDGraphicsAccelerator19createAccelChannelsEbE12channelTypes",
         orgChannelTypes, kChannelTypesPattern},
        {"__ZN31AMDRadeonX5000_AMDGFX9PM4Engine10gMetaClassE", this->pm4EngineMC},
        {"__ZN32AMDRadeonX5000_AMDGFX9SDMAEngine10gMetaClassE", this->sdmaEngineMC},
        {"__ZN26AMDRadeonX5000_AMDHardware14startHWEnginesEv", orgStartHWEngines},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware32setupAndInitializeHWCapabilitiesEv",
         this->orgGFX9SetupAndInitializeHWCapabilities},
        {"__ZTV39AMDRadeonX5000_AMDGFX9PM4ComputeChannel", pm4ComputeChannelVT},
        {"__ZN30AMDRadeonX5000_AMDPM4HWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
         this->orgPM4SubmitCommandBuffer},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X5000",
               "Failed to resolve symbols");

    AMDRadeonX5000_AMDHWAlignManager::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDHWDisplay::resolve(patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCNDisplay::registerMC(kextRadeonX5000.id, patcher, id, slide, size);
    AMDRadeonX5000_AMDGFX9DCN1Display::resolve(kextRadeonX5000.id);
    AMDRadeonX5000_AMDGFX9DCN2Display::resolve(kextRadeonX5000.id);

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware17allocateHWEnginesEv", allocateHWEngines},
        {"__ZN32AMDRadeonX5000_AMDVega10Hardware32setupAndInitializeHWCapabilitiesEv",
         wrapSetupAndInitializeHWCapabilities},
        {"__ZN26AMDRadeonX5000_AMDHardware12getHWChannelE20_eAMD_HW_ENGINE_TYPE18_eAMD_HW_RING_TYPE", wrapGetHWChannel,
         this->orgGetHWChannel},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20initializeFamilyTypeEv", initializeFamilyType},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20allocateAMDHWDisplayEv", allocateAMDHWDisplay},
        {"__ZN26AMDRadeonX5000_AMDHWMemory17adjustVRAMAddressEy", wrapAdjustVRAMAddress, this->orgAdjustVRAMAddress},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware20writeASICHangLogInfoEPPv", returnZero},
        {"__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj", wrapHwlConvertChipFamily, this->orgHwlConvertChipFamily,
         kHwlConvertChipFamilyPattern},
        {"__ZN27AMDRadeonX5000_AMDHWDisplay14getDisplayInfoEjbbPvP17_FRAMEBUFFER_INFO", fixedGetDisplayInfo},
        {"__ZN33AMDRadeonX5000_AMDHWAlignManager214getSurfaceInfoEP24_AMD_SURFACE_INFO_STRUCT", fixedGetSurfaceInfo},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X5000",
               "Failed to route symbols");

    this->orgTotalFreeMemory = reinterpret_cast<decltype(this->orgTotalFreeMemory)>(
        patcher.solveSymbol(id, "__ZN26AMDRadeonX5000_AMDHWMemory15totalFreeMemoryEv", slide, size));
    this->orgAllocate = reinterpret_cast<decltype(this->orgAllocate)>(patcher.solveSymbol(
        id,
        "__ZN26AMDRadeonX5000_AMDHWMemory8allocateEP16AMDMemoryElementyy28_eAMD_MEMORY_ALLOCATION_TYPEb15_eOP_ORIGINATOR9_eOP_TYPE",
        slide, size));
    this->orgAllocateNonContiguous = reinterpret_cast<decltype(this->orgAllocateNonContiguous)>(patcher.solveSymbol(
        id,
        "__ZN26AMDRadeonX5000_AMDHWMemory21allocateNonContiguousEP16AMDMemoryElementyyyybby15_eOP_ORIGINATOR9_eOP_TYPE",
        slide, size));

    if (this->orgTotalFreeMemory != nullptr && this->orgAllocate != nullptr && this->orgAllocateNonContiguous != nullptr) {
        PenguinWizardry::PatternRouteRequest allocLargeReq{
            "__ZN26AMDRadeonX5000_AMDHWMemory19allocateLargeBlocksEP16AMDMemoryElementyyy15_eOP_ORIGINATOR9_eOP_TYPEb",
            wrapAllocateLargeBlocks, this->orgAllocateLargeBlocks};
        if (!allocLargeReq.route(patcher, id, slide, size)) {
            patcher.clearError();
            SYSLOG("X5000", "Failed to route AMDHWMemory::allocateLargeBlocks");
        }
    }
    patchIOAccelAllocPages(patcher, id, slide, size);
    patchVideoProcessCommand(
        patcher, id, slide, size,
        "__ZN35AMDRadeonX5000_AMDAccelVideoContext14processCommandEPj16VideoCommandTypeS0_j",
        "__ZN35AMDRadeonX5000_AMDAccelVideoContext13fetchResourceEP22_AMDVideoResourceParamP21_AMDVideoResourceInfo");
    patchSWSchedulerAdvance(patcher, id, slide, size);

    const auto isGfxOffSupported =
        patcher.solveSymbol(id, "__ZN28AMDRadeonX5000_AMDRTHardware17isGfxOffSupportedEv", slide, size);
    if (isGfxOffSupported != 0) {
        const auto* code = reinterpret_cast<const UInt8*>(isGfxOffSupported);
        if (code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xe5 && code[4] == 0x8a &&
            code[5] == 0x87) {
            this->gfxOffSupportedOffset = *reinterpret_cast<const UInt32*>(code + 6);
            DBGLOG("X5000", "Detected gfxOffSupportedOffset = 0x%x", this->gfxOffSupportedOffset);
        } else {
            this->gfxOffSupportedOffset = 0x206b8;
        }
    } else {
        patcher.clearError();
        this->gfxOffSupportedOffset = 0x206b8;
    }

    PenguinWizardry::PatternRouteRequest rtStartReq{
        "__ZN28AMDRadeonX5000_AMDRTHardware16hardwareDidStartEv",
        wrapRTHardwareDidStart, this->orgRTHardwareDidStart};
    if (!rtStartReq.route(patcher, id, slide, size)) {
        patcher.clearError();
        SYSLOG("X5000", "Failed to route AMDRTHardware::hardwareDidStart");
    }

    PenguinWizardry::PatternRouteRequest gfxOffRoutes[] = {
        {"__ZN28AMDRadeonX5000_AMDRTHardware17isGfxOffSupportedEv", returnZero},
        {"__ZN28AMDRadeonX5000_AMDRTHardware12enableGfxOffEv", returnZero},
        {"__ZN30AMDRadeonX5000_AMDGFX9Hardware19gfxOffTimerCallbackEv", returnZero},
    };
    for (auto& req : gfxOffRoutes) {
        if (!req.route(patcher, id, slide, size)) {
            patcher.clearError();
            DBGLOG("X5000", "Optional GFXOFF route omitted: %s", req.symbol);
        }
    }
    NRed::singleton().setProp32("NRedGFXOFFPatched", 1);

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternSolveRequest solveRequest{"__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv",
                                                          this->notifyGfxAccess};
        PANIC_COND(!solveRequest.solve(patcher, id, slide, size), "X5000", "Failed to resolve notifyGfxAccess");

        bool hooked = false;
        if (pm4ComputeChannelVT != nullptr && this->orgPM4SubmitCommandBuffer != 0) {
            auto* vt = reinterpret_cast<mach_vm_address_t*>(pm4ComputeChannelVT);
            for (size_t idx = (0x170 / sizeof(mach_vm_address_t)); idx <= (0x1B0 / sizeof(mach_vm_address_t)); ++idx) {
                if (vt[idx] == this->orgPM4SubmitCommandBuffer) {
                    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                        vt[idx] = reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                        hooked = true;
                        DBGLOG("X5000", "Successfully hooked computeSubmitCommandBuffer at dynamic vtable offset 0x%lx",
                               idx * sizeof(mach_vm_address_t));
                    }
                    break;
                }
            }
        }
        if (!hooked) {
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                this->hwChannelSubmitCommandBuffer(pm4ComputeChannelVT) =
                    reinterpret_cast<mach_vm_address_t>(computeSubmitCommandBuffer);
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                hooked = true;
                SYSLOG("X5000", "Fallback hooked computeSubmitCommandBuffer via field offset");
            }
        }
    }

    if (currentKernelVersion() >= MACOS_13_4) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_"
            "PRIORITYP27AMDRadeonX5000_AMDAccelTask",
            wrapObtainAccelChannelGroup1304, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }
    else if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::PatternRouteRequest request{
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator23obtainAccelChannelGroupE11SS_PRIORITY",
            wrapObtainAccelChannelGroup, this->orgObtainAccelChannelGroup};
        PANIC_COND(!request.route(patcher, id, slide, size), "X5000", "Failed to route obtainAccelChannelGroup");
    }

    if (currentKernelVersion() >= MACOS_14_4) {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,          kAddrLibCreateOriginal1404,    kAddrLibCreateOriginalMask1404,
            kAddrLibCreatePatched1404, kAddrLibCreatePatchedMask1404, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply 14.4+ Addr::Lib::Create patch");
    }
    else if (currentKernelVersion() <= MACOS_10_15_X || currentKernelVersion() >= MACOS_13_4) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kAddrLibCreateOriginal, kAddrLibCreatePatched,
                                                       1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to apply Addr::Lib::Create patch");
    }

    if (currentKernelVersion() <= MACOS_10_15_X) {
        if (currentKernelVersion().majorMatches(MACOS_10_15)) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal,
                                                           kCreateAccelChannelsPatched, 2};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX5000, kCreateAccelChannelsOriginal10_14,
                                                           kCreateAccelChannelsPatched10_14, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X5000", "Failed to patch createAccelChannels");
        }

        // TODO: wait, what is this doing again?
        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32                                   findNonBpp64 = Dcn1NonBpp64SwModeMask1015;
            UInt32                                   replNonBpp64 = Dcn2NonBpp64SwModeMask1015;
            UInt32                                   findBpp64 = Dcn1NonBpp64SwModeMask1015 ^ Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64 = Dcn2NonBpp64SwModeMask1015 ^ Dcn2Bpp64SwModeMask1015;
            UInt32                                   findBpp64Pt2 = Dcn1Bpp64SwModeMask1015;
            UInt32                                   replBpp64Pt2 = Dcn2Bpp64SwModeMask1015;
            const PenguinWizardry::MaskedLookupPatch patches[]    = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32), 2},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32), 1},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64Pt2),
                 reinterpret_cast<const UInt8*>(&replBpp64Pt2), sizeof(UInt32), 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        *orgChannelTypes = 1;    // Make VMPT use SDMA0 instead of SDMA1
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
    else {
        const PenguinWizardry::MaskedLookupPatch patch{
            &kextRadeonX5000,       kStartHWEnginesOriginal, kStartHWEnginesMask,
            kStartHWEnginesPatched, kStartHWEnginesMask,     currentKernelVersion() >= MACOS_13 ? 2U : 1};
        PANIC_COND(!patch.apply(patcher, orgStartHWEngines, PAGE_SIZE), "X5000", "Failed to patch startHWEngines");

        if (NRed::singleton().getAttributes().isRenoir()) {
            UInt32 findBpp64 = Dcn1Bpp64SwModeMask, replBpp64 = Dcn2Bpp64SwModeMask;
            UInt32 findNonBpp64 = Dcn1NonBpp64SwModeMask, replNonBpp64 = Dcn2NonBpp64SwModeMask;
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findBpp64),
                 reinterpret_cast<const UInt8*>(&replBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
                {&kextRadeonX5000, reinterpret_cast<const UInt8*>(&findNonBpp64),
                 reinterpret_cast<const UInt8*>(&replNonBpp64), sizeof(UInt32),
                 currentKernelVersion() >= MACOS_13_4 ? 2U : 4},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X5000",
                       "Failed to patch swizzle mode");
        }

        PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X5000",
                   "Failed to enable kernel writing");
        // createAccelChannels: stop at SDMA0
        orgChannelTypes[5] = 1;
        // getPagingChannel: get only SDMA0
        orgChannelTypes[currentKernelVersion() >= MACOS_12 ? 12 : 11] = 0;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        DBGLOG("X5000", "Applied SDMA1 patches");
    }
}

bool X5000::allocateHWEngines(void* const self)
{
    [[clang::suppress]] singleton().pm4EngineField(self)   = singleton().pm4EngineMC->alloc();
    [[clang::suppress]] singleton().sdma0EngineField(self) = singleton().sdmaEngineMC->alloc();

    // VCN0 is only populated when the opt-in bridge validated both drivers and allocated the native X6000
    // VCN2 engine.  If allocation fails, make sure the capability does not stay advertised.
    const auto* const profile = DeviceProfile::current();
    if (profile != nullptr && profile->media.hasVCN0 && VCNBridge::singleton().isRequested()) {
        if (auto* const vcn0 = VCNBridge::singleton().allocateEngine(self); vcn0 != nullptr) {
            (singleton().pm4EngineField + kAMDHWEngineTypeVCN0 * sizeof(void*))(self) = vcn0;
            // Keep the capability in sync regardless of whether setupAndInitializeHWCapabilities runs before
            // or after allocateHWEngines in this macOS build.
            singleton().hasVCN0Field(self) = true;
        }
        else {
            singleton().hasVCN0Field(self) = false;
        }
    }

    return true;
}

void X5000::setVCN0Capability(void* const self, const bool enabled)
{
    if (self != nullptr) { singleton().hasVCN0Field(self) = enabled; }
}

// TODO: Replace with IP Discovery?
void X5000::wrapSetupAndInitializeHWCapabilities(void* const self)
{
    auto& seCount  = singleton().seCountField(self);
    auto& shCount  = singleton().shCountField(self);
    auto& hwMaxCUs = singleton().hwMaxCUsField(self);

    seCount = 1;
    shCount = 1;
    if (NRed::singleton().getAttributes().isRenoir()) { hwMaxCUs = 8; }
    else if (NRed::singleton().getAttributes().isRaven2()) {
        hwMaxCUs = 3;
    }
    else {
        hwMaxCUs = 11;
    }

    FunctionCast(wrapSetupAndInitializeHWCapabilities, singleton().orgGFX9SetupAndInitializeHWCapabilities)(self);

    const auto* const profile = DeviceProfile::current();

    // Media engines stay off unless the user opted in with -NRedVDA, the validated 0x1638 profile advertises
    // VCN0, and the bridge actually enabled/allocated the native engine.  allocateHWEngines() sets the same
    // capability after a successful allocation, so the result does not depend on the hook ordering.
    const auto mediaEnabled = VDA::singleton().isRequested() && VCNBridge::singleton().engineEnabled();

    singleton().supportedDisplayCountField(self) = 4;
    singleton().hasUVD0Field(self)               = mediaEnabled && profile != nullptr && profile->media.hasUVD0;
    singleton().hasVCEField(self)                = mediaEnabled && profile != nullptr && profile->media.hasVCE;
    singleton().hasVCN0Field(self)               = mediaEnabled && profile != nullptr && profile->media.hasVCN0;
    singleton().hasSDMAPagingQueueField(self)    = false;
    singleton().hasGetAllClockLimitsField(self)  = false;
    if (currentKernelVersion() >= MACOS_10_15) { singleton().dccDisplayableSupportField(self) = true; }

    if (mediaEnabled) { DBGLOG("X5000", "VCN0 media engine enabled from the device profile"); }
}

// TODO: Investigate why this is needed.
void* X5000::wrapGetHWChannel(void* const self, AMDHWEngineType engineType, const UInt32 ringId)
{
    if (engineType == kAMDHWEngineTypeSDMA1) { engineType = kAMDHWEngineTypeSDMA0; }
    return FunctionCast(wrapGetHWChannel, singleton().orgGetHWChannel)(self, engineType, ringId);
}

void X5000::initializeFamilyType(void* const self) { singleton().familyTypeField(self) = AMD_FAMILY_RAVEN; }

void* X5000::allocateAMDHWDisplay(void* const)
{
    if (NRed::singleton().getAttributes().isRenoir()) {
        return AMDRadeonX5000_AMDGFX9DCN2Display::gRTMetaClass.alloc();
    }
    return AMDRadeonX5000_AMDGFX9DCN1Display::gRTMetaClass.alloc();
}

UInt64 X5000::wrapAdjustVRAMAddress(void* const self, const UInt64 addr)
{
    auto ret = FunctionCast(wrapAdjustVRAMAddress, singleton().orgAdjustVRAMAddress)(self, addr);
    if (ret != addr) { return ret + NRed::singleton().getFbOffset(); }
    return ret;
}

bool X5000::wrapAllocateLargeBlocks(void* const self, void* const elem, const UInt64 size, const UInt64 align,
                                    const UInt64 minBlockSize, const UInt32 orig, const UInt32 opType,
                                    const bool onlyNonVisible)
{
    auto& inst = singleton();
    if (self == nullptr || elem == nullptr || size == 0) { return false; }

    const UInt64 pageAlignedSize = (size + PAGE_MASK) & ~static_cast<UInt64>(PAGE_MASK);
    if (inst.orgTotalFreeMemory != nullptr && inst.orgTotalFreeMemory(self) < pageAlignedSize) {
        // Fast-fail without walking the allocator free list or spamming IOLog while
        // IOAcceleratorFamily2 is evicting small LRU buffers one by one to make room.
        return false;
    }

    const UInt64 blockGranularity = minBlockSize >= PAGE_SIZE ? minBlockSize : 0x10000ULL;
    const UInt64 roundedSize =
        size > blockGranularity ? ((size + blockGranularity - 1) & ~(blockGranularity - 1)) : pageAlignedSize;
    const UInt64 safeAlign = align >= blockGranularity ? align : blockGranularity;

    // 1. Contiguous attempt in Non-Visible range [256MB, totalVRAMSize] (allocType = 1)
    if (inst.orgAllocate(self, elem, size, align, 1, false, orig, opType)) { return true; }

    // 2. Contiguous attempt in Visible range [reservedNdrvEnd, 256MB] (allocType = 0) if allowed
    if (!onlyNonVisible && inst.orgAllocate(self, elem, size, align, 0, false, orig, opType)) { return true; }

    auto tryNonContiguousRange = [&](const UInt32 allocType) -> bool {
        if (roundedSize > blockGranularity &&
            (inst.orgTotalFreeMemory == nullptr || inst.orgTotalFreeMemory(self) >= roundedSize)) {
            UInt64 chunkSize = 1;
            UInt64 tmp       = roundedSize;
            while ((tmp & 1) == 0) {
                tmp >>= 1;
                chunkSize <<= 1;
            }
            if (tmp == 1) { chunkSize >>= 1; }

            if (chunkSize >= blockGranularity &&
                inst.orgAllocateNonContiguous(self, elem, roundedSize, align, allocType, false, false, chunkSize, orig,
                                              opType)) {
                return true;
            }
            if (chunkSize > blockGranularity) {
                const UInt64 halfChunk = chunkSize >> 1;
                if (halfChunk >= blockGranularity &&
                    inst.orgAllocateNonContiguous(self, elem, roundedSize, align, allocType, false, false, halfChunk,
                                                  orig, opType)) {
                    return true;
                }
                if (halfChunk > blockGranularity &&
                    inst.orgAllocateNonContiguous(self, elem, roundedSize, align, allocType, false, false,
                                                  blockGranularity, orig, opType)) {
                    return true;
                }
            }
        }
        return false;
    };

    // 3. Non-contiguous attempts in Non-Visible range [256MB, totalVRAMSize] (allocType = 1)
    if (tryNonContiguousRange(1)) { return true; }

    // 4. Fallback to Full UMA VRAM range [reservedNdrvEnd, totalVRAMSize] (allocType = 2):
    //    On AMD APUs, BAR0-visible and non-visible UMA are backed by identical system RAM,
    //    and totalFreeMemory() includes free space in [reservedNdrvEnd, 256MB].
    if (inst.orgAllocate(self, elem, size, align, 2, false, orig, opType)) { return true; }
    if (tryNonContiguousRange(2)) { return true; }

    // 5. Fine-grained non-contiguous fallback (16KB -> 4KB segment lengths with 64KB+ start alignment)
    //    when VRAM is heavily fragmented:
    if (pageAlignedSize > 0x4000ULL) {
        const UInt64 align16K = (pageAlignedSize + 0x3FFFULL) & ~0x3FFFULL;
        if (inst.orgTotalFreeMemory == nullptr || inst.orgTotalFreeMemory(self) >= align16K) {
            if (inst.orgAllocateNonContiguous(self, elem, align16K, safeAlign, 1, false, false, 0x4000ULL, orig,
                                              opType) ||
                inst.orgAllocateNonContiguous(self, elem, align16K, safeAlign, 2, false, false, 0x4000ULL, orig,
                                              opType)) {
                return true;
            }
        }
    }
    if (pageAlignedSize > PAGE_SIZE) {
        if (inst.orgAllocateNonContiguous(self, elem, pageAlignedSize, safeAlign, 1, false, false, PAGE_SIZE, orig,
                                          opType) ||
            inst.orgAllocateNonContiguous(self, elem, pageAlignedSize, safeAlign, 2, false, false, PAGE_SIZE, orig,
                                          opType) ||
            inst.orgAllocateNonContiguous(self, elem, pageAlignedSize, PAGE_SIZE, 2, false, false, PAGE_SIZE, orig,
                                          opType)) {
            return true;
        }
    }

    return false;
}

void X5000::wrapRTHardwareDidStart(void* const self)
{
    FunctionCast(wrapRTHardwareDidStart, singleton().orgRTHardwareDidStart)(self);
    if (self != nullptr && singleton().gfxOffSupportedOffset != 0) {
        *reinterpret_cast<UInt8*>(reinterpret_cast<uintptr_t>(self) + singleton().gfxOffSupportedOffset) = 0;
        NRed::singleton().setProp32("NRedGFXOFFDisabled", 1);
        DBGLOG("X5000", "Disabled GFXOFF in RTHardware (offset 0x%x)", singleton().gfxOffSupportedOffset);
    }
}

UInt32 X5000::returnZero() { return 0; }

// Replaces SDMA1 field with SDMA0 because we don't have SDMA1
// TODO: Investigate why this is needed.
static void* fixAccelGroup(void* const group)
{
    if (group != nullptr) { getMember<void*>(group, 0x18) = getMember<void*>(group, 0x10); }
    return group;
}

void* X5000::wrapObtainAccelChannelGroup(void* const self, const UInt32 priority)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup, singleton().orgObtainAccelChannelGroup)(self, priority));
}

void* X5000::wrapObtainAccelChannelGroup1304(void* const self, const UInt32 priority, void* const task)
{
    return fixAccelGroup(
        FunctionCast(wrapObtainAccelChannelGroup1304, singleton().orgObtainAccelChannelGroup)(self, priority, task));
}

UInt32 X5000::wrapHwlConvertChipFamily(void* const self, const UInt32 family, const UInt32 revision)
{
    DBGLOG("X5000", "HwlConvertChipFamily >> (self: %p family: 0x%X revision: 0x%X)", self, family, revision);
    if (family == AMD_FAMILY_RAVEN) {
        auto& settings          = singleton().chipSettingsField(self);
        settings.isArcticIsland = 1;
        settings.isRaven        = 1;
        if (NRed::singleton().getAttributes().isRenoir()) {
            settings.htileAlignFix = 1;
            settings.applyAliasFix = 1;
        }
        else if (!NRed::singleton().getAttributes().isRaven2()) {
            settings.depthPipeXorDisable = 1;
        }
        settings.isDcn1           = 1;    // what to do about this?
        settings.metaBaseAlignFix = 1;
        return ADDR_CHIP_FAMILY_AI;
    }
    return FunctionCast(wrapHwlConvertChipFamily, singleton().orgHwlConvertChipFamily)(self, family, revision);
}

UInt32 X5000::computeSubmitCommandBuffer(void* const self, void* const info)
{
    singleton().notifyGfxAccess(singleton().hwChannelHWInterfaceField(self));
    return FunctionCast(computeSubmitCommandBuffer, singleton().orgPM4SubmitCommandBuffer)(self, info);
}

//  -- VRR was introduced on macOS Big Sur. --

namespace
{

    class Constants
    {
        static void _setVrrTimestampInfoVentura(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                                const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfoVentura();
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = vTotalMin;
            vrrTimestampInfo.horizontalLineTime         = horizontalLineTime;
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = vTotalMin;
            vrrTimestampInfo.vTotalMax                  = vTotalMax;
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _setVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt64 vTotalMin,
                                         const UInt64 vTotalMax, const UInt64 horizontalLineTime)
        {
            auto& vrrTimestampInfo                      = self->vrrTimestampInfo();
            vrrTimestampInfo.field0                     = 0;
            vrrTimestampInfo.lastTransactionTimestamp   = 0;
            vrrTimestampInfo.currentFrameStartTimestamp = 0;
            vrrTimestampInfo.lastTransactionStartTime   = 0;
            vrrTimestampInfo.currentFrameVTotal         = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.horizontalLineTime         = static_cast<UInt32>(horizontalLineTime);
            vrrTimestampInfo.currentFrameTime           = 0;
            vrrTimestampInfo.vTotalMin                  = static_cast<UInt32>(vTotalMin);
            vrrTimestampInfo.vTotalMax                  = static_cast<UInt32>(vTotalMax);
            vrrTimestampInfo.transactionOnGlassTime     = 0;
        }

        static void _calcAndSetVrrTimestampInfo(AMDRadeonX5000_AMDHWDisplay* const self,
                                                const FramebufferInfo* const       fbInfo,
                                                const IOTimingInformation&         timingInfo)
        {
            assert(currentKernelVersion() >= MACOS_11);

            if (!fbInfo->isOnline) { return; }

            const auto vTotalMin =
                timingInfo.detailedInfo.v2.verticalBlanking + timingInfo.detailedInfo.v2.verticalActive;
            const auto vTotalMax  = vTotalMin + timingInfo.detailedInfo.v2.verticalBlankingExtension;
            const auto pixelClock = timingInfo.detailedInfo.v2.pixelClock;
            if (pixelClock == 0) { singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, 0); }
            else {
                const auto hTotal =
                    timingInfo.detailedInfo.v2.horizontalBlanking + timingInfo.detailedInfo.v2.horizontalActive;
                const auto hLineTimeNs = static_cast<UInt64>(hTotal) * 1000000000ULL / pixelClock;
                UInt64     horizontalLineTime;
                nanoseconds_to_absolutetime(hLineTimeNs, &horizontalLineTime);
                singleton.setVrrTimestampInfo(self, vTotalMin, vTotalMax, horizontalLineTime);
            }
        }

        static void _calcAndSetVrrTimestampInfoDummy(AMDRadeonX5000_AMDHWDisplay*, const FramebufferInfo*,
                                                     const IOTimingInformation&)
        { assert(currentKernelVersion() <= MACOS_10_15_X); }

    public:
        static const Constants singleton;

        void (*setVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, UInt64 vTotalMin, UInt64 vTotalMax,
                                    const UInt64 horizontalLineTime){nullptr};
        void (*calcAndSetVrrTimestampInfo)(AMDRadeonX5000_AMDHWDisplay* self, const FramebufferInfo* fbInfo,
                                           const IOTimingInformation& timingInfo){_calcAndSetVrrTimestampInfoDummy};

        explicit Constants()
        {
            if (currentKernelVersion() < MACOS_11) { return; }

            this->calcAndSetVrrTimestampInfo = _calcAndSetVrrTimestampInfo;
            this->setVrrTimestampInfo =
                currentKernelVersion() >= MACOS_13 ? _setVrrTimestampInfoVentura : _setVrrTimestampInfo;
        }
    };

    const Constants Constants::singleton;

    constexpr inline auto& vrrConstants = Constants::singleton;

}    // namespace

bool X5000::fixedGetDisplayInfo(AMDRadeonX5000_AMDHWDisplay* const self, const UInt32 fbIndex, const bool isCRTEnabled,
                                const bool ignoreCRTOffsetCheck, IOFramebuffer* const fb, FramebufferInfo* const fbInfo)
{
    if (fb == nullptr || fbIndex >= self->supportedDisplayCount()) { return false; }

    fbInfo->crtOffset   = 0;
    fbInfo->size        = 0;
    fbInfo->crtSize     = 0;
    fbInfo->pitch       = 0;
    fbInfo->rect.width  = 0;
    fbInfo->rect.height = 0;

    auto& displayState = self->displayStates()[fbIndex];

    displayState.framebuffer = fb;
    displayState.status.setIsEnabled(isCRTEnabled);
    displayState.status.setIsIOFBFlipEnabled(true);
    displayState.status.setIsAccelBacked(false);

    uintptr_t wsaa = -1ULL;
    if (fb->getAttribute(ATTRIBUTE_WSAA, &wsaa) == kIOReturnSuccess) {
        self->wsaaAttributes()[fbIndex] = static_cast<UInt32>(wsaa);
        displayState.status.setIsWSAASupported(true);
    }
    else {
        displayState.status.setIsWSAASupported(false);
    }

    uintptr_t  dpt    = 0;
    const auto dptRet = fb->getAttribute(ATTRIBUTE_DISPLAY_PIPE_TRANSACTION, &dpt);
    displayState.status.setIsDPTSupported(dptRet == kIOReturnSuccess || dptRet == kIOReturnNotReady);

    const auto crtOffset = OSDynamicCast(OSData, fb->getProperty("ATY,fb_offset"));
    if (crtOffset != nullptr) {
        const auto data = crtOffset->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtOffset = *static_cast<const UInt64*>(data); }
    }

    const auto crtSize = OSDynamicCast(OSData, fb->getProperty("ATY,fb_size"));
    if (crtSize != nullptr) {
        const auto data = crtSize->getBytesNoCopy();
        if (data != nullptr) { fbInfo->crtSize = *static_cast<const UInt32*>(data); }
    }

    auto aperture = fb->getApertureRange(kIOFBSystemAperture);
    auto vram     = fb->getVRAMRange();

    fbInfo->isMapped = aperture != nullptr && vram != nullptr;

    [[clang::suppress]] OSSafeReleaseNULL(aperture);
    [[clang::suppress]] OSSafeReleaseNULL(vram);

    self->getDisplayModeViewportSpecificInfo(fbIndex, &self->viewportStartYs()[fbIndex],
                                             &self->viewportHeights()[fbIndex]);

    uintptr_t isOnline = 0;
    fbInfo->isOnline =
        fb->getAttributeForConnection(static_cast<IOIndex>(fbIndex), kConnectionEnable, &isOnline) == kIOReturnSuccess
        && isOnline != 0;

    self->fedsParamInfo()[fbIndex].crtIndex = 0;
    self->fedsParamInfo()[fbIndex].scaledW  = 0;
    self->fedsParamInfo()[fbIndex].scaledH  = 0;
    self->fedsParamInfo()[fbIndex].srcW     = 0;
    self->fedsParamInfo()[fbIndex].srcH     = 0;

    self->scalerFlags()[fbIndex] = 0;

    IODisplayModeID displayMode = 0;
    IOIndex         depth       = 0;
    if (fb->getCurrentDisplayMode(&displayMode, &depth) == kIOReturnSuccess) {
        if (fb->getPixelInformation(displayMode, depth, kIOFBSystemAperture, &displayState.pixelInfo)
            == kIOReturnSuccess)
        {
            const auto bytesPerPixel = displayState.pixelInfo.bitsPerPixel / 8;
            if (bytesPerPixel != 0) { fbInfo->pitch = displayState.pixelInfo.bytesPerRow / bytesPerPixel; }
            fbInfo->rect.width  = displayState.pixelInfo.activeWidth;
            fbInfo->rect.height = displayState.pixelInfo.activeHeight;
        }

        IODisplayModeInformation modeInfo;
        memset(&modeInfo, 0, sizeof(modeInfo));
        if (fb->getInformationForDisplayMode(displayMode, &modeInfo) == kIOReturnSuccess
            && (modeInfo.flags & kDisplayModeAcceleratorBackedFlag) != 0)
        {
            displayState.status.setIsAccelBacked(true);
            self->fedsParamInfo()[fbIndex].crtIndex = 1;
        }

        IOTimingInformation timingInfo;
        memset(&timingInfo, 0, sizeof(timingInfo));
        timingInfo.flags = kIODetailedTimingValid;
        if (fb->getTimingInfoForDisplayMode(displayMode, &timingInfo) == kIOReturnSuccess
            && (timingInfo.flags & kIODetailedTimingValid) != 0)
        {
            self->scalerFlags()[fbIndex] = timingInfo.detailedInfo.v2.scalerFlags;

            if (self->fedsParamInfo()[fbIndex].crtIndex == 1) {
                self->fedsParamInfo()[fbIndex].scaledW = timingInfo.detailedInfo.v2.horizontalActive;
                self->fedsParamInfo()[fbIndex].scaledH = timingInfo.detailedInfo.v2.verticalActive;
                self->fedsParamInfo()[fbIndex].srcW    = timingInfo.detailedInfo.v2.horizontalScaled;
                self->fedsParamInfo()[fbIndex].srcH    = timingInfo.detailedInfo.v2.verticalScaled;
            }

            vrrConstants.calcAndSetVrrTimestampInfo(self, fbInfo, timingInfo);
        }
    }

    auto& hwSpecificInfo = self->crtHWSpecificInfos()[fbIndex];

    auto ret = true;

    if (!isCRTEnabled
        || (!ignoreCRTOffsetCheck && fbInfo->crtOffset >= self->getHWInterface()->getHWMemory()->getVisibleSize()))
    {
        fbInfo->crtOffset = 0;
        fbInfo->size      = 0;
    }
    else {
        CRTHWDepth hwDepth;
        switch (displayState.pixelInfo.bitsPerPixel) {
            case 8: {
                hwDepth = CRTHWDepth::DEPTH_8;
            } break;
            case 16: {
                hwDepth = CRTHWDepth::DEPTH_16;
            } break;
            case 32: {
                hwDepth = CRTHWDepth::DEPTH_32;
            } break;
            case 64: {
                hwDepth = CRTHWDepth::DEPTH_64;
            } break;
            default: {
                hwDepth = CRTHWDepth::DEPTH_32;
                ret     = false;
            } break;
        }
        DBGLOG("X5000", "%s hwDepth=%s for bpp %d", __func__, stringifyCRTHWDepth(hwDepth),
               displayState.pixelInfo.bitsPerPixel);
        hwSpecificInfo.graphDepth = hwDepth;

        CRTHWFormat hwFormat;    // bug fix - original code only handled depth 64 and format 10
        switch (displayState.pixelInfo.bitsPerComponent) {
            case 8: {
                hwFormat = CRTHWFormat::FORMAT_8;
            } break;
            case 10: {
                hwFormat = CRTHWFormat::FORMAT_10;
            } break;
            case 12: {
                hwFormat = CRTHWFormat::FORMAT_12;
            } break;
            default: {
                hwFormat = CRTHWFormat::FORMAT_8;
                ret      = false;
            } break;
        }
        DBGLOG("X5000", "%s hwFormat=%s for bpc %d", __func__, stringifyCRTHWFormat(hwFormat),
               displayState.pixelInfo.bitsPerComponent);
        hwSpecificInfo.graphFormat = hwFormat;

        switch (hwDepth) {
            case CRTHWDepth::DEPTH_8: {
                hwSpecificInfo.bytesPerPixel = 1;
            } break;
            case CRTHWDepth::DEPTH_16: {
                hwSpecificInfo.bytesPerPixel = 2;
            } break;
            case CRTHWDepth::DEPTH_32: {
                hwSpecificInfo.bytesPerPixel = 4;
            } break;
            case CRTHWDepth::DEPTH_64: {
                hwSpecificInfo.bytesPerPixel = 8;
            } break;
        }
        hwSpecificInfo.pixelMode = self->getPixelMode(hwDepth, hwFormat);
        DBGLOG("X5000", "%s hwSpecificInfo.pixelMode=%s", __func__, stringifyATIPixelMode(hwSpecificInfo.pixelMode));
        hwSpecificInfo.format = self->getPixelFormat(hwSpecificInfo.pixelMode);
        DBGLOG("X5000", "%s hwSpecificInfo.format=%s", __func__, stringifyATIFormat(hwSpecificInfo.format));
        hwSpecificInfo.isInterlaced = self->isDisplayInterlaceEnabled(fbIndex);
        displayState.status.setIsInterlaced(hwSpecificInfo.isInterlaced);

        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = fbInfo->rect.width;
        surfInfoInput.height       = fbInfo->rect.height;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.resourceType = ADDR_RSRC_TEX_2D;
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        surfInfoInput.swizzleMode =
            self->getHWInterface()->getHWAlignManager()->getPreferredSwizzleMode2(&surfInfoInput);
        self->savedSwizzleModes()[fbIndex]  = surfInfoInput.swizzleMode;
        self->swizzleModes()[fbIndex]       = surfInfoInput.swizzleMode;
        self->savedResourceTypes()[fbIndex] = surfInfoInput.resourceType;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput,
                                                                         &self->surfInfoOutputs()[fbIndex])
            == kIOReturnSuccess)
        {
            fbInfo->size = fbInfo->pitch * self->surfInfoOutputs()[fbIndex].height * hwSpecificInfo.bytesPerPixel;
        }
        else {
            fbInfo->size = 0;
        }
        displayState.status.setIsEnabled(true);
    }

    AMDHWDisplayState::Status combinedStatus;
    for (UInt32 i = 0; i < self->supportedDisplayCount(); i += 1) { combinedStatus |= self->displayStates()[i].status; }
    self->combinedStatus() = combinedStatus;

    fbInfo->savedSize = fbInfo->size;

    if (self->fedsParamInfo()[fbIndex].crtIndex != 0) {
        ADDR2_COMPUTE_SURFACE_INFO_INPUT surfInfoInput;
        surfInfoInput.width        = self->fedsParamInfo()[fbIndex].scaledW;
        surfInfoInput.height       = self->fedsParamInfo()[fbIndex].scaledH;
        surfInfoInput.bpp          = displayState.pixelInfo.bitsPerPixel;
        surfInfoInput.swizzleMode  = self->savedSwizzleModes()[fbIndex];
        surfInfoInput.resourceType = self->savedResourceTypes()[fbIndex];
        surfInfoInput.format     = self->getHWInterface()->getHWAlignManager()->getAddrFormat(hwSpecificInfo.pixelMode);
        surfInfoInput.numSamples = 1;
        surfInfoInput.numSlices  = 1;
        surfInfoInput.flags.display = true;
        ADDR2_COMPUTE_SURFACE_INFO_OUTPUT surfInfoOutput;
        if (self->getHWInterface()->getHWAlignManager()->getSurfaceInfo2(&surfInfoInput, &surfInfoOutput)
            == kIOReturnSuccess)
        {
            fbInfo->savedSize =
                static_cast<UInt64>(surfInfoOutput.height) * surfInfoOutput.pitch * hwSpecificInfo.bytesPerPixel;
        }
    }

    UInt64 baseAlign = self->surfInfoOutputs()[fbIndex].baseAlign;
    UInt8  shift     = 0;
    while (page_size < baseAlign) {
        baseAlign >>= 1;
        shift      += 1;
    }
    fbInfo->pageCount = alignValue(page_size << shift);

    return ret;
}

void X5000::fixedGetSurfaceInfo(AMDRadeonX5000_AMDHWAlignManager* const self, AMD_SURFACE_INFO_STRUCT* const pStruct)
{
    if (pStruct == nullptr) { return; }

    pStruct->outWidth  = 0;
    pStruct->outHeight = 0;

    if (pStruct->version != 1 || pStruct->revision != 0 || pStruct->sizeOf != sizeof(*pStruct)) { return; }

    ADDR2_COMPUTE_SURFACE_INFO_INPUT input;
    input.width         = pStruct->inWidth;
    input.height        = pStruct->inHeight;
    input.bpp           = pStruct->bytesPerPixel * 8;
    input.resourceType  = ADDR_RSRC_TEX_2D;
    input.numSamples    = 1;
    input.numSlices     = 1;
    input.flags.display = 1;
    input.swizzleMode   = self->getPreferredSwizzleMode2(&input);    // this was hardcoded to 0xa

    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT output;
    if (self->getSurfaceInfo2(&input, &output) == kIOReturnSuccess) {
        pStruct->outWidth      = static_cast<UInt16>(output.pitch);
        pStruct->outHeight     = static_cast<UInt16>(output.height);
        pStruct->outTilingMode = input.swizzleMode;    // I think AMD forgot this, albeit seemingly unused
    }
}
