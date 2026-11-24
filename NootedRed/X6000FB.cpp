// AMDRadeonX6000Framebuffer Patches
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

#include "AmdAtomPspDirectoryDummy.hpp"
#include "AmdAtomVramInfoIGP.hpp"
#include <ASICCaps.hpp>
#include <GPUDriversAMD/ATOMBIOS.hpp>
#include <GPUDriversAMD/CAIL/ASICCaps.hpp>
#include <GPUDriversAMD/FB/AmdAsicInfo.hpp>
#include <GPUDriversAMD/FB/AmdDeviceMemoryManager.hpp>
#include <GPUDriversAMD/FB/VidMemType.hpp>
#include <GPUDriversAMD/Family.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <NRedAudio.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <PenguinWizardry/PatcherPlus.hpp>
#include <Regs/OSSSYS_4.hpp>
#include <Regs/SMUIO.hpp>
#include <X6000FB.hpp>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_param.h>
#include <mach/i386/vm_types.h>
#include <mach/kern_return.h>

static const UInt8 kCailAsicCapsTablePattern[] = {0x6E, 0x00, 0x00, 0x00, 0x98, 0x67, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

static const UInt8 kPopulateVramInfoPattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                                                     0x81, 0xEC, 0x08, 0x01, 0x00, 0x00, 0x40, 0x89, 0xF0, 0x40,
                                                     0x89, 0xF0, 0x4C, 0x8D, 0xBD, 0xE0, 0xFE, 0xFF, 0xFF};
static const UInt8 kPopulateVramInfoPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xF0,
                                                     0xFF, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static const UInt8 kIH40IVRingInitHardwarePattern[]     = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                                           0x55, 0x41, 0x54, 0x53, 0x50, 0x40, 0x89, 0xF0, 0x49,
                                                           0x89, 0xF0, 0x40, 0x8B, 0x00, 0x00, 0x44, 0x00, 0x00};
static const UInt8 kIH40IVRingInitHardwarePatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0xFF, 0xF0, 0xFF,
                                                           0xFF, 0xF0, 0xF0, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF};

static const UInt8      kIRQMGRWriteRegisterCallPattern[]          = {0xBE, 0x4F, 0x0E, 0x00, 0x00, 0x4C, 0x89, 0xF7,
                                                                      0x89, 0xC2, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kIRQMGRWriteRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                      0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kIRQMGRWriteRegisterCallPatternJumpInstOff = 10;

static const UInt8      kIRQMGRReadRegisterCallPattern[]          = {0xBE, 0x4F, 0x0E, 0x00, 0x00, 0x4C, 0x89,
                                                                     0xF7, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kIRQMGRReadRegisterCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                     0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr size_t kIRQMGRReadRegisterCallPatternJumpInstOff = 8;

static const UInt8 kDpReceiverPowerCtrlPattern[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                                    0x48, 0x83, 0xEC, 0x10, 0x89, 0xF3, 0xB0, 0x02, 0x28, 0xD8};
static const UInt8 kDpReceiverPowerCtrlPattern1404[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
                                                        0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x41,
                                                        0x89, 0xF7, 0xB0, 0x02, 0x44, 0x28, 0xF8};

static const UInt8      kCreateVramInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                 0xB7, 0x70, 0x3C, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateVramInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateVramInfoCallPatternJumpInstOff = 12;

static const UInt8      kCreatePspDirectoryCallPattern[]     = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                0xB7, 0x70, 0x16, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreatePspDirectoryCallPatternMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreatePspDirectoryCallPatternJumpInstOff = 12;

static const UInt8      kCreateObjectInfoCallPattern[]          = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x8B, 0x43, 0x20, 0x0F,
                                                                   0xB7, 0x70, 0x30, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const UInt8      kCreateObjectInfoCallPatternMask[]      = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                                   0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
static constexpr UInt32 kCreateObjectInfoCallPatternJumpInstOff = 12;

// Fix register read (0xD31 -> 0xD2F) and family ID (0x8F -> 0x8E).
static const UInt8 kPopulateDeviceInfoOriginal[]{0xBE, 0x31, 0x0D, 0x00, 0x00, 0xFF, 0x90, 0x40, 0x01,
                                                 0x00, 0x00, 0xC7, 0x43, 0x00, 0x8F, 0x00, 0x00, 0x00};
static const UInt8 kPopulateDeviceInfoMask[]{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
static const UInt8 kPopulateDeviceInfoPatched[]{0xBE, 0x2F, 0x0D, 0x00, 0x00, 0xFF, 0x90, 0x40, 0x01,
                                                0x00, 0x00, 0xC7, 0x43, 0x00, 0x8E, 0x00, 0x00, 0x00};

// Remove check for Navi family
static const UInt8 kInitializeDmcubServices1Original[] = {0x81, 0x79, 0x2C, 0x8F, 0x00, 0x00, 0x00};
static const UInt8 kInitializeDmcubServices1Patched[]  = {0x39, 0xC0, 0x66, 0x90, 0x66, 0x90, 0x90};

// Set DMCUB ASIC constant to DCN 2.1
static const UInt8 kInitializeDmcubServices2Original[] = {0x83, 0xC0, 0xC4, 0x83, 0xF8, 0x0A, 0xB8,
                                                          0x03, 0x00, 0x00, 0x00, 0x83, 0xD0, 0x00};
static const UInt8 kInitializeDmcubServices2Patched[]  = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x66, 0x90,
                                                          0x66, 0x90, 0x66, 0x90, 0x66, 0x90, 0x90};

// Ditto, 14.4+
static const UInt8 kInitializeDmcubServices2Original1404[] = {0x83, 0xC0, 0xC4, 0x31, 0xC9, 0x83,
                                                              0xF8, 0x0A, 0x83, 0xD1, 0x03};
static const UInt8 kInitializeDmcubServices2Patched1404[]  = {0xB9, 0x02, 0x00, 0x00, 0x00, 0x66,
                                                              0x90, 0x66, 0x90, 0x66, 0x90};

// Ditto, 10.15
static const UInt8 kInitializeDmcubServices2Original1015[] = {0xC7, 0x46, 0x20, 0x01, 0x00, 0x00, 0x00};
static const UInt8 kInitializeDmcubServices2Patched1015[]  = {0xC7, 0x46, 0x20, 0x02, 0x00, 0x00, 0x00};

// 10.15: Set inst_const_size/bss_data_size to 0. To disable DMCUB firmware loading logic.
static const UInt8 kInitializeHardware1Original[] = {0x49, 0xBC, 0x00, 0x0A, 0x01, 0x00, 0xF4, 0x01, 0x00, 0x00};
static const UInt8 kInitializeHardware1Patched[]  = {0x49, 0xC7, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90};

// 10.15: Set fw_inst_const to nullptr, pt.2 of above.
static const UInt8 kInitializeHardware2Original[]     = {0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x00, 0x00,
                                                         0x10, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C};
static const UInt8 kInitializeHardware2OriginalMask[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
                                                         0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
static const UInt8 kInitializeHardware2Patched[]      = {0x49, 0xC7, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kInitializeHardware2PatchedMask[]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00,
                                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 10.15: Disable DMCUB firmware loading from DAL. HWLibs should be doing that.
static const UInt8 kAmdDalServicesInitializeOriginal[]     = {0xBE, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x00,
                                                              0x00, 0x00, 0x00, 0x49, 0x00, 0x00, 0x60};
static const UInt8 kAmdDalServicesInitializeOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
                                                              0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFF};
static const UInt8 kAmdDalServicesInitializePatched[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kAmdDalServicesInitializePatchedMask[]  = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
                                                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Change cursor and underflow tracker count to 4 instead of 6.
static const UInt8 kCreateControllerServicesOriginal[]     = {0x40, 0x00, 0x00, 0x40, 0x83, 0x00, 0x06};
static const UInt8 kCreateControllerServicesOriginalMask[] = {0xF0, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
static const UInt8 kCreateControllerServicesPatchedMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Ditto, 10.15.
static const UInt8 kCreateControllerServicesOriginal1015[]     = {0x48, 0x00, 0x00, 0x48, 0x83, 0x00, 0x05};
static const UInt8 kCreateControllerServicesOriginalMask1015[] = {0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF};
static const UInt8 kCreateControllerServicesPatched1015[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
static const UInt8 kCreateControllerServicesPatchedMask1015[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F};

// Change cursor count to 4 instead of 6.
static const UInt8 kSetupCursorsOriginal[]     = {0x40, 0x83, 0x00, 0x05};
static const UInt8 kSetupCursorsOriginalMask[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched[]      = {0x00, 0x00, 0x00, 0x03};
static const UInt8 kSetupCursorsPatchedMask[]  = {0x00, 0x00, 0x00, 0x0F};

// Ditto, 12.0+.
static const UInt8 kSetupCursorsOriginal12[]     = {0x40, 0x83, 0x00, 0x06};
static const UInt8 kSetupCursorsOriginalMask12[] = {0xF0, 0xFF, 0x00, 0xFF};
static const UInt8 kSetupCursorsPatched12[]      = {0x00, 0x00, 0x00, 0x04};
static const UInt8 kSetupCursorsPatchedMask12[]  = {0x00, 0x00, 0x00, 0x0F};

// Change link count to 4 instead of 6.
static const UInt8 kCreateLinksOriginal[]     = {0x06, 0x00, 0x00, 0x00, 0x40};
static const UInt8 kCreateLinksOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
static const UInt8 kCreateLinksPatched[]      = {0x04, 0x00, 0x00, 0x00, 0x00};
static const UInt8 kCreateLinksPatchedMask[]  = {0x0F, 0x00, 0x00, 0x00, 0x00};

// Remove new FB count condition so we can restore the original behaviour before Ventura.
static const UInt8 kControllerPowerUpOriginal[]     = {0x38, 0xC8, 0x0F, 0x42, 0xC8, 0x88, 0x8F,
                                                       0xBC, 0x00, 0x00, 0x00, 0x72, 0x00};
static const UInt8 kControllerPowerUpOriginalMask[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static const UInt8 kControllerPowerUpReplace[]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xEB, 0x00};
static const UInt8 kControllerPowerUpReplaceMask[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                       0x00, 0x00, 0x00, 0x00, 0xFF, 0x00};

// Remove new problematic Ventura pixel clock multiplier calculation which causes timing validation mishaps.
static const UInt8 kValidateDetailedTimingOriginal[] = {0x66, 0x0F, 0x2E, 0xC1, 0x76, 0x06, 0xF2, 0x0F, 0x5E, 0xC1};
static const UInt8 kValidateDetailedTimingPatched[]  = {0x66, 0x0F, 0x2E, 0xC1, 0x66, 0x90, 0xF2, 0x0F, 0x5E, 0xC1};

// Fix Navi10 (DCN 2.0) hardcoded OTG_V_TOTAL_MIN/MAX/CONTROL (0x1B88..0x1B8A) -> Raven/Renoir (0x1B48..0x1B4A)
// inside dce110_timing_generator_set_drr across macOS 13-26 so hardware DRR/VRR updates reach the APU OTG.
static const UInt8 kSetDrrRead1Orig[] = {0xBE, 0x88, 0x1B, 0x00, 0x00, 0x03, 0x77, 0x20};
static const UInt8 kSetDrrRead1Pat[]  = {0xBE, 0x48, 0x1B, 0x00, 0x00, 0x03, 0x77, 0x20};

static const UInt8 kSetDrrRead2Orig13[]  = {0xBE, 0x89, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x76, 0x20};
static const UInt8 kSetDrrRead2Pat13[]   = {0xBE, 0x49, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x76, 0x20};
static const UInt8 kSetDrrRead3Orig13[]  = {0xBE, 0x8A, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x76, 0x20};
static const UInt8 kSetDrrRead3Pat13[]   = {0xBE, 0x4A, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x76, 0x20};
static const UInt8 kSetDrrWrite1Orig13[] = {0xBB, 0x88, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};
static const UInt8 kSetDrrWrite1Pat13[]  = {0xBB, 0x48, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};
static const UInt8 kSetDrrWrite2Orig13[] = {0xBB, 0x89, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};
static const UInt8 kSetDrrWrite2Pat13[]  = {0xBB, 0x49, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};
static const UInt8 kSetDrrWrite3Orig13[] = {0xBB, 0x8A, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};
static const UInt8 kSetDrrWrite3Pat13[]  = {0xBB, 0x4A, 0x1B, 0x00, 0x00, 0x41, 0x03, 0x5E, 0x20};

static const UInt8 kSetDrrRead2Orig15[]  = {0xBE, 0x89, 0x1B, 0x00, 0x00, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrRead2Pat15[]   = {0xBE, 0x49, 0x1B, 0x00, 0x00, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrRead3Orig15[]  = {0xBE, 0x8A, 0x1B, 0x00, 0x00, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrRead3Pat15[]   = {0xBE, 0x4A, 0x1B, 0x00, 0x00, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrWrite1Orig15[] = {0x41, 0xBD, 0x88, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x6B, 0x20};
static const UInt8 kSetDrrWrite1Pat15[]  = {0x41, 0xBD, 0x48, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x6B, 0x20};
static const UInt8 kSetDrrWrite2Orig15[] = {0x41, 0xBE, 0x89, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrWrite2Pat15[]  = {0x41, 0xBE, 0x49, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrWrite3Orig15[] = {0x41, 0xBE, 0x8A, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x73, 0x20};
static const UInt8 kSetDrrWrite3Pat15[]  = {0x41, 0xBE, 0x4A, 0x1B, 0x00, 0x00, 0x44, 0x03, 0x73, 0x20};

static X6000FB moduleInstance;

X6000FB& X6000FB::singleton() { return moduleInstance; }

void X6000FB::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    if (kextRadeonX6000Framebuffer.loadIndex != id) { return; }

    NRed::singleton().hwLateInit();

    CAILAsicCapsEntry*                   orgAsicCapsTable       = nullptr;
    void*                                orgAmdAsicInfoNavi10VT = nullptr;
    PenguinWizardry::PatternSolveRequest solveRequests[]        = {
        {"__ZL20CAIL_ASIC_CAPS_TABLE", orgAsicCapsTable, kCailAsicCapsTablePattern},
        {"__ZN37AMDRadeonX6000_AmdDeviceMemoryManager17mapMemorySubRangeE25AmdReservedMemorySelectoryyj",
         this->mapMemorySubRange},
        {"__ZTV32AMDRadeonX6000_AmdAsicInfoNavi10", orgAmdAsicInfoNavi10VT},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readEfiAtomBiosImageEPhm", this->readEfiAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper20readPciAtomBiosImageEPhm", this->readPciAtomBiosImage},
        {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper21validateAtomBiosImageEPhm", this->validateAtomBiosImage},
    };
    PANIC_COND(!PenguinWizardry::PatternSolveRequest::solveAll(patcher, id, solveRequests, slide, size), "X6000FB",
               "Failed to resolve symbols");

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X6000FB",
               "Failed to enable kernel writing");
    getMember<decltype(getGpuBrandingNameListRenoir)*>(orgAmdAsicInfoNavi10VT, 0x228) =
        NRed::singleton().getAttributes().isRenoir()  ? getGpuBrandingNameListRenoir :
        NRed::singleton().getAttributes().isPicasso() ? getGpuBrandingNameListPicasso :
                                                        getGpuBrandingNameListRaven;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    if (checkKernelArgument("-NRedDPDelay")) {
        if (currentKernelVersion() >= MACOS_14_4) {
            PenguinWizardry::PatternRouteRequest request{"_dp_receiver_power_ctrl", wrapDpReceiverPowerCtrl,
                                                         this->orgDpReceiverPowerCtrl, kDpReceiverPowerCtrlPattern1404};
            PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB",
                       "Failed to route dp_receiver_power_ctrl (14.4+)");
        }
        else {
            PenguinWizardry::PatternRouteRequest request{"_dp_receiver_power_ctrl", wrapDpReceiverPowerCtrl,
                                                         this->orgDpReceiverPowerCtrl, kDpReceiverPowerCtrlPattern};
            PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB", "Failed to route dp_receiver_power_ctrl");
        }
    }

    if (currentKernelVersion() >= MACOS_13) {
        this->orgMessageAccelerator = patcher.solveSymbol<decltype(this->orgMessageAccelerator)>(
            id, "__ZNK34AMDRadeonX6000_AmdRadeonController18messageAcceleratorE25_eAMDAccelIOFBRequestTypePvS1_S1_",
            slide, size);
        PANIC_COND(this->orgMessageAccelerator == nullptr, "X6000FB", "Failed to resolve messageAccelerator");

        KernelPatcher::RouteRequest request{"__ZN34AMDRadeonX6000_AmdRadeonController7powerUpEv", wrapControllerPowerUp,
                                            this->orgControllerPowerUp};
        PANIC_COND(!patcher.routeMultiple(id, &request, 1, slide, size), "X6000FB", "Failed to route powerUp");

        if (checkKernelArgument("-NRedOLED")) {
            this->oledEnabled = true;
            if (this->fbPowerLock == nullptr) { this->fbPowerLock = IOLockAlloc(); }
            KernelPatcher::RouteRequest fbPowerRequest{
                "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer18doPowerStateChangeENS_15AmdFbPowerStateE",
                wrapDoPowerStateChange, this->orgDoPowerStateChange};
            if (!patcher.routeMultiple(id, &fbPowerRequest, 1, slide, size)) {
                patcher.clearError();
                SYSLOG("X6000FB", "Failed to route doPowerStateChange (non-fatal)");
            }
        }

        this->vrrEnabled = !checkKernelArgument("-NRedNoVRR");

        KernelPatcher::RouteRequest displayPipeFixes[] = {
            {"__ZN35AMDRadeonX6000_AmdRadeonFramebuffer12getAttributeEjPm", wrapFbGetAttribute,
             this->orgFbGetAttribute},
            {"__ZN29AMDRadeonX6000_AmdFbPresenter18handleTransactionsEP21_AMD_PIPE_TRANSACTION",
             wrapPresenterHandleTransactions, this->orgPresenterHandleTransactions},
            {"__ZN29AMDRadeonX6000_AmdFbPresenter26getOutstandingTransactionsEv",
             wrapPresenterGetOutstandingTransactions, this->orgPresenterGetOutstandingTransactions},
            {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi11isPoweredUpEv", wrapControllerIsPoweredUp,
             this->orgControllerIsPoweredUp},
            {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi12getDrrStatusEiP10_DrrStatus", wrapGetDrrStatus,
             this->orgGetDrrStatus},
            {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi24getVUpdateVBlankIntervalEiP22_VUpdateVBlankInterval",
             wrapGetVUpdateVBlankInterval, this->orgGetVUpdateVBlankInterval},
            {"__ZN38AMDRadeonX6000_AmdRadeonControllerNavi19setupBootWatermarksEv", wrapSetupBootWatermarks,
             this->orgSetupBootWatermarks},
        };
        for (auto& req : displayPipeFixes) {
            if (!patcher.routeMultiple(id, &req, 1, slide, size)) {
                patcher.clearError();
                SYSLOG("X6000FB", "Optional display/controller route %s unavailable; continuing", req.symbol);
            }
        }

        const PenguinWizardry::MaskedLookupPatch patches[] = {
            {&kextRadeonX6000Framebuffer, kControllerPowerUpOriginal, kControllerPowerUpOriginalMask,
             kControllerPowerUpReplace, kControllerPowerUpReplaceMask, 1},
            {&kextRadeonX6000Framebuffer, kValidateDetailedTimingOriginal, kValidateDetailedTimingPatched, 1},
        };
        PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X6000FB",
                   "Failed to apply logic revert patches");

        if (this->vrrEnabled) {
            const PenguinWizardry::MaskedLookupPatch drrPatches15[] = {
                {&kextRadeonX6000Framebuffer, kSetDrrRead1Orig, kSetDrrRead1Pat, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrRead2Orig15, kSetDrrRead2Pat15, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrRead3Orig15, kSetDrrRead3Pat15, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite1Orig15, kSetDrrWrite1Pat15, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite2Orig15, kSetDrrWrite2Pat15, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite3Orig15, kSetDrrWrite3Pat15, 1},
            };
            const PenguinWizardry::MaskedLookupPatch drrPatches13[] = {
                {&kextRadeonX6000Framebuffer, kSetDrrRead1Orig, kSetDrrRead1Pat, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrRead2Orig13, kSetDrrRead2Pat13, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrRead3Orig13, kSetDrrRead3Pat13, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite1Orig13, kSetDrrWrite1Pat13, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite2Orig13, kSetDrrWrite2Pat13, 1},
                {&kextRadeonX6000Framebuffer, kSetDrrWrite3Orig13, kSetDrrWrite3Pat13, 1},
            };
            const bool drrOk = currentKernelVersion() >= MACOS_14_4
                                   ? PenguinWizardry::MaskedLookupPatch::applyAll(patcher, drrPatches15, slide, size)
                                   : PenguinWizardry::MaskedLookupPatch::applyAll(patcher, drrPatches13, slide, size);
            if (!drrOk) {
                patcher.clearError();
                SYSLOG("X6000FB", "Optional DCN OTG DRR register patch unavailable; continuing");
            }
        }
    }

    PenguinWizardry::PatternRouteRequest requests[] = {
        {"__ZNK15AmdAtomVramInfo16populateVramInfoER16AtomFirmwareInfo", wrapPopulateVramInfo, kPopulateVramInfoPattern,
         kPopulateVramInfoPatternMask},
        {"__ZNK32AMDRadeonX6000_AmdAsicInfoNavi1027getEnumeratedRevisionNumberEv", getEnumeratedRevision},
        {"__ZN41AMDRadeonX6000_AmdDeviceMemoryManagerNavi21intializeReservedVramEv", initialiseReservedVRAM},
        {"__ZNK30AMDRadeonX6000_AmdAgdcServices13getVendorInfoEP16AGDCVendorInfo_tm", wrapGetVendorInfo,
         this->orgGetVendorInfo},
        {"__ZN34AMDRadeonX6000_AmdBiosParserHelper12readAtomBiosEv", readAtomBios},
    };
    PANIC_COND(!PenguinWizardry::PatternRouteRequest::routeAll(patcher, id, requests, slide, size), "X6000FB",
               "Failed to route symbols");

    KernelPatcher::RouteRequest linkInfoReq{
        "__ZN35AMDRadeonX6000_AmdRadeonFramebuffer27reportCapabilities_LinkInfoEv",
        wrapReportCapabilitiesLinkInfo,
        this->orgReportCapabilitiesLinkInfo,
    };
    if (!patcher.routeMultiple(id, &linkInfoReq, 1, slide, size)) {
        patcher.clearError();
        SYSLOG("X6000FB", "Optional route reportCapabilities_LinkInfo unavailable; continuing");
    }

    PenguinWizardry::JumpPatternRouteRequest atombiosRequests[] = {
        {"__ZN15AmdAtomVramInfo14createVramInfoEP15AmdAtomFwHelperj", wrapCreateVramInfo, this->orgCreateVramInfo,
         kCreateVramInfoCallPattern, kCreateVramInfoCallPatternMask, kCreateVramInfoCallPatternJumpInstOff},
        {"__ZN17AmdAtomObjectInfo16createObjectInfoEP15AmdAtomFwHelperj", wrapCreateObjectInfo,
         this->orgCreateObjectInfo, kCreateObjectInfoCallPattern, kCreateObjectInfoCallPatternMask,
         kCreateObjectInfoCallPatternJumpInstOff},
    };
    PANIC_COND(!PenguinWizardry::JumpPatternRouteRequest::routeAll(patcher, id, atombiosRequests, slide, size),
               "X6000FB", "Failed to route ATOMBIOS-related functions");

    if (currentKernelVersion() >= MACOS_11) {
        PenguinWizardry::JumpPatternRouteRequest createPspDirectoryRequest{
            "__ZN19AmdAtomPspDirectory18createPspDirectoryEP15AmdAtomFwHelperj",
            wrapCreatePspDirectory,
            this->orgCreatePspDirectory,
            kCreatePspDirectoryCallPattern,
            kCreatePspDirectoryCallPatternMask,
            kCreatePspDirectoryCallPatternJumpInstOff};
        PANIC_COND(!createPspDirectoryRequest.route(patcher, id, slide, size), "X6000FB",
                   "Failed to route createPspDirectory");

        if (currentKernelVersion() <= MACOS_12_X) {
            PenguinWizardry::PatternRouteRequest getTriageHardwareDataRequest{
                "__ZN38AMDRadeonX6000_AmdRadeonControllerNavi21getTriageHardwareDataEjP12_AMD_TRIAGE_",
                NRed::singleton().getAttributes().isRenoir() ? getTriageHardwareDataRN : getTriageHardwareDataRV};
            PANIC_COND(!getTriageHardwareDataRequest.route(patcher, id, slide, size), "X6000FB",
                       "Failed to route getTriageHardwareData");
        }

        KernelPatcher::RouteRequest request{
            "__ZN32AMDRadeonX6000_AmdRegisterAccess20createRegisterAccessERNS_8InitDataE", wrapCreateRegisterAccess,
            this->orgCreateRegisterAccess};
        PANIC_COND(!patcher.routeMultiple(id, &request, 1, slide, size), "X6000FB",
                   "Failed to route createRegisterAccess");
    }

    if (NRed::singleton().getAttributes().isRenoir()) {
        PenguinWizardry::PatternRouteRequest request{"_IH_4_0_IVRing_InitHardware", wrapIH40IVRingInitHardware,
                                                     this->orgIH40IVRingInitHardware, kIH40IVRingInitHardwarePattern,
                                                     kIH40IVRingInitHardwarePatternMask};
        PANIC_COND(!request.route(patcher, id, slide, size), "X6000FB", "Failed to route IH_4_0_IVRing_InitHardware");
        PenguinWizardry::JumpPatternRouteRequest jumpPatternRequest{"_IRQMGR_WriteRegister",
                                                                    wrapIRQMGRWriteRegister,
                                                                    this->orgIRQMGRWriteRegister,
                                                                    kIRQMGRWriteRegisterCallPattern,
                                                                    kIRQMGRWriteRegisterCallPatternMask,
                                                                    kIRQMGRWriteRegisterCallPatternJumpInstOff};
        PANIC_COND(!jumpPatternRequest.route(patcher, id, slide, size), "X6000FB",
                   "Failed to route IRQMGR_WriteRegister");
        PenguinWizardry::JumpPatternSolveRequest jumpPatternSolveRequest{
            "_IRQMGR_ReadRegister", this->irqMGRReadRegister, kIRQMGRReadRegisterCallPattern,
            kIRQMGRReadRegisterCallPatternMask, kIRQMGRReadRegisterCallPatternJumpInstOff};
        PANIC_COND(!jumpPatternSolveRequest.solve(patcher, id, slide, size), "X6000FB",
                   "Failed to solve IRQMGR_ReadRegister");
    }

    const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer, kPopulateDeviceInfoOriginal,
                                                   kPopulateDeviceInfoMask,     kPopulateDeviceInfoPatched,
                                                   kPopulateDeviceInfoMask,     1};
    PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB", "Failed to apply populateDeviceInfo patch");

    if (NRed::singleton().getAttributes().isRenoir()) {
        const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer, kInitializeDmcubServices1Original,
                                                       kInitializeDmcubServices1Patched, 1};
        PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                   "Failed to apply initializeDmcubServices family id patch");
        if (currentKernelVersion() <= MACOS_10_15_X) {
            const PenguinWizardry::MaskedLookupPatch patches[] = {
                {&kextRadeonX6000Framebuffer, kInitializeDmcubServices2Original1015,
                 kInitializeDmcubServices2Patched1015, 1},
                {&kextRadeonX6000Framebuffer, kInitializeHardware1Original, kInitializeHardware1Patched, 1},
                {&kextRadeonX6000Framebuffer, kInitializeHardware2Original, kInitializeHardware2OriginalMask,
                 kInitializeHardware2Patched, kInitializeHardware2PatchedMask, 1},
                {&kextRadeonX6000Framebuffer, kAmdDalServicesInitializeOriginal, kAmdDalServicesInitializeOriginalMask,
                 kAmdDalServicesInitializePatched, kAmdDalServicesInitializePatchedMask, 1},
            };
            PANIC_COND(!PenguinWizardry::MaskedLookupPatch::applyAll(patcher, patches, slide, size), "X6000FB",
                       "Failed to apply AmdDalDmcubService and AmdDalServices::initialize patches (10.15)");
        }
        else if (currentKernelVersion() >= MACOS_14_4) {
            const PenguinWizardry::MaskedLookupPatch patch{&kextRadeonX6000Framebuffer,
                                                           kInitializeDmcubServices2Original1404,
                                                           kInitializeDmcubServices2Patched1404, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                       "Failed to apply initializeDmcubServices ASIC patch (14.4+)");
        }
        else {
            const PenguinWizardry::MaskedLookupPatch patch{
                &kextRadeonX6000Framebuffer, kInitializeDmcubServices2Original, kInitializeDmcubServices2Patched, 1};
            PANIC_COND(!patch.apply(patcher, slide, size), "X6000FB",
                       "Failed to apply initializeDmcubServices ASIC patch");
        }
    }

    PANIC_COND(MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS, "X6000FB",
               "Failed to enable kernel writing");
    orgAsicCapsTable->familyId = AMD_FAMILY_RAVEN;
    orgAsicCapsTable->ddiCaps  = NRed::singleton().getAttributes().isRenoirE() ? ddiCapsRenoirE :
                                 NRed::singleton().getAttributes().isRenoir()  ? ddiCapsRenoir :
                                                                                 ddiCapsRaven;
    orgAsicCapsTable->deviceId = NRed::singleton().getDeviceID();
    orgAsicCapsTable->revision = NRed::singleton().getDevRevision();
    orgAsicCapsTable->extRevision =
        static_cast<UInt32>(NRed::singleton().getEnumRevision()) + NRed::singleton().getDevRevision();
    orgAsicCapsTable->pciRevision = NRed::singleton().getPciRevision();
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    DBGLOG("X6000FB", "Applied DDI Caps patches");

    // We need to patch the kext to create only 4 cursors, links and underflow trackers.
    auto* const orgCreateControllerServices = patcher.solveSymbol<void*>(
        id, "__ZN40AMDRadeonX6000_AmdRadeonControllerNavi1024createControllerServicesEv", slide, size, true);
    PANIC_COND(orgCreateControllerServices == nullptr, "X6000FB", "Failed to solve createControllerServices");

    auto* const orgSetupCursors =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController12setupCursorsEv", slide, size, true);
    PANIC_COND(orgSetupCursors == nullptr, "X6000FB", "Failed to solve setupCursors");

    auto* const orgCreateLinks =
        patcher.solveSymbol<void*>(id, "__ZN34AMDRadeonX6000_AmdRadeonController11createLinksEv", slide, size, true);
    PANIC_COND(orgCreateLinks == nullptr, "X6000FB", "Failed to solve createLinks");

    if (currentKernelVersion() <= MACOS_10_15_X) {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal1015,
                       kCreateControllerServicesOriginalMask1015, kCreateControllerServicesPatched1015,
                       kCreateControllerServicesPatchedMask1015, 1, 0),
                   "X6000FB", "Failed to apply createControllerServices patch (10.15)");
    }
    else {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(
                       orgCreateControllerServices, PAGE_SIZE, kCreateControllerServicesOriginal,
                       kCreateControllerServicesOriginalMask, kCreateControllerServicesPatched,
                       kCreateControllerServicesPatchedMask, 2, 0),
                   "X6000FB", "Failed to apply createControllerServices patch");
    }

    if (currentKernelVersion() >= MACOS_12) {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgSetupCursors, PAGE_SIZE, kSetupCursorsOriginal12,
                                                          kSetupCursorsOriginalMask12, kSetupCursorsPatched12,
                                                          kSetupCursorsPatchedMask12, 1, 0),
                   "X6000FB", "Failed to apply setupCursors patch (12.0+)");
    }
    else {
        PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgSetupCursors, PAGE_SIZE, kSetupCursorsOriginal,
                                                          kSetupCursorsOriginalMask, kSetupCursorsPatched,
                                                          kSetupCursorsPatchedMask, 1, 0),
                   "X6000FB", "Failed to apply setupCursors patch");
    }

    PANIC_COND(!KernelPatcher::findAndReplaceWithMask(orgCreateLinks, PAGE_SIZE, kCreateLinksOriginal,
                                                      kCreateLinksOriginalMask, kCreateLinksPatched,
                                                      kCreateLinksPatchedMask, 1, 0),
               "X6000FB", "Failed to apply createLinks patch");
}

UInt16 X6000FB::getEnumeratedRevision() { return NRed::singleton().getEnumRevision(); }

enum IRQMgrIVRingMemoryType
{
    IRQMgrIVRingMemoryTypeGART        = 0x0,
    IRQMgrIVRingMemoryTypeSysPhysical = 0x1,
    IRQMgrIVRingMemoryTypeFB          = 0x2,
};

bool X6000FB::wrapIH40IVRingInitHardware(void* const ctx, void* const ring)
{
    if (getMember<IRQMgrIVRingMemoryType>(ring, 0x24) == IRQMgrIVRingMemoryTypeSysPhysical) {
        singleton().orgIRQMGRWriteRegister(ctx, IH_CHICKEN,
                                           singleton().irqMGRReadRegister(ctx, IH_CHICKEN) | IH_MC_SPACE_GPA_ENABLE);
    }
    return FunctionCast(wrapIH40IVRingInitHardware, singleton().orgIH40IVRingInitHardware)(ctx, ring);
}

void X6000FB::wrapIRQMGRWriteRegister(void* const ctx, const UInt64 off, UInt32 value)
{
    if (off == IH_CLK_CTRL) {
        if ((value & getBit(IH_DBUS_MUX_CLK_SOFT_OVERRIDE_SHIFT)) != 0) {
            value |= getBit(IH_IH_BUFFER_MEM_CLK_SOFT_OVERRIDE_SHIFT);
        }
    }
    singleton().orgIRQMGRWriteRegister(ctx, off, value);
}

void* X6000FB::wrapCreateRegisterAccess(void* const initData)
{
    getMember<UInt32>(initData, 0x24) = SMUIO_BASE_0 + ROM_INDEX;
    getMember<UInt32>(initData, 0x28) = SMUIO_BASE_0 + ROM_DATA;
    return FunctionCast(wrapCreateRegisterAccess, singleton().orgCreateRegisterAccess)(initData);
}

IOReturn X6000FB::initialiseReservedVRAM(void* const self)
{
#define CHECK(_expr)                                                     \
    if (const auto ret = _expr; ret != kIOReturnSuccess) { return ret; }
    static constexpr IOOptionBits mapOptions = kIOMapWriteCombineCache | kIOMapAnywhere;
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_32bpp, 0, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor1_2bpp, 0x40000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_32bpp, 0x80000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor2_2bpp, 0xC0000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_32bpp, 0x100000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor3_2bpp, 0x140000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_32bpp, 0x180000, 0x40000, mapOptions));
    CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::Cursor4_2bpp, 0x1C0000, 0x40000, mapOptions));
    CHECK(
        singleton().mapMemorySubRange(self, AmdReservedMemorySelector::PPLIBReserved, 0x200000, 0x100000, mapOptions));
    if (NRed::singleton().getAttributes().isRenoir()) {
        CHECK(singleton().mapMemorySubRange(self, AmdReservedMemorySelector::DMCUBReserved, 0x300000, 0x100000,
                                            mapOptions));
        return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x400000, mapOptions);
    }
    return singleton().mapMemorySubRange(self, AmdReservedMemorySelector::ReserveVRAM, 0, 0x300000, mapOptions);
#undef CHECK
}

static const AmdAsicBrandingTableEntry ravenBrandingTable[] = {
    {0x15DD, 0x81, "Radeon RX", "Vega 11"}, {0x15DD, 0x82, "Radeon RX", "Vega 8"},
    {0x15DD, 0x83, "Radeon RX", "Vega 8"},  {0x15DD, 0x84, "Radeon RX", "Vega 6"},
    {0x15DD, 0x85, "Radeon RX", "Vega 3"},  {0x15DD, 0x86, "Radeon RX", "Vega 11"},
    {0x15DD, 0x88, "Radeon RX", "Vega 8"},  {0x15DD, 0xC1, "Radeon RX", "Vega 11"},
    {0x15DD, 0xC2, "Radeon RX", "Vega 8"},  {0x15DD, 0xC3, "Radeon RX", "Vega 10"},
    {0x15DD, 0xC4, "Radeon RX", "Vega 8"},  {0x15DD, 0xC5, "Radeon RX", "Vega 3"},
    {0x15DD, 0xC6, "Radeon RX", "Vega 11"}, {0x15DD, 0xC8, "Radeon RX", "Vega 8"},
    {0x15DD, 0xC9, "Radeon RX", "Vega 11"}, {0x15DD, 0xCA, "Radeon RX", "Vega 8"},
    {0x15DD, 0xCB, "Radeon RX", "Vega 3"},  {0x15DD, 0xCC, "Radeon RX", "Vega 6"},
    {0x15DD, 0xCE, "Radeon RX", "Vega 3"},  {0x15DD, 0xCF, "Radeon RX", "Vega 3"},
    {0x15DD, 0xD0, "Radeon RX", "Vega 10"}, {0x15DD, 0xD1, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD3, "Radeon RX", "Vega 11"}, {0x15DD, 0xD5, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD6, "Radeon RX", "Vega 11"}, {0x15DD, 0xD7, "Radeon RX", "Vega 8"},
    {0x15DD, 0xD8, "Radeon RX", "Vega 3"},  {0x15DD, 0xD9, "Radeon RX", "Vega 6"},
    {0x15DD, 0xE1, "Radeon RX", "Vega 3"},  {0x15DD, 0xE2, "Radeon RX", "Vega 3"},
    {"Radeon RX", "Raven Graphics"},
};

static const AmdAsicBrandingTableEntry picassoBrandingTable[] = {
    {0x15D8, 0x00, "Radeon RX", "Vega 8 WS"}, {0x15D8, 0x91, "Radeon RX", "Vega 3"},
    {0x15D8, 0x92, "Radeon RX", "Vega 3"},    {0x15D8, 0x93, "Radeon RX", "Vega 1"},
    {0x15D8, 0xA1, "Radeon RX", "Vega 10"},   {0x15D8, 0xA2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xA3, "Radeon RX", "Vega 6"},    {0x15D8, 0xA4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xB1, "Radeon RX", "Vega 10"},   {0x15D8, 0xB2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xB3, "Radeon RX", "Vega 6"},    {0x15D8, 0xB4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xC1, "Radeon RX", "Vega 10"},   {0x15D8, 0xC2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xC3, "Radeon RX", "Vega 6"},    {0x15D8, 0xC4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xC5, "Radeon RX", "Vega 3"},    {0x15D8, 0xC8, "Radeon RX", "Vega 11"},
    {0x15D8, 0xC9, "Radeon RX", "Vega 8"},    {0x15D8, 0xCA, "Radeon RX", "Vega 11"},
    {0x15D8, 0xCB, "Radeon RX", "Vega 8"},    {0x15D8, 0xCC, "Radeon RX", "Vega 3"},
    {0x15D8, 0xCE, "Radeon RX", "Vega 3"},    {0x15D8, 0xCF, "Radeon RX", "Vega 3"},
    {0x15D8, 0xD1, "Radeon RX", "Vega 10"},   {0x15D8, 0xD2, "Radeon RX", "Vega 8"},
    {0x15D8, 0xD3, "Radeon RX", "Vega 6"},    {0x15D8, 0xD4, "Radeon RX", "Vega 3"},
    {0x15D8, 0xD8, "Radeon RX", "Vega 11"},   {0x15D8, 0xD9, "Radeon RX", "Vega 8"},
    {0x15D8, 0xDA, "Radeon RX", "Vega 11"},   {0x15D8, 0xDB, "Radeon RX", "Vega 8"},
    {0x15D8, 0xDC, "Radeon RX", "Vega 3"},    {0x15D8, 0xDD, "Radeon RX", "Vega 3"},
    {0x15D8, 0xDE, "Radeon RX", "Vega 3"},    {0x15D8, 0xDF, "Radeon RX", "Vega 3"},
    {0x15D8, 0xE1, "Radeon RX", "Vega 11"},   {0x15D8, 0xE2, "Radeon RX", "Vega 9"},
    {0x15D8, 0xE3, "Radeon RX", "Vega 3"},    {0x15D8, 0xE4, "Radeon RX", "Vega 3"},
    {"Radeon RX", "Picasso Graphics"},
};

static const AmdAsicBrandingTableEntry renoirBrandingTable[] = {
    {0x1636, 0xD1, "Radeon Pro", "Graphics"},
    {0x1636, 0xD3, "Radeon Pro", "Graphics"},
    {"Radeon RX", "Renoir Graphics"},
};

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListRaven(const void*) { return ravenBrandingTable; }

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListPicasso(const void*) { return picassoBrandingTable; }

const AmdAsicBrandingTableEntry* X6000FB::getGpuBrandingNameListRenoir(const void*) { return renoirBrandingTable; }

IOReturn X6000FB::dummyIOReturnSuccess() { return kIOReturnSuccess; }

IOReturn X6000FB::getTriageHardwareDataRV(void*, const UInt32 fbIndex, void* const triageData)
{
    auto& bufferPointer = getMember<char*>(triageData, 0x0);
    auto& bufferSize    = getMember<UInt32>(triageData, 0x8);

    if (bufferSize < 2) { return kIOReturnNoResources; }
    if (fbIndex >= 4) { return kIOReturnSuccess; }

    const auto odmOptcInputGlobalControl = NRed::singleton().readReg32(DCN_BASE_2 + 0x1ACA + (0x10 * fbIndex));
    const auto otgMasterEn               = NRed::singleton().readReg32(DCN_BASE_2 + 0x1B5F + (0x80 * fbIndex));
    const auto hubpClkControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x567 + (0xC4 * fbIndex));
    const auto digBeEnControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x20B0 + (0x100 * fbIndex));

    const auto chars = snprintf(bufferPointer, bufferSize, "%x %x %x %x", odmOptcInputGlobalControl, otgMasterEn,
                                hubpClkControl, digBeEnControl);
    if (chars < 0) { return kIOReturnError; }
    const auto realChars = static_cast<UInt32>(chars) > bufferSize ? bufferSize : static_cast<UInt32>(chars);

    bufferSize    -= realChars;
    bufferPointer += realChars;

    return kIOReturnSuccess;
}

IOReturn X6000FB::getTriageHardwareDataRN(void*, const UInt32 fbIndex, void* const triageData)
{
    auto& bufferPointer = getMember<char*>(triageData, 0x0);
    auto& bufferSize    = getMember<UInt32>(triageData, 0x8);

    if (bufferSize < 2) { return kIOReturnNoResources; }
    if (fbIndex >= 4) { return kIOReturnSuccess; }

    const auto odmOptcInputGlobalControl = NRed::singleton().readReg32(DCN_BASE_2 + 0x1ACA + (0x10 * fbIndex));
    const auto otgMasterEn               = NRed::singleton().readReg32(DCN_BASE_2 + 0x1B5C + (0x80 * fbIndex));
    const auto hubpClkControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x5F4 + (0xDC * fbIndex));
    const auto digBeEnControl            = NRed::singleton().readReg32(DCN_BASE_2 + 0x20B0 + (0x100 * fbIndex));

    const auto chars = snprintf(bufferPointer, bufferSize, "%x %x %x %x", odmOptcInputGlobalControl, otgMasterEn,
                                hubpClkControl, digBeEnControl);
    if (chars < 0) { return kIOReturnError; }
    const auto realChars = static_cast<UInt32>(chars) > bufferSize ? bufferSize : static_cast<UInt32>(chars);

    bufferSize    -= realChars;
    bufferPointer += realChars;

    return kIOReturnSuccess;
}

UInt32 X6000FB::wrapControllerPowerUp(void* const self)
{
    if (singleton().cachedController == nullptr) { singleton().cachedController = self; }
    auto& m_flags  = getMember<UInt8>(self, 0x5F18);
    auto  send     = (m_flags & 2) == 0;
    m_flags       |= singleton().oledEnabled ? 0x0C : 4;    // All framebuffers enabled (+ hw attached for OLED)
    auto ret       = FunctionCast(wrapControllerPowerUp, singleton().orgControllerPowerUp)(self);
    if (send) { singleton().orgMessageAccelerator(self, IOFBRequestControllerEnabled, nullptr, nullptr, nullptr); }
    NRedAudio::singleton().onControllerPowerUp();
    return ret;
}

UInt32 X6000FB::wrapDoPowerStateChange(void* const self, const UInt32 state)
{
    const auto ret = FunctionCast(wrapDoPowerStateChange, singleton().orgDoPowerStateChange)(self, state);
    if (self == nullptr) { return ret; }

    static constexpr UInt32 fbPowerOn = 2;
    auto&                   inst      = singleton();
    bool                    repower   = false;

    if (inst.fbPowerLock != nullptr) { IOLockLock(inst.fbPowerLock); }
    FbPowerStateEntry* entry = nullptr;
    for (auto& item : inst.fbPowerStates) {
        if (item.fb == self) {
            entry = &item;
            break;
        }
        if (item.fb == nullptr && entry == nullptr) { entry = &item; }
    }
    if (entry != nullptr) {
        if (entry->fb == nullptr) { entry->fb = self; }
        if (state == fbPowerOn) {
            if (!entry->armed) { entry->armed = true; }
            else if (entry->lastState != fbPowerOn) { repower = true; }
        }
        entry->lastState = state;
    }
    if (inst.fbPowerLock != nullptr) { IOLockUnlock(inst.fbPowerLock); }

    if (repower) {
        void* const controller = inst.cachedController;
        // Only re-power if the controller is genuinely powered down (m_flags bit 1 clear).
        if (controller != nullptr && (getMember<UInt8>(controller, 0x5F18) & 2) == 0) {
            wrapControllerPowerUp(controller);
        }
    }

    return ret;
}

IOReturn X6000FB::wrapFbGetAttribute(void* const self, const UInt32 attribute, uintptr_t* const value)
{
    // 'vrrs' (0x76727273): Keep VRR shared timing struct enabled by default while providing -NRedNoVRR as
    // an emergency opt-out boot argument.
    static constexpr UInt32 kAttributeVRRS = 0x76727273U;
    if (attribute == kAttributeVRRS && !singleton().vrrEnabled) {
        if (value != nullptr) { *value = 0; }
        return kIOReturnUnsupported;
    }
    return FunctionCast(wrapFbGetAttribute, singleton().orgFbGetAttribute)(self, attribute, value);
}

IOReturn X6000FB::wrapPresenterHandleTransactions(void* const self, void* const transaction)
{
    // AmdFbPresenter::handleTransactions uses a single-shot zero-retry OSCompareAndSwap(0, 1, &this->c8)
    // spin-flag.  At 120Hz, when WindowServer's executeTransaction collides with the ~5-20us VBlank/VUpdate
    // interrupt handler holding this->c8, handleTransactions logs "[FB:x] ??? Busy" and returns
    // kIOReturnNotReady (0xE00002D8), causing IOAccelDisplayPipe transaction stamp 30 to time out.
    // Pre-wait on this->c8 (and keep the kIOReturnNotReady fallback loop) in 5us increments (up to 2ms)
    // until the concurrent interrupt releases this->c8, avoiding both timeout and "??? Busy" log spam.
    if (self != nullptr) {
        for (UInt32 wait = 0; __atomic_load_n(&getMember<UInt32>(self, 0xC8), __ATOMIC_ACQUIRE) != 0 && wait < 400;
             ++wait)
        {
            IODelay(5);
        }
    }
    auto ret = FunctionCast(wrapPresenterHandleTransactions, singleton().orgPresenterHandleTransactions)(
        self, transaction);
    for (UInt32 retry = 0; ret == kIOReturnNotReady && retry < 400; ++retry) {
        IODelay(5);
        ret = FunctionCast(wrapPresenterHandleTransactions, singleton().orgPresenterHandleTransactions)(
            self, transaction);
    }

    if (ret == kIOReturnSuccess && self != nullptr && singleton().vrrEnabled) {
        // Guard the shared 'vrrs' timing struct (presenter + 0x20..0x78) against two Apple bugs:
        // 1) During mode/mirror switches before the first VUpdate interrupt, prepareVRR() zeros +0x20/+0x48/+0x58,
        //    causing executeTransaction() to log "AMD ERROR! VRR FAILED TIMESTAMPS ... Next Frame: 0".
        // 2) On fixed-refresh commits (requestedHostTime at +0x210 == 0) or when VRR is inactive (+0xBC == 0),
        //    keep maxNextFrame (+0x30) == minNextFrame (+0x28) so executeTransaction() does not pre-fill a stale
        //    actual_host_time and instead lets processTransactionInterrupt() record mach_absolute_time().
        const UInt64 now               = mach_absolute_time();
        const UInt64 requestedHostTime = transaction != nullptr ? getMember<UInt64>(transaction, 0x210) : 0ULL;
        const bool   isVrrActive       = getMember<UInt8>(self, 0xBC) != 0;

        auto& lastFrame   = getMember<UInt64>(self, 0x20);
        auto& minNext     = getMember<UInt64>(self, 0x28);
        auto& maxNext     = getMember<UInt64>(self, 0x30);
        auto& nextFrame   = getMember<UInt64>(self, 0x48);
        auto& txComplete  = getMember<UInt64>(self, 0x58);

        if (lastFrame == 0) { lastFrame = now; }
        if (nextFrame == 0) { nextFrame = now; }
        if (txComplete == 0) {
            const UInt64 vBlankTime = getMember<UInt64>(self, 0xF0);
            UInt64       fallback   = vBlankTime;
            if (fallback == 0) {
                const UInt64 hLineTime = getMember<UInt64>(self, 0xD0);
                const UInt32 vTotalMin = getMember<UInt32>(self, 0x16BC);
                if (hLineTime != 0 && vTotalMin != 0) {
                    fallback = hLineTime * vTotalMin;
                }
                else {
                    nanoseconds_to_absolutetime(8333333ULL, &fallback);
                }
            }
            txComplete = nextFrame + fallback;
        }

        if (!isVrrActive || requestedHostTime == 0) {
            maxNext = minNext;
        }
        else if (txComplete < now) {
            txComplete = now;
        }
    }

    return ret;
}

UInt64 X6000FB::wrapPresenterGetOutstandingTransactions(void* const self)
{
    // AmdFbPresenter::getOutstandingTransactions loads 0xD8(%self) into %rbx before checking whether
    // stuckCounter (0xC0(%self)) reached 121 and calling handleStuckQueue().  When handleStuckQueue()
    // clears 0xD8(%self) to 0, the original function still returns the stale pre-clear %rbx value,
    // forcing an unnecessary extra timeout cycle.  Re-read 0xD8(%self) after the original call.
    const auto mask =
        FunctionCast(wrapPresenterGetOutstandingTransactions, singleton().orgPresenterGetOutstandingTransactions)(
            self);
    if (mask != 0 && self != nullptr) {
        return getMember<UInt64>(self, 0xD8);
    }
    return mask;
}

bool X6000FB::wrapControllerIsPoweredUp(void* const self)
{
    // Issue #451: AMDRadeonX6000_AmdRadeonControllerNavi::isPoweredUp() checks Navi10 dGPU BACOS
    // register 0x53 bit 9 and spins up to 10ms (1000 x 10us) polling Navi10 MP1 0x16061.
    // On Raven/Renoir APUs the GPU shares the SoC power domain; check m_flags bit 3 (0x8) and live MMIO.
    if (self == nullptr) { return false; }
    if ((getMember<UInt8>(self, 0x5F18) & 0x8U) == 0) { return false; }
    return NRed::singleton().readReg32(0x0) != 0xFFFFFFFFU;
}

IOReturn X6000FB::wrapGetDrrStatus(void*, const SInt32 otgInst, UInt32* const outDrrStatus)
{
    // Issue #449 / #441: Reimplement AmdRadeonControllerNavi::getDrrStatus() for Raven (DCN 1.0) and
    // Renoir (DCN 2.1): 4 OTG pipes, correct OTG_V_TOTAL_STATUS offset, and return kIOReturnSuccess (0)
    // instead of Apple's buggy unconditional 0xE0000001 return.
    if (otgInst < 0 || otgInst >= 4 || outDrrStatus == nullptr) { return kIOReturnError; }
    const UInt32 vTotalStatusOff = NRed::singleton().getAttributes().isRenoir() ? 0x1B57U : 0x1B5AU;
    const UInt32 regVal =
        NRed::singleton().readReg32(DCN_BASE_2 + vTotalStatusOff + (0x80U * static_cast<UInt32>(otgInst)));
    *outDrrStatus = (regVal >> 16) & 0x7FFFU;
    return kIOReturnSuccess;
}

IOReturn X6000FB::wrapGetVUpdateVBlankInterval(void*, const SInt32 otgInst, UInt32* const outInterval)
{
    // Issue #449 / #441: Reimplement AmdRadeonControllerNavi::getVUpdateVBlankInterval() for Raven/Renoir:
    // 4 OTG pipes and DCN 1.0/2.1 OTG_V_SYNC_A_CNTL offset (DCN_BASE_2 + 0x1B47) instead of Navi10's 0x5047.
    if (otgInst < 0 || otgInst >= 4 || outInterval == nullptr) { return kIOReturnError; }
    const UInt32 regVal =
        NRed::singleton().readReg32(DCN_BASE_2 + 0x1B47U + (0x80U * static_cast<UInt32>(otgInst)));
    *outInterval = regVal & 0x3FFU;
    return kIOReturnSuccess;
}

IOReturn X6000FB::wrapSetupBootWatermarks(void*)
{
    // Issue #450: Reimplement AmdRadeonControllerNavi::setupBootWatermarks() for Raven (DCN 1.0) and
    // Renoir (DCN 2.1) by propagating UEFI-initialized DCHUBBUB watermark Set A (0x39C9..0x39CD) to
    // Sets B/C/D (+5/+10/+15), avoiding Navi10/14 hardcoded values and 5-pipe HUBP writes.
    auto& nred = NRed::singleton();
    for (UInt32 reg = 0x39C9U; reg <= 0x39CDU; ++reg) {
        const UInt32 setA = nred.readReg32(reg);
        if (setA != 0 && setA != 0xFFFFFFFFU) {
            nred.writeReg32(reg + 5U, setA);
            nred.writeReg32(reg + 10U, setA);
            nred.writeReg32(reg + 15U, setA);
        }
    }
    return kIOReturnSuccess;
}

IOReturn X6000FB::wrapReportCapabilitiesLinkInfo(void* const self)
{
    const auto ret = FunctionCast(wrapReportCapabilitiesLinkInfo, singleton().orgReportCapabilitiesLinkInfo)(self);
    NRedAudio::singleton().onLinkCapabilitiesReported();
    return ret;
}

void X6000FB::wrapDpReceiverPowerCtrl(void* const link, const bool powerOn)
{
    FunctionCast(wrapDpReceiverPowerCtrl, singleton().orgDpReceiverPowerCtrl)(link, powerOn);
    if (powerOn) {
        IOSleep(250);
    }
}

void* X6000FB::wrapCreateObjectInfo(void* const helper, const UInt32 tableOffset)
{
    const auto ret = FunctionCast(wrapCreateObjectInfo, singleton().orgCreateObjectInfo)(helper, tableOffset);
    if (ret == nullptr) { return ret; }

    const auto infoTable = getMember<DispObjInfoTableV1*>(ret, 0x28);
    const auto n         = infoTable->pathCount;
    for (UInt8 i = 0, j = 0; i < n; i++) {
        // Skip invalid device tags
        if (infoTable->paths[i].devTag == 0) { infoTable->pathCount--; }
        else {
            infoTable->paths[j++] = infoTable->paths[i];
        }
    }

    return ret;
}

static UInt32 getTableOffset(const AmdAtomFwHelper* const biosHelper, const UInt32 index)
{
    const auto romTableOffset = static_cast<const UInt16*>(biosHelper->getImage(ATOM_ROM_TABLE_PTR, sizeof(UInt16)));
    if (romTableOffset == nullptr) { return 0; }
    const auto mdtOffset =
        static_cast<const UInt16*>(biosHelper->getImage(*romTableOffset + ATOM_ROM_DATA_PTR, sizeof(UInt32)));
    if (mdtOffset == nullptr) { return 0; }
    const auto mdt =
        static_cast<const UInt8*>(biosHelper->getImage(*mdtOffset, /*sizeof(atom_master_data_table_v2_1)*/ 0x4A));
    if (mdt == nullptr) { return 0; }
    return reinterpret_cast<const UInt16*>(mdt + sizeof(ATOMCommonTableHeader))[index];
}

AmdAtomVramInfo* X6000FB::wrapCreateVramInfo(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreateVramInfo, singleton().orgCreateVramInfo)(biosHelper, tableOffset);
    }
    return AmdAtomVramInfoIGP::createVramInfoIGP(biosHelper, getTableOffset(biosHelper, 0x1E));
}

IOReturn X6000FB::wrapPopulateVramInfo(AmdAtomVramInfo* const self, AtomFirmwareInfo& fwInfo)
{ return self->populateVramInfo(fwInfo); }

IOReturn X6000FB::wrapGetVendorInfo(const void* const self, AGDCVendorInfo_t* const vendorInfo,
                                    const size_t sizeofVendorInfo)
{
    const auto ret = FunctionCast(wrapGetVendorInfo, singleton().orgGetVendorInfo)(self, vendorInfo, sizeofVendorInfo);
    if (ret == kIOReturnSuccess) [[likely]] { vendorInfo->VendorClass = kAGDCVendorClassIntegratedGPU; }
    return ret;
}

// Hack
class AppleACPIPlatformExpert : IOACPIPlatformExpert
{
    friend class X6000FB;
};

size_t X6000FB::readVfctAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize, const bool strict)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto expert = static_cast<AppleACPIPlatformExpert*>(pciDevice->getPlatform());
    if (expert == nullptr) [[unlikely]] { return 0; }

    const auto vfctData = expert->getACPITableData("VFCT", 0);
    if (vfctData == nullptr) [[unlikely]] { return 0; }

    const auto vfct = static_cast<const VFCT*>(vfctData->getBytesNoCopy());
    if (vfct == nullptr) [[unlikely]] { return 0; }

    if (sizeof(VFCT) > vfctData->getLength()) [[unlikely]] { return 0; }

    const auto deviceID = pciDevice->extendedConfigRead16(kIOPCIConfigDeviceID);
    const auto vendor   = pciDevice->extendedConfigRead16(kIOPCIConfigVendorID);
    const auto busNum   = pciDevice->getBusNumber();
    const auto devNum   = pciDevice->getDeviceNumber();
    const auto devFunc  = pciDevice->getFunctionNumber();

    for (auto offset = vfct->vbiosImageOffset; offset < vfctData->getLength();) {
        auto vHdr =
            static_cast<const GOPVideoBIOSHeader*>(vfctData->getBytesNoCopy(offset, sizeof(GOPVideoBIOSHeader)));
        if (vHdr == nullptr) [[unlikely]] { return 0; }

        const auto vContent =
            static_cast<const UInt8*>(vfctData->getBytesNoCopy(offset + sizeof(GOPVideoBIOSHeader), vHdr->imageLength));
        if (vContent == nullptr) [[unlikely]] { return 0; }

        offset += sizeof(GOPVideoBIOSHeader) + vHdr->imageLength;

        if (vHdr->imageLength != 0 && vHdr->imageLength <= bufferSize
            && (!strict || (vHdr->pciBus == busNum && vHdr->pciDevice == devNum && vHdr->pciFunction == devFunc))
            && vHdr->vendorID == vendor && vHdr->deviceID == deviceID) [[likely]]
        {
            if (singleton().validateAtomBiosImage(self, const_cast<UInt8*>(vContent), vHdr->imageLength)) [[likely]] {
                memcpy(buffer, vContent, vHdr->imageLength);
                return vHdr->imageLength;
            }
            else {
                SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VFCT.");
            }
        }
        else {
            SYSLOG("NRed",
                   "VFCT image does not match, is empty or too long (pciBus: 0x%X pciDevice: 0x%X pciFunction: 0x%X "
                   "vendorID: 0x%X deviceID: 0x%X imageLength: 0x%X).",
                   vHdr->pciBus, vHdr->pciDevice, vHdr->pciFunction, vHdr->vendorID, vHdr->deviceID, vHdr->imageLength);
        }
    }

    SYSLOG("NRed", "VFCT table present but broken.");
    return 0;
}

size_t X6000FB::readVramAtomBiosImage(void* const self, UInt8* const buffer, const size_t bufferSize)
{
    const auto pciDevice = getMember<IOPCIDevice*>(self, 0x28);

    const auto bar0 =
        pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapWriteCombineCache | kIOMapAnywhere);
    if (bar0 == nullptr) [[unlikely]] { return 0; }

    if (bar0->getLength() == 0) [[unlikely]] {
        bar0->release();
        return 0;
    }

    const auto fb = reinterpret_cast<UInt8*>(bar0->getVirtualAddress());

    if (singleton().validateAtomBiosImage(self, fb, bufferSize)) [[likely]] {
        memcpy(buffer, fb, bufferSize);
        bar0->release();
        return bufferSize;
    }

    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from VRAM.");
    bar0->release();
    return 0;
}

// TODO: See `amdgpu_device_need_post`, `amdgpu_get_bios_dgpu`, `amdgpu_get_bios_apu`.
IOReturn X6000FB::readAtomBios(void* const self)
{
    auto& biosImage = getMember<UInt8[0x10000]>(self, 0x48);
    auto  size      = singleton().readEfiAtomBiosImage(self, biosImage, sizeof(biosImage));
    if (size == 0) [[likely]] {
        size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage));
        if (size == 0) [[unlikely]] {
            size = readVramAtomBiosImage(self, biosImage, sizeof(biosImage));
            if (size == 0) [[unlikely]] {
                size = singleton().readPciAtomBiosImage(self, biosImage, sizeof(biosImage));
                if (size == 0) [[likely]] {
                    size = readVfctAtomBiosImage(self, biosImage, sizeof(biosImage), false);
                    if (size == 0) [[unlikely]] { return kIOReturnInternalError; }
                }
                else if (!singleton().validateAtomBiosImage(self, biosImage, sizeof(biosImage))) [[unlikely]] {
                    SYSLOG("X6000FB", "BIOS Validation Failed - Reading from PCI Device.");
                    return kIOReturnInternalError;
                }
            }
        }
    }
    getMember<size_t>(self, 0x10048) = size;
    return kIOReturnSuccess;
}

AmdAtomPspDirectory* X6000FB::wrapCreatePspDirectory(AmdAtomFwHelper* const biosHelper, const UInt32 tableOffset)
{
    if (biosHelper == nullptr || tableOffset != 0) {
        return FunctionCast(wrapCreatePspDirectory, singleton().orgCreatePspDirectory)(biosHelper, tableOffset);
    }
    return AmdAtomPspDirectoryDummy::create();
}
