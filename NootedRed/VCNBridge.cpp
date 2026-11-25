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
#include <sys/proc.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <UserPatches.hpp>
#include <VCNBridge.hpp>
#include <VCNKernelData.hpp>
#include <VCNKernelDataVentura.hpp>
#if __has_include(<VCNKernelDataTahoe.hpp>)
#include <VCNKernelDataTahoe.hpp>
#define NRED_HAS_VCN_TAHOE 1
#endif
#include <VDA.hpp>
#include <VP9Probe.hpp>
#include <VideoCommandPatch.hpp>
#include <X5000.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>

// libkern/version.h declares this with C linkage; including the header conflicts with the version
// constants already declared by KernelVersion.hpp, so declare only the build string we need.
extern "C" char osversion[];

static VCNBridge moduleInstance;

namespace
{

    const char* const kBootArgNoVDA  = "-NRedNoVDA";
    constexpr UInt32  kVCNDecChannel = 8;


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
        return isSupportedBuild() && VDA::singleton().canAdvertise() && VDA::singleton().isBridgeRequested();
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


    struct VP9ServiceSink
    {
        IOService* service;
        bool       string(const char* key, const char* value) { return this->service->setProperty(key, value); }
        bool       profiles(const char* key, const char* arrayKey, const UInt32* values, const size_t count)
        {
            auto* array = OSArray::withCapacity(static_cast<unsigned int>(count));
            auto* dict  = OSDictionary::withCapacity(1);
            bool  ok    = array != nullptr && dict != nullptr;
            for (size_t i = 0; ok && i < count; ++i) {
                auto* number = OSNumber::withNumber(values[i], 32);
                ok           = number != nullptr && array->setObject(number);
                OSSafeReleaseNULL(number);
            }
            ok = ok && dict->setObject(arrayKey, array) && this->service->setProperty(key, dict);
            OSSafeReleaseNULL(array);
            OSSafeReleaseNULL(dict);
            return ok;
        }
        void remove(const char* key) { this->service->removeProperty(key); }
    };

    void publishVP9Probe(IOService* const service, const bool eligible)
    {
        if (service == nullptr) { return; }
        VP9ServiceSink sink{service};
        const auto     status = VP9Probe::publish(sink, eligible);
        NRed::singleton().setProp32(VP9Probe::StatusKey, status);
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

    this->acceleratorLock = IOLockAlloc();
    if (this->acceleratorLock == nullptr) {
        this->requested    = false;
        this->mediaFailure = 20;
        SYSLOG("VCN", "Cannot allocate media state lock; bridge disabled");
        return;
    }

    // Register the optional X6000 load callback up front.  The kext is only requested from inside the
    // X5000 accelerator start, i.e. after both drivers' load hooks are in place.
    lilu.onKextLoadForce(&kextRadeonX6000);

    SInt32 maxCtx = 6;
    if (PE_parse_boot_argn("vcnmaxctx", &maxCtx, sizeof(maxCtx))) {
        if (maxCtx < 1) { maxCtx = 1; }
        else if (maxCtx > static_cast<SInt32>(kMaxTrackedVideoContexts)) {
            maxCtx = static_cast<SInt32>(kMaxTrackedVideoContexts);
        }
    }
    this->maxVideoContexts = maxCtx;

    DBGLOG("VCN", "VCN bridge requested (maxVideoContexts=%d); waiting for device and driver load",
           this->maxVideoContexts);
}

bool VCNBridge::loadReady() const
{
    return this->requested && this->x5000Prepared && this->x6000Prepared && this->loadCompleted;
}

void VCNBridge::setAccelerator(IOService* const accelerator)
{
    if (this->acceleratorLock == nullptr) { return; }
    if (accelerator != nullptr) { accelerator->retain(); }
    IOLockLock(this->acceleratorLock);
    auto* const previous    = this->acceleratorHandle;
    this->acceleratorHandle = accelerator;
    IOLockUnlock(this->acceleratorLock);
    if (previous != nullptr) { previous->release(); }
}

void VCNBridge::failMedia(const UInt32 reason)
{
    if (reason == 0 || !OSCompareAndSwap(0, reason, &this->mediaFailure)) { return; }
    publishReject(reason);
    NRed::singleton().setProp32("NRedVCNMediaFailure", reason);
    NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
    NRed::singleton().setProp32("NRedVCNUserPatchesAllowed", 0);
    IOService* accelerator = nullptr;
    if (this->acceleratorLock != nullptr) {
        IOLockLock(this->acceleratorLock);
        accelerator = this->acceleratorHandle;
        if (accelerator != nullptr) { accelerator->retain(); }
        IOLockUnlock(this->acceleratorLock);
    }
    if (accelerator != nullptr) {
        VDA::withdrawCapabilities(accelerator);
        accelerator->release();
    }
    SYSLOG("VCN", "Media bridge failed (%u); new page patches/capabilities disabled until reboot", reason);
}

void VCNBridge::setVP9Ready(const bool available)
{
    if (!this->requested) { return; }
    const bool previous = __atomic_exchange_n(&this->vp9Ready, available, __ATOMIC_ACQ_REL);
    if (previous == available) { return; }

    IOService* accelerator = nullptr;
    if (this->acceleratorLock != nullptr) {
        IOLockLock(this->acceleratorLock);
        accelerator = this->acceleratorHandle;
        if (accelerator != nullptr) { accelerator->retain(); }
        IOLockUnlock(this->acceleratorLock);
    }
    if (accelerator != nullptr) {
        publishVP9Probe(accelerator, available && this->userPatchesAllowed());
        accelerator->release();
    }
}

void* VCNBridge::allocateEngine(void* const hardware)
{
    if (!this->requested) { return nullptr; }

    if (!this->loadReady() || this->hasMediaFailed() || !VDA::singleton().canAdvertise() ||
        this->engineMC == nullptr) {
        NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
        return nullptr;
    }

    this->hardwareHandle  = hardware;
    auto* const engine    = this->engineMC->alloc();
    this->vcnEngine       = engine;
    this->engineAllocated = engine != nullptr;
    this->clearTrackedContexts();

    NRed::singleton().setProp32("NRedVCNEngineAllocated", static_cast<UInt32>(this->engineAllocated));
    NRed::singleton().setProp32("NRedVDAMediaEnabled", static_cast<UInt32>(this->engineAllocated));
    NRed::singleton().setProp32("NRedVCNUserPatchesAllowed", this->userPatchesAllowed() ? 1 : 0);
    NRed::singleton().setProp32("NRedVCNMaxContexts", static_cast<UInt32>(this->maxVideoContexts));
    NRed::singleton().setProp32("NRedVCNQuotaFallbacks", 0);
    NRed::singleton().setProp32("NRedVCNResetCount", 0);
    NRed::singleton().setProp32("NRedVCNSkipReplayRetired", 0);
    NRed::singleton().setPropStr("NRedVCNActiveOwners", "none");
    if (!this->engineAllocated) {
        this->failMedia(7);
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
    bridge.setAccelerator(static_cast<IOService*>(self));
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
        bridge.vcnEngine       = nullptr;
        bridge.clearTrackedContexts();
        NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
        NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
        if (bridge.hardwareHandle != nullptr) {
            X5000::singleton().setVCN0Capability(bridge.hardwareHandle, false);
            bridge.hardwareHandle = nullptr;
        }
    }

    publishVP9Probe(static_cast<IOService*>(self),
                    result && bridge.userPatchesAllowed() && __atomic_load_n(&bridge.vp9Ready, __ATOMIC_ACQUIRE));

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
    bridge.vcnEngine       = nullptr;
    bridge.hardwareHandle  = nullptr;
    bridge.clearTrackedContexts();
    bridge.setAccelerator(nullptr);

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
    auto&      bridge = singleton();
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
    bridge.vcnEngine       = nullptr;
    bridge.loadCompleted   = false;
    bridge.clearTrackedContexts();
    NRed::singleton().setProp32("NRedVCNEngineAllocated", 0);
    NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
    if (bridge.hardwareHandle != nullptr) {
        X5000::singleton().setVCN0Capability(bridge.hardwareHandle, false);
    }

    return false;
}

bool VCNBridge::fetchResource(void* const self, void* const parameters, void* const resourceInfo)
{
    auto&      bridge = singleton();
    const bool ok     = FunctionCast(fetchResource, bridge.orgFetchResource)(self, parameters, resourceInfo);
    if (!ok) {
        const auto count = OSIncrementAtomic(&bridge.resourceFailures) + 1;
        NRed::singleton().setProp32("NRedVCNResourceFailures", static_cast<UInt32>(count));
    }
    return ok;
}

bool VCNBridge::initVCNContext(void* const self, void* const accelerator, void* const context,
                              void* const createInfo)
{
    auto& bridge = singleton();
    if (!bridge.userPatchesAllowed() || createInfo == nullptr) { return false; }

    // sVideoContextCreateInfo + 4 holds the context/codec type across macOS 13-26:
    // type 5 is AV1 (unsupported on VCN1), type 6/7 are AVC/HEVC encode.
    UInt32 type;
    memcpy(&type, static_cast<const UInt8*>(createInfo) + sizeof(UInt32), sizeof(type));
    if (type == 5) { return false; }

    if (type == 6 || type == 7) {
        const bool hevc = type == 7;
        const auto& vda = VDA::singleton();
        const bool requested = hevc ? vda.isRaven2HEVCEncodeTestRequested() : vda.isRaven2AVCEncodeTestRequested();
        if (!requested || !UserPatches::singleton().raven2EncoderPagesReady(hevc)) {
            NRed::singleton().setProp32("NRedRaven2EncodeContextRejected", 1);
            return false;
        }
        const bool result = FunctionCast(initVCNContext, bridge.orgInitVCNContext)(self, accelerator, context, createInfo);
        NRed::singleton().setProp32(hevc ? "NRedRaven2HEVCEncodeContextResult" : "NRedRaven2EncodeContextResult",
                                   result ? 1 : 2);
        return result;
    }

    return FunctionCast(initVCNContext, bridge.orgInitVCNContext)(self, accelerator, context, createInfo);
}

static void publishActiveOwners(void* const (&slots)[16], const int (&pids)[16], const char (&names)[16][16])
{
    char   buf[256];
    size_t pos   = 0;
    bool   first = true;
    for (size_t i = 0; i < 16; ++i) {
        if (__atomic_load_n(&slots[i], __ATOMIC_ACQUIRE) == nullptr) { continue; }
        const int n = snprintf(buf + pos, sizeof(buf) - pos, "%s%d:%s", first ? "" : ",", pids[i], names[i]);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf) - pos) { break; }
        pos  += static_cast<size_t>(n);
        first = false;
    }
    NRed::singleton().setPropStr("NRedVCNActiveOwners", first ? "none" : buf);
}

bool VCNBridge::trackContextStart(void* const context)
{
    if (context == nullptr) { return false; }
    for (size_t i = 0; i < kMaxTrackedVideoContexts; ++i) {
        if (__atomic_load_n(&this->activeContextSlots[i], __ATOMIC_ACQUIRE) == context) {
            return true;
        }
    }
    for (size_t i = 0; i < kMaxTrackedVideoContexts; ++i) {
        void* expected = nullptr;
        if (__atomic_compare_exchange_n(&this->activeContextSlots[i], &expected, context, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            this->activeContextPids[i] = proc_selfpid();
            proc_selfname(this->activeContextNames[i], sizeof(this->activeContextNames[i]));
            this->activeContextNames[i][sizeof(this->activeContextNames[i]) - 1] = '\0';
            const SInt32 count = OSIncrementAtomic(&this->activeVideoContexts) + 1;
            NRed::singleton().setProp32("NRedVCNActiveContexts", static_cast<UInt32>(count > 0 ? count : 0));
            publishActiveOwners(this->activeContextSlots, this->activeContextPids, this->activeContextNames);
            return true;
        }
    }
    return false;
}

bool VCNBridge::trackContextStop(void* const context)
{
    if (context == nullptr) { return false; }
    for (size_t i = 0; i < kMaxTrackedVideoContexts; ++i) {
        void* expected = context;
        if (__atomic_compare_exchange_n(&this->activeContextSlots[i], &expected, nullptr, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            this->activeContextPids[i]    = 0;
            this->activeContextNames[i][0] = '\0';
            SInt32 current = __atomic_load_n(&this->activeVideoContexts, __ATOMIC_ACQUIRE);
            while (current > 0) {
                if (__atomic_compare_exchange_n(&this->activeVideoContexts, &current, current - 1, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    current -= 1;
                    break;
                }
            }
            NRed::singleton().setProp32("NRedVCNActiveContexts", static_cast<UInt32>(current > 0 ? current : 0));
            publishActiveOwners(this->activeContextSlots, this->activeContextPids, this->activeContextNames);
            return true;
        }
    }
    return false;
}

void VCNBridge::clearTrackedContexts()
{
    for (size_t i = 0; i < kMaxTrackedVideoContexts; ++i) {
        __atomic_store_n(&this->activeContextSlots[i], nullptr, __ATOMIC_RELEASE);
        this->activeContextPids[i]     = 0;
        this->activeContextNames[i][0] = '\0';
    }
    __atomic_store_n(&this->activeVideoContexts, 0, __ATOMIC_RELEASE);
    NRed::singleton().setProp32("NRedVCNActiveContexts", 0);
    NRed::singleton().setPropStr("NRedVCNActiveOwners", "none");
}

void VCNBridge::drainVCN(const char* const reason)
{
    if (!this->engineEnabled()) { return; }
    void* const engine = this->vcnEngine;
    if (engine == nullptr || this->vcnIsIdle == 0) { return; }

    auto isQuiet = [&]() -> bool {
        if (!reinterpret_cast<bool (*)(void*)>(this->vcnIsIdle)(engine)) {
            return false;
        }
        if (this->vcnGetHWChannel != 0 && this->vcnChannelIsEmpty != 0) {
            for (UInt32 ringType = 0; ringType < 3; ++ringType) {
                void* const ch = reinterpret_cast<void* (*)(void*, UInt32)>(this->vcnGetHWChannel)(engine, ringType);
                if (ch != nullptr && !reinterpret_cast<bool (*)(void*)>(this->vcnChannelIsEmpty)(ch)) {
                    return false;
                }
            }
        }
        return true;
    };

    if (isQuiet()) { return; }

    // Bounded micro-drain (<= 1ms spin in 50us steps, then <= 3ms yield in 1ms steps) before the video
    // context tears down its VMID GPU page tables in MMHUB.
    for (UInt32 i = 0; i < 20; ++i) {
        IODelay(50);
        if (isQuiet()) { return; }
    }
    for (UInt32 i = 0; i < 3; ++i) {
        IOSleep(1);
        if (isQuiet()) { return; }
    }
    DBGLOG("VCN", "drainVCN(%s): VCN still busy after 4ms bounded wait; proceeding with context stop",
           reason != nullptr ? reason : "?");
}

void VCNBridge::triggerSelfHealingCooldown(const char* const reason)
{
    const auto totalResets = static_cast<UInt32>(OSIncrementAtomic(&this->vcnResetCount) + 1);
    const auto streak      = static_cast<UInt32>(OSIncrementAtomic(&this->consecutiveResets) + 1);
    NRed::singleton().setProp32("NRedVCNResetCount", totalResets);

    // Stage 1: Enter a 3-second cooldown window so new VideoToolbox sessions temporarily fall back to CPU
    // software decode while PowerPlay power-gates VCN (PowerDownVcn / PowerUpVcn) to clear any MMHUB/VCN wedge.
    static constexpr UInt64 kCooldownDurationNs = 3000000000ULL;
    UInt64                  nowNs               = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
    __atomic_store_n(&this->cooldownUntilNs, nowNs + kCooldownDurationNs, __ATOMIC_RELEASE);

    SYSLOG("VCN", "VCN reset triggered (%s): total=%u consecutive=%u; entering 3s self-healing cooldown",
           reason != nullptr ? reason : "?", totalResets, streak);

    // Stage 2: Only trip the permanent circuit breaker if VCN fails 3 consecutive times without a clean
    // video context completion in between.
    if (streak >= 3) {
        SYSLOG("VCN", "VCN consecutive resets reached %u; escalating to permanent software decode fallback", streak);
        this->failMedia(19);
    }
}

bool VCNBridge::videoContextStart(void* const self)
{
    auto& bridge = singleton();
    if (!bridge.userPatchesAllowed()) { return false; }

    const UInt64 cooldownEnd = __atomic_load_n(&bridge.cooldownUntilNs, __ATOMIC_ACQUIRE);
    if (cooldownEnd != 0) {
        UInt64 nowNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
        if (nowNs < cooldownEnd) {
            SYSLOG("VCN", "videoContextStart: VCN in 3s self-healing cooldown; routing session to software decode");
            return false;
        }
    }

    // Check if this context is already tracked (idempotent re-start); otherwise enforce concurrency quota.
    bool alreadyTracked = false;
    if (self != nullptr) {
        for (size_t i = 0; i < kMaxTrackedVideoContexts; ++i) {
            if (__atomic_load_n(&bridge.activeContextSlots[i], __ATOMIC_ACQUIRE) == self) {
                alreadyTracked = true;
                break;
            }
        }
    }
    if (!alreadyTracked) {
        const SInt32 active = __atomic_load_n(&bridge.activeVideoContexts, __ATOMIC_ACQUIRE);
        if (active >= bridge.maxVideoContexts) {
            const auto fallbacks = static_cast<UInt32>(OSIncrementAtomic(&bridge.quotaFallbacks) + 1);
            NRed::singleton().setProp32("NRedVCNQuotaFallbacks", fallbacks);
            DBGLOG("VCN",
                   "videoContextStart: active=%d >= quota=%d; routing excess stream to software decode (fallbacks=%u)",
                   active, bridge.maxVideoContexts, fallbacks);
            return false;
        }
    }

    const bool ok = FunctionCast(videoContextStart, bridge.orgVideoContextStart)(self);
    if (ok && !alreadyTracked) {
        bridge.trackContextStart(self);
    }
    return ok;
}

void VCNBridge::videoContextStop(void* const self)
{
    auto&      bridge     = singleton();
    const bool wasTracked = bridge.trackContextStop(self);
    if (wasTracked) {
        bridge.drainVCN("videoContextStop");
    }

    FunctionCast(videoContextStop, bridge.orgVideoContextStop)(self);

    if (wasTracked) {
        const UInt64 cooldownEnd = __atomic_load_n(&bridge.cooldownUntilNs, __ATOMIC_ACQUIRE);
        UInt64       nowNs       = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
        if (cooldownEnd == 0 || nowNs >= cooldownEnd) {
            __atomic_store_n(&bridge.consecutiveResets, 0, __ATOMIC_RELEASE);
        }
    }
}

void VCNBridge::videoContextDestroy(void* const self)
{
    auto& bridge = singleton();
    if (bridge.trackContextStop(self)) {
        bridge.drainVCN("videoContextDestroy");
    }
    FunctionCast(videoContextDestroy, bridge.orgVideoContextDestroy)(self);
}

bool VCNBridge::engineReset(void* const self, const UInt32 ringType, UInt32* const resetMaskOut)
{
    auto& bridge = singleton();
    if (self == nullptr || (bridge.vcnEngine != nullptr && self != bridge.vcnEngine)) {
        return FunctionCast(engineReset, bridge.orgEngineReset)(self, ringType, resetMaskOut);
    }

    if (resetMaskOut != nullptr) { *resetMaskOut = 0; }

    // AMDRadeonX6000_AMDHWEngine::reset returns early with *resetMaskOut == 0 because ring reset types at
    // +0x1A0 are 0xE (uninitialized) on VCN2, and its fallback path calls unpatched X6000 hardware vtable
    // slots 0x2C0/0x2C8 on the X5000 AMDVega10Hardware instance.
    // Power-cycle the VCN2 hardware block before re-initializing the ring, and only advertise the reset
    // mask (allowing IOAcceleratorFamily2 to reset the channel) when hardware re-initialization succeeds.
    if (ringType < 3) {
        void* hwChannel = nullptr;
        if (bridge.vcnGetHWChannel != 0) {
            hwChannel = reinterpret_cast<void* (*)(void*, UInt32)>(bridge.vcnGetHWChannel)(self, ringType);
        }

        // Perform a real VCN2 hardware power-cycle reset (releaseHardware + initHardware) before touching
        // the ring, because initializing the ring while VCN2 is powered down corrupts ring/PSP state.
        const UInt32 vcnChannel = kVCNDecChannel + ringType;
        if (bridge.vcnReleaseHardware != 0) {
            reinterpret_cast<bool (*)(void*, UInt32)>(bridge.vcnReleaseHardware)(self, vcnChannel);
        }

        bool reinitOk = true;
        if (bridge.orgInitHardware != 0) {
            reinitOk = FunctionCast(initHardware, bridge.orgInitHardware)(self, vcnChannel);
        }
        if (!reinitOk) {
            SYSLOG("VCN", "VCN2 hardware re-init failed during engineReset(channel=%u); failing media", vcnChannel);
            if (hwChannel != nullptr && bridge.vcnChannelReset != 0) {
                reinterpret_cast<void (*)(void*)>(bridge.vcnChannelReset)(hwChannel);
            }
            bridge.triggerSelfHealingCooldown("AMDHWEngine::reset");
            bridge.failMedia(19);
            return false;
        }

        void* const hwRing = getMember<void*>(self, 0x50 + ringType * sizeof(void*));
        if (hwRing != nullptr) {
            void* const vtable = *reinterpret_cast<void**>(hwRing);
            if (vtable != nullptr) {
                auto* const ringInitHw = getMember<void (*)(void*)>(vtable, 0x130);
                if (ringInitHw != nullptr) {
                    ringInitHw(hwRing);
                }
            }
        }

        if (hwChannel != nullptr) {
            const UInt32 channelIndex = getMember<UInt32>(hwChannel, 0x0C);
            if (resetMaskOut != nullptr && channelIndex < 32) {
                *resetMaskOut = (1U << channelIndex);
            }
            else if (bridge.vcnChannelReset != 0) {
                reinterpret_cast<void (*)(void*)>(bridge.vcnChannelReset)(hwChannel);
            }
        }
    }

    bridge.triggerSelfHealingCooldown("AMDHWEngine::reset");
    return true;
}

UInt64 VCNBridge::vcnSubmitCommandBuffer(void* const self, void* const submitInfo)
{
    auto&      bridge       = singleton();
    const UInt32 flags      = submitInfo != nullptr ? getMember<UInt32>(submitInfo, 0) : 0;
    const bool skipGuiltyCb = (flags & 0x2U) != 0;

    const UInt64 result = FunctionCast(vcnSubmitCommandBuffer, bridge.orgVcnSubmitCommandBuffer)(self, submitInfo);
    if (skipGuiltyCb && self != nullptr) {
        // Apple's AMDVCN2DecChannel::commitIndirectCommandBuffer returns early on flags & 0x02 without
        // emitting a fence packet or ringing the doorbell, leaving the skipped CB's HW timestamp unretired.
        // Do NOT call AMDHWChannel::reset() here because AMDSWScheduler::advance() already holds the
        // SWScheduler mutex and reset() would call resetHwChannel() and self-deadlock on the same lock.
        // Instead, retire the skipped CB's HW timestamp lock-free via AMDHWChannel's verified offsets
        // (+0x80 submitted, +0x84 completed, +0xC0 HW timestamp pointer).
        const UInt32 submitted    = getMember<UInt32>(self, 0x80);
        auto* const  completedPtr = reinterpret_cast<volatile UInt32*>(static_cast<UInt8*>(self) + 0x84);
        auto* const  tsPtr        = getMember<volatile UInt32*>(self, 0xC0);
        if (tsPtr != nullptr && tsPtr != completedPtr) {
            __atomic_store_n(tsPtr, submitted, __ATOMIC_RELEASE);
        }
        __atomic_store_n(completedPtr, submitted, __ATOMIC_RELEASE);

        const auto retired = static_cast<UInt32>(OSIncrementAtomic(&bridge.vcnSkippedReplayRetired) + 1);
        NRed::singleton().setProp32("NRedVCNSkipReplayRetired", retired);
        SYSLOG("VCN", "vcnSubmitCommandBuffer: retired skipped replay CB lock-free (flags=0x%x, ts=0x%x, count=%u)",
               flags, submitted, retired);
    }
    return result;
}

IOReturn VCNBridge::accelWaitForStamp(void* const self, const int stampIdx, const UInt32 stamp, UInt32* const statusOut)
{
    auto& bridge = singleton();
    // Guard only VCN channels (12: VCN0Dec, 13: VCN0Enc, 14: VCN0EncLLQ).
    if (stampIdx >= 12 && stampIdx <= 14) {
        const UInt64 cooldownEnd = __atomic_load_n(&bridge.cooldownUntilNs, __ATOMIC_ACQUIRE);
        bool         inCooldown  = bridge.hasMediaFailed();
        if (!inCooldown && cooldownEnd != 0) {
            UInt64 nowNs = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
            inCooldown = nowNs < cooldownEnd;
        }
        if (inCooldown) {
            if (statusOut != nullptr) { *statusOut = stamp; }
            SYSLOG("VCN", "accelWaitForStamp(ch=%d, stamp=0x%x): VCN in cooldown/failed; retiring wait cleanly",
                   stampIdx, stamp);
            return kIOReturnSuccess;
        }
    }

    return FunctionCast(accelWaitForStamp, bridge.orgAccelWaitForStamp)(self, stampIdx, stamp, statusOut);
}

bool VCNBridge::channelWaitForHwStamp(void* const self, const UInt32 stamp)
{
    auto& bridge = singleton();

    // If VCN is currently in the post-reset cooldown window or permanently failed, retire any pending
    // channel wait immediately so teardown/replay never stalls holding the IOAcceleratorFamily2 lock.
    const UInt64 cooldownEnd = __atomic_load_n(&bridge.cooldownUntilNs, __ATOMIC_ACQUIRE);
    if (bridge.hasMediaFailed() || cooldownEnd != 0) {
        UInt64 nowNs = 0;
        if (!bridge.hasMediaFailed()) {
            absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
        }
        if (bridge.hasMediaFailed() || nowNs < cooldownEnd) {
            if (self != nullptr && bridge.vcnChannelReset != 0) {
                reinterpret_cast<void (*)(void*)>(bridge.vcnChannelReset)(self);
            }
            return true;
        }
    }

    const bool ok = FunctionCast(channelWaitForHwStamp, bridge.orgWaitForHwStamp)(self, stamp);
    if (!ok && self != nullptr) {
        if (bridge.vcnChannelReset != 0) {
            reinterpret_cast<void (*)(void*)>(bridge.vcnChannelReset)(self);
        }
        bridge.triggerSelfHealingCooldown("AMDHWChannel::waitForHwStamp timeout");
        return true;
    }
    return ok;
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

        KernelPatcher::RouteRequest waitForStampRoute{
            "__ZN35AMDRadeonX5000_AMDAccelEventMachine12waitForStampEijPj", accelWaitForStamp,
            this->orgAccelWaitForStamp};
        if (!patcher.routeMultiple(id, &waitForStampRoute, 1, slide, size)) {
            patcher.clearError();
            SYSLOG("VCN", "Optional X5000 waitForStamp route unavailable; continuing");
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
            {"__ZN33AMDRadeonX6000_AMDAccelVCNContext4initEP37AMDRadeonX6000_AMDGraphicsAcceleratorP35AMDRadeonX6000_AMDAccelVideoContextP23sVideoContextCreateInfo",
             initVCNContext, this->orgInitVCNContext},
        };
        const size_t routeCount = arrsize(routes) - (VDA::singleton().isVcn1Mode() ? 0 : 1);
        if (!patcher.routeMultiple(id, routes, routeCount, slide, size)) {
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
            this->failMedia(17);
            SYSLOG("VCN", "Failed to route X6000 VCN entry points; patches rolled back");
            return;
        }

        // Route fetchResource independently for failure telemetry; gracefully skip if absent on an OS build.
        KernelPatcher::RouteRequest fetchRoute{
            "__ZN35AMDRadeonX6000_AMDAccelVideoContext13fetchResourceEP22_AMDVideoResourceParamP21_AMDVideoResourceInfo",
            fetchResource, this->orgFetchResource};
        if (!patcher.routeMultiple(id, &fetchRoute, 1, slide, size)) {
            patcher.clearError();
            SYSLOG("VCN", "Optional X6000 fetchResource telemetry route unavailable; continuing");
        }

        // Resolve VCN2 idle/channel helpers and route video context lifecycle + self-healing reset hooks.
        // All symbols are verified present across macOS 13-26; use graceful fallback on any missing symbol.
        resolve(patcher, id, "__ZN30AMDRadeonX6000_AMDVCN2HWEngine15releaseHardwareE21eAMDAccelVideoChannel",
                this->vcnReleaseHardware, slide, size);
        resolve(patcher, id, "__ZN30AMDRadeonX6000_AMDVCN2HWEngine6isIdleEv", this->vcnIsIdle, slide, size);
        resolve(patcher, id, "__ZN26AMDRadeonX6000_AMDHWEngine12getHWChannelE18_eAMD_HW_RING_TYPE",
                this->vcnGetHWChannel, slide, size);
        resolve(patcher, id, "__ZN27AMDRadeonX6000_AMDHWChannel7isEmptyEv", this->vcnChannelIsEmpty, slide, size);
        resolve(patcher, id, "__ZN27AMDRadeonX6000_AMDHWChannel5resetEv", this->vcnChannelReset, slide, size);

        KernelPatcher::RouteRequest lifecycleRoutes[] = {
            {"__ZN35AMDRadeonX6000_AMDAccelVideoContext12contextStartEv", videoContextStart,
             this->orgVideoContextStart},
            {"__ZN35AMDRadeonX6000_AMDAccelVideoContext11contextStopEv", videoContextStop, this->orgVideoContextStop},
            {"__ZN35AMDRadeonX6000_AMDAccelVideoContextD2Ev", videoContextDestroy, this->orgVideoContextDestroy},
            {"__ZN26AMDRadeonX6000_AMDHWEngine5resetE18_eAMD_HW_RING_TYPEPj", engineReset, this->orgEngineReset},
            {"__ZN27AMDRadeonX6000_AMDHWChannel19submitCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
             vcnSubmitCommandBuffer, this->orgVcnSubmitCommandBuffer},
            {"__ZN27AMDRadeonX6000_AMDHWChannel14waitForHwStampEj", channelWaitForHwStamp, this->orgWaitForHwStamp},
        };
        for (auto& req : lifecycleRoutes) {
            if (!patcher.routeMultiple(id, &req, 1, slide, size)) {
                patcher.clearError();
                SYSLOG("VCN", "Optional X6000 lifecycle/reset route %s unavailable; continuing", req.symbol);
            }
        }

        VideoCommandPatch::install(
            patcher, id, slide, size,
            "__ZN35AMDRadeonX6000_AMDAccelVideoContext14processCommandEPj16VideoCommandTypeS0_j",
            "__ZN35AMDRadeonX6000_AMDAccelVideoContext13fetchResourceEP22_AMDVideoResourceParamP21_AMDVideoResourceInfo",
            "NRedX6000VideoCommandGuarded");

        this->x6000Prepared = true;
        NRed::singleton().setProp32("NRedVCNX6000ABIValidated", 1);
        NRed::singleton().setProp32("NRedVCNResourceFailures", 0);
        SYSLOG("VCN", "X6000 VCN bridge prepared (%zu call-site patches)", callPatchCount);
    }
}
