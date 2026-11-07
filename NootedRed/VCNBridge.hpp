// VCN2 Hardware Decode Bridge
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

#pragma once
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOService.h>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSMetaClass.h>

// Bridges Apple's native AMDRadeonX6000 VCN2 engine into the active AMDRadeonX5000 accelerator.
//
// The X5000 accelerator keeps driving the APU.  When the user opts in with -NRedVDA, this module:
//  - force-loads com.apple.kext.AMDRadeonX6000 as a symbol donor from inside the X5000 accelerator start;
//  - verifies the generated per-build vtable/getter ABI on both drivers before touching anything;
//  - installs the generated cross-driver call-site patches in the X6000 text with rollback;
//  - allocates AMDVCN2HWEngine and places it in X5000's PM4 engine array at kAMDHWEngineTypeVCN0;
//  - bridges the X5000/X6000 metaclasses and video/shared/SML entry points.
//
// Every failure path clears readiness so the accelerator stays in its normal graphics-only state.  The
// module never panics and does nothing unless -NRedVDA is present.
class VCNBridge
{
    struct ClassPair
    {
        OSMetaClass* oldClass{nullptr};
        OSMetaClass* newClass{nullptr};
    };

    bool requested{false};
    bool x5000Prepared{false};
    bool x6000Prepared{false};
    bool loadAttempted{false};
    bool loadCompleted{false};
    bool engineAllocated{false};

    mach_vm_address_t orgAcceleratorStart{0};
    mach_vm_address_t orgAcceleratorStop{0};
    mach_vm_address_t orgNewVideo{0};
    mach_vm_address_t orgNewShared{0};
    mach_vm_address_t orgNewClient{0};
    mach_vm_address_t orgCreateSML{0};
    mach_vm_address_t orgSignalWork{0};
    mach_vm_address_t orgClientStart{0};
    mach_vm_address_t orgClientStop{0};
    mach_vm_address_t orgSurfaceCopy{0};
    mach_vm_address_t orgInitHardware{0};
    mach_vm_address_t orgSafeMetaCast{0};

    mach_vm_address_t newVideo6000{0};
    mach_vm_address_t createSML6000{0};
    mach_vm_address_t clientStart5000{0};
    mach_vm_address_t clientStop5000{0};
    mach_vm_address_t surfaceCopy5000{0};
    mach_vm_address_t notifyGfxAccess{0};

    OSMetaClass* engineMC{nullptr};
    OSMetaClass* sharedMC{nullptr};
    OSMetaClass* clientMC{nullptr};
    void*        hardwareHandle{nullptr};
    static constexpr size_t kClassPairCount = 6;
    ClassPair              classPairs[kClassPairCount];

public:
    static VCNBridge& singleton();

    // Parses -NRedVDA and registers the X6000 kext-load hook.  Called from NRed::init().
    void init();

    // Handles X5000 and X6000 kext loads.  Best-effort: a failure disables the bridge without panicking.
    void processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);

    // True when the user asked for VDA and the device is one we have generated data for.
    bool isRequested() const { return this->requested; }

    // Both drivers prepared and the optional X6000 load completed.
    bool loadReady() const;

    // Native VCN2 engine allocated successfully in the X5000 engine array.  This is also true during the
    // original accelerator start (engine allocation happens before start() returns), which is required by
    // the video/shared entry points that run inside that start.
    bool engineEnabled() const { return this->loadReady() && this->engineAllocated; }

    // Allocates the X6000 VCN2 engine for X5000::allocateHWEngines().  `hardware` is the X5000
    // AMDHardware object whose capability field is cleared again if the engine later fails to initialise.
    void* allocateEngine(void* hardware);

private:
    static bool          acceleratorStart(void* self, IOService* provider);
    static void          acceleratorStop(void* self, IOService* provider);
    static void*         newShared();
    static void*         newClient();
    static void*         newVideo(void* self);
    static void*         createSML(UInt32 configuration);
    static bool          clientStart(void* self, void* provider);
    static void          clientStop(void* self, void* provider);
    static UInt64        surfaceCopy(void* self, void* input, UInt64 size, void* event);
    static void          signalWork(void* hardware);
    static bool          initHardware(void* self, UInt32 channel);
    static OSMetaClassBase* safeMetaCast(const OSMetaClassBase* object, const OSMetaClass* target);
};
