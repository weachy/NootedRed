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

#include <DeviceProfile.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <VCNBridge.hpp>
#include <VCNKernelData.hpp>
#include <VCNKernelDataVentura.hpp>
#if __has_include(<VCNKernelDataTahoe.hpp>)
#include <VCNKernelDataTahoe.hpp>
#define NRED_HAS_VCN_TAHOE 1
#endif
#include <VDA.hpp>
#include <X5000.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <libkern/OSAtomic.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>

// libkern/version.h declares this with C linkage; including the header conflicts with the version
// constants already declared by KernelVersion.hpp, so declare only the build string we need.
extern "C" char osversion[];

static VCNBridge moduleInstance;

namespace
{

    const char* const kBootArgNoVDA = "-NRedNoVDA";


    // VCN bridge supports macOS 13 through 26 (Darwin 22 - 25).
    // Version-specific ABI stability is guarded dynamically by verifyKernelABI() vtable checks.
    bool isSupportedOS()
    {
        const auto major = currentKernelVersion().major();
        return (major >= 22 && major <= 25);
    }

    // The build string is checked once the accelerator starts, not in NRed::init: the early kext-load
    // phase can run before `osversion` is populated, which would otherwise disable a valid bridge.
    // Major release prefix check ensures sanity while allowing all minor updates (e.g. 14.0-14.8, 15.0-15.8).
    bool isSupportedBuild()
    {
        if (!isSupportedOS()) { return false; }
        if (osversion[0] == '\0') { return true; }
        const auto major = currentKernelVersion().major();
        if (major == 22) {
            return strncmp(osversion, "22", 2) == 0;
        } else if (major == 23) {
            return strncmp(osversion, "23", 2) == 0;
        } else if (major == 24) {
            return strncmp(osversion, "24", 2) == 0;
        }
#ifdef NRED_HAS_VCN_TAHOE
        else if (major == 25) {
            return strncmp(osversion, "25", 2) == 0;
        }
#endif
        return false;
    }

    // True when the Tahoe (Darwin 25) generated tables must be used instead of the legacy ones.
    bool useTahoeVCNData()
    {
#ifdef NRED_HAS_VCN_TAHOE
        return currentKernelVersion().major() == 25;
#else
        return false;
#endif
    }

    bool isSupportedDevice()
    {
        // The profile is the single source of truth for the target: a VCN 2.x device whose profile enables
        // the engine (0x1638 is validated; the other VCN 2.x APUs are experimental) may use this bridge.  The
        // generated ABI guards and the exact build check remain the hard safety gates.
        const auto* const profile = DeviceProfile::current();
        return isSupportedBuild() && !NRed::singleton().hasExternalVideo() && profile != nullptr
               && profile->media.vcnGeneration == DeviceProfile::kVcn2_2 && profile->media.hasVCN0;
    }

    // Boot diagnostics: the iGPU is guaranteed to exist once the device ID is known, so properties can be
    // published from the kext-load/accelerator stages.  Record the furthest stage reached and a rejection
    // reason, which makes a failed boot diagnosable from ioreg without the unified log.
    void publishStage(const UInt32 stage)
    {
        static volatile UInt32 lastStage{0};
        if (NRed::singleton().getDeviceID() == 0) { return; }

        // The kext-load/start callbacks can run on different threads; publish only a monotonic maximum.
        UInt32 current;
        do {
            current = lastStage;
            if (stage <= current) { return; }
        } while (!OSCompareAndSwap(current, stage, &lastStage));

        NRed::singleton().setProp32("NRedVCNStage", stage);
    }

    void publishReject(const UInt32 reason)
    {
        if (NRed::singleton().getDeviceID() != 0) {
            NRed::singleton().setProp32("NRedVCNReject", reason);
        }
    }

    template<typename T>
    bool resolve(KernelPatcher& patcher, const size_t id, const char* const name, T& target,
                 const mach_vm_address_t slide, const size_t size)
    {
        target = reinterpret_cast<T>(patcher.solveSymbol(id, name, slide, size));
        if (target) { return true; }

        patcher.clearError();
        SYSLOG("VCN", "missing symbol: %s", name);
        return false;
    }

    bool verifyKernelABI(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size,
                         const bool old)
    {
        if (size < 16 || slide > ~mach_vm_address_t{0} - size) { return false; }

        const VCNKernelData::SlotGuard* guards{nullptr};
        const char*                     tableSymbol{nullptr};
        size_t                          count{0};
        if (currentKernelVersion().major() == 22) {
            guards      = old ? VCNKernelDataVentura::Guards5000 : VCNKernelDataVentura::Guards6000;
            count       = old ? arrsize(VCNKernelDataVentura::Guards5000) : arrsize(VCNKernelDataVentura::Guards6000);
            tableSymbol = old ? VCNKernelDataVentura::Table5000 : VCNKernelDataVentura::Table6000;
        } else
#ifdef NRED_HAS_VCN_TAHOE
        if (useTahoeVCNData()) {
            guards      = old ? VCNKernelDataTahoe::Guards5000 : VCNKernelDataTahoe::Guards6000;
            count       = old ? arrsize(VCNKernelDataTahoe::Guards5000) : arrsize(VCNKernelDataTahoe::Guards6000);
            tableSymbol = old ? VCNKernelDataTahoe::Table5000 : VCNKernelDataTahoe::Table6000;
        } else
#endif
        {
            guards      = old ? VCNKernelData::Guards5000 : VCNKernelData::Guards6000;
            count       = old ? arrsize(VCNKernelData::Guards5000) : arrsize(VCNKernelData::Guards6000);
            tableSymbol = old ? VCNKernelData::Table5000 : VCNKernelData::Table6000;
        }

        mach_vm_address_t table{0};
        if (!resolve(patcher, id, tableSymbol, table, slide, size)) {
            return false;
        }

        for (size_t i = 0; i < count; ++i) {
            const auto& guard  = guards[i];
            const auto  offset = 2 * sizeof(void*) + guard.slot;
            if (offset > size - sizeof(void*) || table < slide || table - slide > size - sizeof(void*) - offset) {
                SYSLOG("VCN", "%s vtable guard %zu is out of bounds", old ? "X5000" : "X6000", i);
                return false;
            }

            mach_vm_address_t target{0};
            memcpy(&target, reinterpret_cast<void*>(table + offset), sizeof(target));
            if (target < slide || target - slide >= size || guard.getterSize > size - (target - slide)) {
                SYSLOG("VCN", "%s vtable guard %zu target is out of bounds", old ? "X5000" : "X6000", i);
                return false;
            }

            if (guard.methodSymbol != nullptr) {
                mach_vm_address_t expected{0};
                if (!resolve(patcher, id, guard.methodSymbol, expected, slide, size) || target != expected) {
                    SYSLOG("VCN", "%s vtable guard %zu method mismatch", old ? "X5000" : "X6000", i);
                    return false;
                }
            }
            else if (guard.getter == nullptr || guard.getterSize == 0
                     || memcmp(reinterpret_cast<void*>(target), guard.getter, guard.getterSize) != 0)
            {
                SYSLOG("VCN", "%s vtable guard %zu getter mismatch", old ? "X5000" : "X6000", i);
                return false;
            }
        }

        return true;
    }


}    // namespace

VCNBridge& VCNBridge::singleton() { return moduleInstance; }

void VCNBridge::init()
{
    this->requested = !checkKernelArgument(kBootArgNoVDA);

    if (!this->requested) {
        DBGLOG("VCN", "VCN bridge is disabled by -NRedNoVDA");
        return;
    }

    if (!isSupportedOS()) {
        SYSLOG("VCN", "VCN bridge only supports Darwin 22 to 25 (macOS 13 to 26); hardware decode ignored on this build");
        this->requested = false;
        return;
    }

    // Register the optional X6000 load callback up front.  The kext is only requested from inside the
    // X5000 accelerator start, i.e. after both drivers' load hooks are in place.
    lilu.onKextLoadForce(&kextRadeonX6000);

    DBGLOG("VCN", "VCN bridge requested; waiting for device and driver load");
}

bool VCNBridge::loadReady() const
{
    return this->requested && this->x5000Prepared && this->x6000Prepared && this->loadCompleted;
}

void* VCNBridge::allocateEngine(void* const hardware)
{
    if (!this->requested) { return nullptr; }

    if (!this->loadReady() || this->engineMC == nullptr) {
        NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
        return nullptr;
    }

    this->hardwareHandle = hardware;
    auto* const engine   = this->engineMC->alloc();
    this->engineAllocated = engine != nullptr;

    NRed::singleton().setProp32("NRedVCNEngineAllocated", static_cast<UInt32>(this->engineAllocated));
    NRed::singleton().setProp32("NRedVDAMediaEnabled", static_cast<UInt32>(this->engineAllocated));
    if (!this->engineAllocated) {
        publishReject(7);
        SYSLOG("VCN", "Failed to allocate AMDVCN2HWEngine");
        this->loadCompleted = false;
    }
    else {
        publishStage(9);
    }

    return engine;
}

bool VCNBridge::acceleratorStart(void* const self, IOService* const provider)
{
    auto& bridge = singleton();
    if (bridge.requested) {

        if (currentKernelVersion().major() == 22) {
            NRed::singleton().setProp32("NRedVenturaVDARevision", 1);
        }
        NRed::singleton().setProp32("NRedVCNAccelStartSeen", 1);
        publishStage(5);
    }

    // A partially prepared X5000 must never force-load X6000 or install any hook: the normal accelerator
    // start path is enough.  The device/profile check is done here, after NRed has set the device ID.
    if (!bridge.requested || !bridge.x5000Prepared || !isSupportedDevice()) {
        if (bridge.requested) { publishReject(2); }
        return FunctionCast(acceleratorStart, bridge.orgAcceleratorStart)(self, provider);
    }

    // Only the APU that NRed actually drives may trigger the bridge.  If the provider is not a PCI device we
    // still trust NRed's own device detection; the kext is only loaded for the iGPU.
    auto* const pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci != nullptr) {
        const auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
        const auto device = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID);
        if (vendor != WIOKit::VendorID::ATIAMD || device != NRed::singleton().getDeviceID()) {
            SYSLOG("VCN", "accelerator start provider mismatch (0x%X:0x%X), not bridging", vendor, device);
            return FunctionCast(acceleratorStart, bridge.orgAcceleratorStart)(self, provider);
        }
    }

    if (!bridge.loadAttempted) {
        bridge.loadAttempted = true;
        publishStage(6);
        const auto status   = OSKextLoadKextWithIdentifier("com.apple.kext.AMDRadeonX6000");
        bridge.loadCompleted = status == kOSReturnSuccess && bridge.x5000Prepared && bridge.x6000Prepared;
        NRed::singleton().setProp32("NRedVCNNativeLoadStatus", static_cast<UInt32>(status));
        NRed::singleton().setProp32("NRedVCNNativeLoadReady", static_cast<UInt32>(bridge.loadCompleted));
        if (!bridge.loadCompleted) { publishReject(6); }
        SYSLOG("VCN", "X6000 native load status=0x%X prepared5=%u prepared6=%u", status, bridge.x5000Prepared,
               bridge.x6000Prepared);
    }

    const auto result = FunctionCast(acceleratorStart, bridge.orgAcceleratorStart)(self, provider);
    if (result) { publishStage(10); }
    NRed::singleton().setProp32("NRedVCNAcceleratorStarted", static_cast<UInt32>(result));



    if (!result) {
        bridge.loadCompleted   = false;
        bridge.engineAllocated = false;
        NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
        NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
        if (bridge.hardwareHandle != nullptr) {
            X5000::singleton().setVCN0Capability(bridge.hardwareHandle, false);
            bridge.hardwareHandle = nullptr;
        }
    }

    return result;
}

void VCNBridge::acceleratorStop(void* const self, IOService* const provider)
{
    auto& bridge = singleton();



    FunctionCast(acceleratorStop, bridge.orgAcceleratorStop)(self, provider);

    // The accelerator is going away; drop all engine/accelerator state without touching the hardware
    // object.  The prepared X5000/X6000 symbols stay valid for a possible restart, and a new start will
    // allocate a fresh engine.
    bridge.engineAllocated = false;
    bridge.hardwareHandle  = nullptr;

    NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
    NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
}

void* VCNBridge::newShared()
{
    auto& bridge = singleton();
    if (bridge.engineEnabled() && bridge.sharedMC != nullptr) { return bridge.sharedMC->alloc(); }

    return FunctionCast(newShared, bridge.orgNewShared)();
}

void* VCNBridge::newClient()
{
    auto& bridge = singleton();
    if (bridge.engineEnabled() && bridge.clientMC != nullptr) { return bridge.clientMC->alloc(); }

    return FunctionCast(newClient, bridge.orgNewClient)();
}

void* VCNBridge::newVideo(void* const self)
{
    auto& bridge = singleton();
    if (bridge.engineEnabled() && bridge.newVideo6000 != 0) {
        return reinterpret_cast<void* (*)(void*)>(bridge.newVideo6000)(self);
    }

    return FunctionCast(newVideo, bridge.orgNewVideo)(self);
}

void* VCNBridge::createSML(const UInt32 configuration)
{
    auto& bridge = singleton();

    // The generic configuration selects the VCN-capable SML; the GFX9-specific configuration keeps the
    // native X5000 SML.  The low bit only selects the generic constructor and is not forwarded.
    if (bridge.loadReady() && bridge.createSML6000 != 0 && (configuration & 0xFFF8FFFFU) != 0) {
        return reinterpret_cast<void* (*)(UInt32)>(bridge.createSML6000)(configuration | 1U);
    }

    return FunctionCast(createSML, bridge.orgCreateSML)(configuration);
}

bool VCNBridge::clientStart(void* const self, void* const provider)
{
    auto& bridge = singleton();
    const auto address = bridge.engineEnabled() ? bridge.clientStart5000 : bridge.orgClientStart;
    return reinterpret_cast<bool (*)(void*, void*)>(address)(self, provider);
}

void VCNBridge::clientStop(void* const self, void* const provider)
{
    auto& bridge = singleton();



    const auto address = bridge.engineEnabled() ? bridge.clientStop5000 : bridge.orgClientStop;
    reinterpret_cast<void (*)(void*, void*)>(address)(self, provider);
}

UInt64 VCNBridge::surfaceCopy(void* const self, void* const input, const UInt64 size, void* const event)
{
    auto& bridge = singleton();
    const auto address = bridge.engineEnabled() ? bridge.surfaceCopy5000 : bridge.orgSurfaceCopy;
    return reinterpret_cast<UInt64 (*)(void*, void*, UInt64, void*)>(address)(self, input, size, event);
}


void VCNBridge::signalWork(void* const hardware)
{
    auto& bridge = singleton();
    if (bridge.engineEnabled() && bridge.notifyGfxAccess != 0) {
        reinterpret_cast<void (*)(void*)>(bridge.notifyGfxAccess)(hardware);
        return;
    }

    FunctionCast(signalWork, bridge.orgSignalWork)(hardware);
}

bool VCNBridge::initHardware(void* const self, const UInt32 channel)
{
    auto& bridge = singleton();
    const auto result = FunctionCast(initHardware, bridge.orgInitHardware)(self, channel);
    NRed::singleton().setProp32("NRedVCNLastHardwareInit", static_cast<UInt32>(result));

    if (result) {
        publishStage(11);
        SYSLOG("VCN", "VCN2 initHardware channel=%u succeeded", channel);
        return true;
    }
    publishStage(12);
    publishReject(8);

    // The engine did not come up.  Stop advertising VCN0 and gate future user-space patches on the failed
    // bridge; already patched decoder processes keep the patched VA/GVA code until they exit, which is the
    // best that can be done without a reboot.
    SYSLOG("VCN", "VCN2 initHardware channel=%u failed; disabling the VCN bridge", channel);
    bridge.engineAllocated = false;
    bridge.loadCompleted   = false;
    NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
    NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
    if (bridge.hardwareHandle != nullptr) {
        X5000::singleton().setVCN0Capability(bridge.hardwareHandle, false);
    }

    return false;
}

OSMetaClassBase* VCNBridge::safeMetaCast(const OSMetaClassBase* const object, const OSMetaClass* const target)
{
    auto& bridge = singleton();
    if (target == nullptr) { return nullptr; }

    auto  result =
        FunctionCast(safeMetaCast, bridge.orgSafeMetaCast)(object, target);
    if (result != nullptr || object == nullptr || !bridge.engineEnabled()) { return result; }

    for (const auto& pair : bridge.classPairs) {
        if (pair.oldClass == nullptr || pair.newClass == nullptr) { continue; }

        const auto* const alternative = target == pair.oldClass ? pair.newClass :
                                        target == pair.newClass ? pair.oldClass :
                                                                  nullptr;
        if (alternative == nullptr) { continue; }

        result = FunctionCast(safeMetaCast, bridge.orgSafeMetaCast)(object, alternative);
        if (result != nullptr && result->getMetaClass()->getClassSize() >= target->getClassSize()) { return result; }
    }

    return nullptr;
}

void VCNBridge::processKext(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide,
                            const size_t size)
{
    if (!this->requested) { return; }

    if (id == kextRadeonX5000.loadIndex) {
        NRed::singleton().setProp32("NRedVCNX5000Seen", 1);
        publishStage(2);

        if (!verifyKernelABI(patcher, id, slide, size, true)) {
            this->x5000Prepared = false;
            NRed::singleton().setProp32("NRedVCNX5000ABIValidated", 0);
            publishReject(4);
            SYSLOG("VCN", "X5000 ABI mismatch; VCN bridge disabled");
            return;
        }
        publishStage(3);

        auto ok = true;
        ok &= resolve(patcher, id, "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator4stopEP9IOService",
                      this->orgAcceleratorStop, slide, size);
        ok &= resolve(patcher, id, "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator9newSharedEv", this->orgNewShared,
                      slide, size);
        ok &= resolve(patcher, id, "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator19newSharedUserClientEv",
                      this->orgNewClient, slide, size);
        ok &= resolve(patcher, id, "__ZN41AMDRadeonX5000_AMDGFX9GraphicsAccelerator15newVideoContextEv",
                      this->orgNewVideo, slide, size);
        ok &= resolve(patcher, id, "__ZN31AMDRadeonX5000_IAMDSMLInterface18createSMLInterfaceEj",
                      this->orgCreateSML, slide, size);
        ok &= resolve(patcher, id, "__ZN39AMDRadeonX5000_AMDAccelSharedUserClient5startEP9IOService",
                      this->clientStart5000, slide, size);
        ok &= resolve(patcher, id, "__ZN39AMDRadeonX5000_AMDAccelSharedUserClient4stopEP9IOService",
                      this->clientStop5000, slide, size);
        ok &= resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDAccelShared11SurfaceCopyEPjyP12IOAccelEvent",
                      this->surfaceCopy5000, slide, size);
        ok &= resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv", this->notifyGfxAccess,
                      slide, size);
        if (!ok) {
            publishReject(9);
            SYSLOG("VCN", "failed to resolve X5000 bridge symbols");
            this->x5000Prepared = false;
            return;
        }

        static const char* const kPairs[] = {
            "__ZN37AMDRadeonX5000_AMDGraphicsAccelerator10gMetaClassE",
            "__ZN35AMDRadeonX5000_AMDAccelVideoContext10gMetaClassE",
            "__ZN30AMDRadeonX5000_AMDAccelChannel10gMetaClassE",
            "__ZN28AMDRadeonX5000_IAMDHWChannel10gMetaClassE",
            "__ZN29AMDRadeonX5000_AMDAccelShared10gMetaClassE",
            "__ZN39AMDRadeonX5000_AMDAccelSharedUserClient10gMetaClassE",
        };
        static_assert(arrsize(kPairs) == kClassPairCount, "Update kClassPairCount with the class pair table");
        for (size_t i = 0; i < arrsize(kPairs); ++i) {
            ok &= resolve(patcher, id, kPairs[i], this->classPairs[i].oldClass, slide, size);
        }
        if (!ok) {
            publishReject(10);
            SYSLOG("VCN", "failed to resolve X5000 metaclass pairs");
            this->x5000Prepared = false;
            return;
        }

        KernelPatcher::RouteRequest routes[] = {
            {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5startEP9IOService", acceleratorStart,
             this->orgAcceleratorStart},
            {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator4stopEP9IOService", acceleratorStop,
             this->orgAcceleratorStop},
            {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator9newSharedEv", newShared, this->orgNewShared},
            {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator19newSharedUserClientEv", newClient, this->orgNewClient},
            {"__ZN41AMDRadeonX5000_AMDGFX9GraphicsAccelerator15newVideoContextEv", newVideo, this->orgNewVideo},
            {"__ZN31AMDRadeonX5000_IAMDSMLInterface18createSMLInterfaceEj", createSML, this->orgCreateSML},
        };
        if (!patcher.routeMultiple(id, routes, arrsize(routes), slide, size)) {
            patcher.clearError();
            publishReject(11);
            SYSLOG("VCN", "Failed to route X5000 VCN entry points");
            this->x5000Prepared = false;
            return;
        }

        KernelPatcher::RouteRequest castRoute{"__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass", safeMetaCast,
                                              this->orgSafeMetaCast};
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &castRoute, 1)) {
            patcher.clearError();
            publishReject(12);
            SYSLOG("VCN", "Failed to route safeMetaCast");
            this->x5000Prepared = false;
            return;
        }

        this->x5000Prepared = true;
        NRed::singleton().setProp32("NRedVCNX5000ABIValidated", 1);
        publishStage(4);
        SYSLOG("VCN", "X5000 VCN entry points prepared");
    }
    else if (id == kextRadeonX6000.loadIndex) {
        NRed::singleton().setProp32("NRedVCNX6000Seen", 1);
        publishStage(7);

        // Only patch X6000 after we requested the load from the X5000 accelerator start and the X5000 side
        // is fully prepared.  A preloaded X6000 (for a dGPU) must be left alone.
        if (!this->loadAttempted || !this->x5000Prepared || !isSupportedDevice()) {
            publishReject(3);
            SYSLOG("VCN", "ignoring X6000 load that was not requested by the VCN bridge");
            return;
        }

        if (!verifyKernelABI(patcher, id, slide, size, false)) {
            this->x6000Prepared = false;
            NRed::singleton().setProp32("NRedVCNX6000ABIValidated", 0);
            publishReject(5);
            SYSLOG("VCN", "X6000 ABI mismatch; VCN bridge disabled");
            return;
        }
        publishStage(8);

        auto ok = true;
        ok &= resolve(patcher, id, "__ZN30AMDRadeonX6000_AMDVCN2HWEngine10gMetaClassE", this->engineMC, slide, size);
        ok &= resolve(patcher, id, "__ZN29AMDRadeonX6000_AMDAccelShared10gMetaClassE", this->sharedMC, slide, size);
        ok &= resolve(patcher, id, "__ZN39AMDRadeonX6000_AMDAccelSharedUserClient10gMetaClassE", this->clientMC,
                      slide, size);
        ok &= resolve(patcher, id, "__ZN42AMDRadeonX6000_AMDGFX10GraphicsAccelerator15newVideoContextEv",
                      this->newVideo6000, slide, size);
        ok &= resolve(patcher, id, "__ZN31AMDRadeonX6000_IAMDSMLInterface18createSMLInterfaceEj",
                      this->createSML6000, slide, size);


        static const char* const kPairs[] = {
            "__ZN37AMDRadeonX6000_AMDGraphicsAccelerator10gMetaClassE",
            "__ZN35AMDRadeonX6000_AMDAccelVideoContext10gMetaClassE",
            "__ZN30AMDRadeonX6000_AMDAccelChannel10gMetaClassE",
            "__ZN28AMDRadeonX6000_IAMDHWChannel10gMetaClassE",
            "__ZN29AMDRadeonX6000_AMDAccelShared10gMetaClassE",
            "__ZN39AMDRadeonX6000_AMDAccelSharedUserClient10gMetaClassE",
        };
        static_assert(arrsize(kPairs) == kClassPairCount, "Update kClassPairCount with the class pair table");
        for (size_t i = 0; i < arrsize(kPairs); ++i) {
            ok &= resolve(patcher, id, kPairs[i], this->classPairs[i].newClass, slide, size);
        }
        if (!ok) {
            this->x6000Prepared = false;
            return;
        }

        // Verify every generated call-site before touching any of them, then apply all-or-nothing.
        size_t                           callPatchCount = 0;
        const VCNKernelData::CallPatch*  callPatches{nullptr};
        const char*                      signalSymbol{nullptr};
        if (currentKernelVersion().major() == 22) {
            callPatches  = VCNKernelDataVentura::getCallPatches(callPatchCount);
            signalSymbol = VCNKernelDataVentura::SignalSymbol;
        } else
#ifdef NRED_HAS_VCN_TAHOE
        if (useTahoeVCNData()) {
            callPatches  = VCNKernelDataTahoe::getCallPatches(callPatchCount);
            signalSymbol = VCNKernelDataTahoe::SignalSymbol;
        } else
#endif
        {
            callPatches  = VCNKernelData::getCallPatches(callPatchCount);
            signalSymbol = VCNKernelData::SignalSymbol;
        }
        static constexpr size_t kMaxCallPatches = 32;
        static_assert(arrsize(VCNKernelDataVentura::CallPatches) <= kMaxCallPatches);
        static_assert(arrsize(VCNKernelData::CallPatches_Sonoma) <= kMaxCallPatches);
        static_assert(arrsize(VCNKernelData::CallPatches_Sequoia) <= kMaxCallPatches);
#ifdef NRED_HAS_VCN_TAHOE
        static_assert(arrsize(VCNKernelDataTahoe::CallPatches) <= kMaxCallPatches);
#endif
        if (callPatchCount == 0 || callPatchCount > kMaxCallPatches) {
            SYSLOG("VCN", "unexpected generated call patch count %zu", callPatchCount);
            return;
        }
        mach_vm_address_t addresses[kMaxCallPatches]{};
        mach_vm_address_t signal{0};
        if (!resolve(patcher, id, signalSymbol, signal, slide, size)) { return; }

        for (size_t i = 0; i < callPatchCount; ++i) {
            const auto&       patch = callPatches[i];
            mach_vm_address_t function{0};
            if (!resolve(patcher, id, patch.symbol, function, slide, size)) { return; }

            addresses[i] = function + patch.offset;
            if (function > ~mach_vm_address_t{0} - patch.offset || addresses[i] < slide
                || addresses[i] > slide + size - sizeof(patch.original)
                || memcmp(reinterpret_cast<void*>(addresses[i]), patch.original, sizeof(patch.original)) != 0)
            {
                SYSLOG("VCN", "kernel code mismatch: %s+0x%X", patch.symbol, patch.offset);
                return;
            }
        }

        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return; }

        for (size_t i = 0; i < callPatchCount; ++i) {
            const auto& patch = callPatches[i];
            UInt8       replacement[6];
            memcpy(replacement, patch.original, sizeof(replacement));
            if (patch.newSlot != 0) { memcpy(replacement + 2, &patch.newSlot, sizeof(patch.newSlot)); }
            else {
                const auto displacement = static_cast<SInt32>(signal - addresses[i] - 5);
                replacement[0]          = 0xE8;
                memcpy(replacement + 1, &displacement, sizeof(displacement));
                replacement[5] = 0x90;
            }
            memcpy(reinterpret_cast<void*>(addresses[i]), replacement, sizeof(replacement));
        }

        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

        KernelPatcher::RouteRequest routes[] = {
            {"__ZN26AMDRadeonX6000_AMDHardware22signalGPUWorkSubmittedEv", signalWork, this->orgSignalWork},
            {"__ZN39AMDRadeonX6000_AMDAccelSharedUserClient5startEP9IOService", clientStart, this->orgClientStart},
            {"__ZN39AMDRadeonX6000_AMDAccelSharedUserClient4stopEP9IOService", clientStop, this->orgClientStop},
            {"__ZN29AMDRadeonX6000_AMDAccelShared11SurfaceCopyEPjyP12IOAccelEvent", surfaceCopy, this->orgSurfaceCopy},
            {"__ZN30AMDRadeonX6000_AMDVCN2HWEngine12initHardwareE21eAMDAccelVideoChannel", initHardware,
             this->orgInitHardware},
        };
        if (!patcher.routeMultiple(id, routes, arrsize(routes), slide, size)) {
            patcher.clearError();
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                for (size_t i = 0; i < callPatchCount; ++i) {
                    memcpy(reinterpret_cast<void*>(addresses[i]), callPatches[i].original,
                           sizeof(callPatches[i].original));
                }
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
            }
            else {
                SYSLOG("VCN", "Failed to roll back X6000 call-site patches (kernel write lock)");
            }
            this->x6000Prepared = false;
            SYSLOG("VCN", "Failed to route X6000 VCN entry points; patches rolled back");
            return;
        }

        this->x6000Prepared = true;
        NRed::singleton().setProp32("NRedVCNX6000ABIValidated", 1);
        SYSLOG("VCN", "X6000 VCN bridge prepared (%zu call-site patches)", callPatchCount);
    }
}
