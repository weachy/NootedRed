// IOKit Personality Injector
//
// Copyright (C) 2025-2026  ChefKiss.
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

#include <DriverInjector.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <NRed.hpp>
#include <VDA.hpp>
#include <libkern/OSTypes.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSString.h>

static DriverInjector moduleInstance;

// Keep the XML's conservative Main-only default. Publish Main10 only after the
// build/device/backend policy has accepted the complete encode patch manifest.
// Prepare independent nested dictionaries before touching the personality.
static bool enableHEVC10Encode(OSDictionary* const personality)
{
    if (personality == nullptr) { return false; }
    auto* const original = OSDynamicCast(OSDictionary, personality->getObject("IOGVAHEVCEncodeCapabilities"));
    if (original == nullptr) { return false; }
    auto* const originalDetails = OSDynamicCast(OSDictionary, original->getObject("VTPerProfileDetails"));
    auto* const main = originalDetails != nullptr ?
        OSDynamicCast(OSDictionary, originalDetails->getObject("1")) : nullptr;
    if (main == nullptr) { return false; }
    auto* const caps = OSDictionary::withDictionary(original);
    auto* const details = OSDictionary::withDictionary(originalDetails);
    auto* const profiles = OSArray::withCapacity(2);
    auto* const one = OSNumber::withNumber(static_cast<unsigned long long>(1), 32);
    auto* const two = OSNumber::withNumber(static_cast<unsigned long long>(2), 32);
    auto* const level = OSString::withCString("2");
    bool ready = caps != nullptr && details != nullptr && profiles != nullptr &&
                 one != nullptr && two != nullptr && level != nullptr;
    if (ready) {
        ready = details->setObject("2", main) && profiles->setObject(one) && profiles->setObject(two) &&
                caps->setObject("VTPerProfileDetails", details) &&
                caps->setObject("VTSupportedProfileArray", profiles);
    }
    if (ready) {
        // Publish the level last: AppleGVA uses it to accept the Main10 setter.
        ready = personality->setObject("IOGVAHEVCEncodeCapabilities", caps) &&
                personality->setObject("IOGVAHEVCEncode", level);
    }
    if (level != nullptr) { level->release(); }
    if (two != nullptr) { two->release(); }
    if (one != nullptr) { one->release(); }
    if (profiles != nullptr) { profiles->release(); }
    if (details != nullptr) { details->release(); }
    if (caps != nullptr) { caps->release(); }
    return ready;
}

static bool enableHEVC8KDecode(OSDictionary* const personality)
{
    if (personality == nullptr) { return false; }
    auto* const original = OSDynamicCast(OSDictionary, personality->getObject("IOGVAHEVCDecodeCapabilities"));
    auto* const originalDetails = original != nullptr ?
        OSDynamicCast(OSDictionary, original->getObject("VTPerProfileDetails")) : nullptr;
    if (originalDetails == nullptr) { return false; }
    auto* const caps = OSDictionary::withDictionary(original);
    auto* const details = OSDictionary::withDictionary(originalDetails);
    auto* const level = OSNumber::withNumber(static_cast<unsigned long long>(186), 32); // HEVC Level 6.2
    bool ready = caps != nullptr && details != nullptr && level != nullptr;
    const char* const profiles[] = {"1", "2", "3"};
    for (const auto* profile : profiles) {
        if (!ready) { break; }
        auto* const old = OSDynamicCast(OSDictionary, originalDetails->getObject(profile));
        if (old == nullptr) { continue; } // Preserve the Main-only boot policy.
        auto* const updated = OSDictionary::withDictionary(old);
        ready = updated != nullptr && updated->setObject("VTMaxDecodeLevel", level) &&
                details->setObject(profile, updated);
        if (updated != nullptr) { updated->release(); }
    }
    if (ready) {
        ready = caps->setObject("VTPerProfileDetails", details) &&
                personality->setObject("IOGVAHEVCDecodeCapabilities", caps);
    }
    if (level != nullptr) { level->release(); }
    if (details != nullptr) { details->release(); }
    if (caps != nullptr) { caps->release(); }
    return ready;
}

static const char com_apple_kext_AMDRadeonX5000[] = {
#include "Personalities/com.apple.kext.AMDRadeonX5000.inc"
, '\0'
};
static const char com_apple_kext_AMDRadeonX5000HWServices[] = {
#include "Personalities/com.apple.kext.AMDRadeonX5000HWServices.inc"
, '\0'
};
static const char com_apple_kext_AMDRadeonX6000Framebuffer[] = {
#include "Personalities/com.apple.kext.AMDRadeonX6000Framebuffer.inc"
, '\0'
};
static const char com_apple_driver_AppleGFXHDA[] = {
#include "Personalities/com.apple.driver.AppleGFXHDA.inc"
, '\0'
};

DriverInjector::DriverInjector() :
    drivers{
        Driver("com.apple.kext.AMDRadeonX6000Framebuffer", com_apple_kext_AMDRadeonX6000Framebuffer),
        Driver("com.apple.driver.AppleGFXHDA", com_apple_driver_AppleGFXHDA),
        Driver("com.apple.kext.AMDRadeonX5000", com_apple_kext_AMDRadeonX5000),
        Driver("com.apple.kext.AMDRadeonX5000HWServices", com_apple_kext_AMDRadeonX5000HWServices),
    }
{ }

DriverInjector& DriverInjector::singleton() { return moduleInstance; }

void DriverInjector::processPatcher(KernelPatcher& patcher)
{
    KernelPatcher::RouteRequest request{"__ZN11IOCatalogue10addDriversEP7OSArrayb", wrapAddDrivers,
                                        this->orgAddDrivers};
    PANIC_COND(!patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1), "DriverInjector",
               "Failed to route addDrivers");
}

bool DriverInjector::wrapAddDrivers(void* const self, OSArray* const array, const bool doNubMatching)
{
    UInt32 driverCount = array->getCount();
    for (UInt32 driverIndex = 0; driverIndex < driverCount; driverIndex += 1) {
        OSObject* object = array->getObject(driverIndex);
        if (object == nullptr) { continue; }
        auto* dict = OSDynamicCast(OSDictionary, object);
        if (dict == nullptr) { continue; }
        auto* bundleIdentifier = OSDynamicCast(OSString, dict->getObject("CFBundleIdentifier"));
        if (bundleIdentifier == nullptr || bundleIdentifier->getLength() == 0) { continue; }

        const auto toInjectCount = checkKernelArgument("-NRedNoAccel") ? 2 : arrsize(singleton().drivers);
        for (size_t identifierIndex = 0; identifierIndex < toInjectCount; identifierIndex += 1) {
            auto& driver = singleton().drivers[identifierIndex];

            if ((singleton().matchedDrivers & getBit(identifierIndex)) != 0
                || !bundleIdentifier->isEqualTo(driver.identifier))
            {
                continue;
            }

            singleton().matchedDrivers |= getBit(identifierIndex);

            DBGLOG("DriverInjector", "Matched %s, injecting.", driver.identifier);

            if (strcmp(driver.identifier, "com.apple.kext.AMDRadeonX5000") == 0) {
                const auto count = driver.personalities->getCount();
                NRed::singleton().setProp32("NRedHEVC10EncodeAdvertised", 0);
                NRed::singleton().setProp32("NRedHEVC8KDecodeAdvertised", 0);

                // Preserve upstream personality behavior on VCN 1.x. For a
                // VCN 2.2 target, reject codec claims if the pinned backend
                // cannot run. Engine allocation occurs after injection.
                if (VDA::singleton().shouldStripCapabilities()) {
                    for (UInt32 i = 0; i < count; ++i) {
                        auto* const pDict = OSDynamicCast(OSDictionary, driver.personalities->getObject(i));
                        VDA::stripCapabilities(pDict);
                    }
                    VDA::stripCapabilities(dict);
                    DBGLOG("DriverInjector", "Media bridge unavailable: stripped codec capabilities from %s", driver.identifier);
                }
                else {
                    // 1. HEVC Decode policy:
                    // - If !isHEVCDecodeAllowed() (-NRedNoHEVC): strip IOGVAHEVCDecode and IOGVAHEVCDecodeCapabilities.
                    // - If !isHEVC10Allowed() (-NRedNoHEVC10): retain the existing Main-only policy.
                    //   It removes Main10 (profile 2) and Main Still Picture (profile 3);
                    //   profile 3 itself is 8-bit, not a second 10-bit profile.
                    // - Default: Main/Main10; the reviewed VCN 2.2 manifest also enables Level 6.2.
                    if (!VDA::singleton().isHEVCDecodeAllowed()) {
                        for (UInt32 i = 0; i < count; ++i) {
                            auto* const pDict = OSDynamicCast(OSDictionary, driver.personalities->getObject(i));
                            if (pDict != nullptr) {
                                pDict->removeObject("IOGVAHEVCDecode");
                                pDict->removeObject("IOGVAHEVCDecodeCapabilities");
                            }
                        }
                        if (dict != nullptr) {
                            dict->removeObject("IOGVAHEVCDecode");
                            dict->removeObject("IOGVAHEVCDecodeCapabilities");
                        }
                        DBGLOG("DriverInjector", "-NRedNoHEVC active: stripped HEVC decode capabilities from %s", driver.identifier);
                    }
                    else if (!VDA::singleton().isHEVC10Allowed()) {
                        auto strip10Bit = [](OSDictionary* p) {
                            if (p == nullptr) { return; }
                            auto* const hevcCaps = OSDynamicCast(OSDictionary, p->getObject("IOGVAHEVCDecodeCapabilities"));
                            if (hevcCaps == nullptr) { return; }
                            auto* const details = OSDynamicCast(OSDictionary, hevcCaps->getObject("VTPerProfileDetails"));
                            if (details != nullptr) {
                                details->removeObject("2");
                                details->removeObject("3");
                            }
                            auto* const profArray = OSArray::withCapacity(1);
                            if (profArray != nullptr) {
                                auto* const oneNum = OSNumber::withNumber(static_cast<unsigned long long>(1), 32);
                                if (oneNum != nullptr) {
                                    profArray->setObject(oneNum);
                                    oneNum->release();
                                }
                                hevcCaps->setObject("VTSupportedProfileArray", profArray);
                                profArray->release();
                            }
                        };

                        for (UInt32 i = 0; i < count; ++i) {
                            strip10Bit(OSDynamicCast(OSDictionary, driver.personalities->getObject(i)));
                        }
                        if (dict != nullptr) {
                            strip10Bit(dict);
                        }
                        DBGLOG("DriverInjector", "-NRedNoHEVC10 active: restricted HEVC decode to 8-bit Main profile 1 for %s", driver.identifier);
                    }
                    else {
                        DBGLOG("DriverInjector", "Full HEVC hardware decode (8-bit and 10-bit Main 10) enabled for %s", driver.identifier);
                    }
                    if (VDA::singleton().isHEVC8KDecodeAllowed()) {
                        bool advertised = false;
                        for (UInt32 i = 0; i < count; ++i) {
                            advertised |= enableHEVC8KDecode(OSDynamicCast(OSDictionary, driver.personalities->getObject(i)));
                        }
                        advertised |= enableHEVC8KDecode(dict);
                        NRed::singleton().setProp32("NRedHEVC8KDecodeAdvertised", advertised ? 1 : 0);
                    }

                    // 2. HEVC Encode policy:
                    // - If !isHEVCEncodeAllowed() (-NRedNoHEVC): strip IOGVAHEVCEncode and IOGVAHEVCEncodeCapabilities.
                    // - Main10: exact build/device/backend + all four encode fixes.
                    // - With -NRedNoHEVC10: keep 8-bit Main.
                    if (!VDA::singleton().isHEVCEncodeAllowed()) {
                        for (UInt32 i = 0; i < count; ++i) {
                            auto* const pDict = OSDynamicCast(OSDictionary, driver.personalities->getObject(i));
                            if (pDict != nullptr) {
                                pDict->removeObject("IOGVAHEVCEncode");
                                pDict->removeObject("IOGVAHEVCEncodeCapabilities");
                            }
                        }
                        if (dict != nullptr) {
                            dict->removeObject("IOGVAHEVCEncode");
                            dict->removeObject("IOGVAHEVCEncodeCapabilities");
                        }
                        DBGLOG("DriverInjector", "-NRedNoHEVC active: stripped HEVC encode capabilities from %s", driver.identifier);
                    }
                    else if (VDA::singleton().isHEVC10EncodeAllowed()) {
                        bool advertised = false;
                        for (UInt32 i = 0; i < count; ++i) {
                            advertised |= enableHEVC10Encode(OSDynamicCast(OSDictionary, driver.personalities->getObject(i)));
                        }
                        advertised |= enableHEVC10Encode(dict);
                        NRed::singleton().setProp32("NRedHEVC10EncodeAdvertised", advertised ? 1 : 0);
                        DBGLOG("DriverInjector", "Validated HEVC Main10 encode advertisement=%u for %s",
                               static_cast<unsigned>(advertised), driver.identifier);
                    } else {
                        DBGLOG("DriverInjector", "Preserved HEVC 8-bit encode capabilities for %s", driver.identifier);
                    }
                }
            }

            array->merge(driver.personalities);

            break;
        }
    }

    return FunctionCast(wrapAddDrivers, singleton().orgAddDrivers)(self, array, doNubMatching);
}
