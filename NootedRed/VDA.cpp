// VDA / Hardware Video Decode Policy
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
#include <Headers/kern_util.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatches.hpp>
#include <VCNBridge.hpp>
#include <VDA.hpp>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/OSTypes.h>

extern "C" char osversion[];
extern "C" const char version[];

static VDA moduleInstance;

namespace
{

    const char* const kBootArgNoVDA    = "-NRedNoVDA";
    const char* const kBootArgNoHEVC   = "-NRedNoHEVC";
    const char* const kBootArgNoHEVC10 = "-NRedNoHEVC10";
    constexpr char kRaven2OSBuild[]    = "23J631";
    constexpr char kRaven2KernelBuild[] = "root:xnu-10063.141.1.713.39~1/RELEASE_X86_64";

    const char* const kMediaCapabilities[] = {
        "IOGVACodec",         "IOGVAHEVCDecode",           "IOGVAHEVCDecodeCapabilities",
        "IOGVAHEVCEncode",    "IOGVAHEVCEncodeCapabilities", "IOGVAH264EncodeCapabilities",
        "IOGVAXDecode",       "IOGVAVP9Decode",            "IOGVAVP9DecodeCapabilities",
    };

}    // namespace

VDA& VDA::singleton() { return moduleInstance; }

bool VDA::isSupported() const
{
    const auto* const profile = DeviceProfile::current();
    const bool vcn2 = profile != nullptr && profile->media.hasVCN0 &&
                      profile->media.vcnGeneration == DeviceProfile::kVcn2_2;
    return this->enabled && (vcn2 || this->isVcn1DecodeRequested()) &&
           !NRed::singleton().hasExternalVideo() && UserPatches::singleton().mediaSupported();
}

bool VDA::canAdvertise() const
{
    return this->isSupported() && !VCNBridge::singleton().hasMediaFailed();
}

bool VDA::shouldStripCapabilities() const
{
    const auto* const profile = DeviceProfile::current();
    const bool bridgeDevice = NRed::singleton().getDeviceID() == 0x15D8 ||
                              (profile != nullptr && profile->media.vcnGeneration == DeviceProfile::kVcn2_2);
    return !this->enabled || (bridgeDevice && !this->canAdvertise());
}

bool VDA::isHEVC10EncodeAllowed() const
{
    return !this->isVcn1DecodeRequested() && this->isHEVC10Allowed() && this->canAdvertise() &&
           currentKernelVersion() >= KernelVersion::Sequoia;
}

bool VDA::isHEVC8KDecodeAllowed() const
{
    return !this->isVcn1DecodeRequested() && this->isHEVCDecodeAllowed() && this->canAdvertise() &&
           currentKernelVersion() >= KernelVersion::Sequoia;
}

void VDA::stripCapabilities(OSDictionary* const dictionary)
{
    if (dictionary == nullptr) { return; }
    for (const auto* key : kMediaCapabilities) { dictionary->removeObject(key); }
}

void VDA::withdrawCapabilities(IORegistryEntry* const entry)
{
    if (entry == nullptr) { return; }
    for (const auto* key : kMediaCapabilities) { entry->removeProperty(key); }
}

void VDA::init()
{
    const bool noVda    = checkKernelArgument(kBootArgNoVDA);
    const bool noHevc   = checkKernelArgument(kBootArgNoHEVC);
    const bool noHevc10 = checkKernelArgument(kBootArgNoHEVC10);

    this->enabled       = !noVda;
    this->hevcAllowed   = this->enabled && !noHevc;
    this->hevc10Allowed = this->hevcAllowed && !noHevc10;
    this->raven2Requested = this->enabled && checkKernelArgument("-NRedRaven2Decode");
    this->raven2HEVCRequested = this->raven2Requested && checkKernelArgument("-NRedRaven2HEVC");
    this->picassoRequested = this->enabled && checkKernelArgument("-NRedPicassoDecodeTest");

    if (!this->enabled) {
        DBGLOG("VDA", "-NRedNoVDA active: all hardware video acceleration disabled (AVC + HEVC)");
        return;
    }

    if (!this->hevcAllowed) {
        DBGLOG("VDA", "-NRedNoHEVC active: HEVC hardware encode/decode disabled; AVC hardware encode/decode enabled");
    }
    else {
        if (this->hevc10Allowed) {
            DBGLOG("VDA", "Hardware video acceleration enabled: AVC + HEVC (8/10-bit decode, 8-bit encode)");
        } else {
            DBGLOG("VDA", "-NRedNoHEVC10 active: 10-bit HEVC decode disabled");
        }
    }
}

void VDA::processPatcher()
{
    // Never null: DeviceProfile::current() falls back to the generic profile for unknown devices.
    const auto* const profile = DeviceProfile::current();

    const auto& media         = profile->media;
    const auto  vcnGeneration = static_cast<UInt32>(media.vcnGeneration);

    auto& nred = NRed::singleton();
    if (nred.getDeviceID() == 0x15D8) {
        nred.setProp32("NRedRaven2DecodeOptIn", static_cast<UInt32>(this->raven2Requested));
        if (this->raven2Requested || this->picassoRequested) {
            // The kernel version is available even if the OS build string has not been set at patcher load.
            const bool osBuildAvailable = osversion[0] != '\0';
            const bool buildMatches = osBuildAvailable ? strcmp(osversion, kRaven2OSBuild) == 0 :
                                                        strstr(version, kRaven2KernelBuild) != nullptr;
            nred.setProp32("NRedRaven2DecodeOSBuildAvailable", static_cast<UInt32>(osBuildAvailable));
            nred.setProp32("NRedRaven2DecodeBuildMatched", static_cast<UInt32>(buildMatches));
            nred.setProp32("NRedRaven2DecodeExternalAMD", static_cast<UInt32>(nred.hasDetectedExternalVideo()));
            if (buildMatches && !nred.hasDetectedExternalVideo()) {
                nred.hwLateInit();
                nred.setProp32("NRedRaven2DecodeStrapRevision", nred.getDevRevision());
                const auto& attributes = nred.getAttributes();
                this->raven2Decode = this->raven2Requested && attributes.isRaven2();
                // 0x15D8 starts with the Picasso attribute. The MMIO strap can also mark it as Raven2.
                this->picassoDecode = this->picassoRequested && attributes.isPicasso() && !attributes.isRaven2();
                if (this->raven2Decode) {
                    this->raven2AVCEncodeTest = checkKernelArgument("-NRedRaven2AVCEncodeTest");
                    nred.setProp32("NRedRaven2AVCEncodeTest", static_cast<UInt32>(this->raven2AVCEncodeTest));
                    this->hevcAllowed = this->hevcAllowed && this->raven2HEVCRequested;
                    this->hevc10Allowed = this->hevc10Allowed && this->hevcAllowed
                        && checkKernelArgument("-NRedRaven2HEVC10");
                    this->raven2HEVCEncodeTest = this->hevcAllowed
                        && checkKernelArgument("-NRedRaven2HEVCEncodeTest");
                    nred.setProp32("NRedRaven2DecodeRequested", 1);
                }
                if (this->picassoDecode) {
                    this->hevcAllowed = false;
                    this->hevc10Allowed = false;
                    nred.setProp32("NRedPicassoDecodeRequested", 1);
                }
            }
        }
    }

    DBGLOG("VDA", "%s: VCN %u.%u, firmware %s, ring 0x%X, fence 0x%X, H.264=%u HEVC=%u AV1=%u", profile->name,
           vcnGeneration >> 8, vcnGeneration & 0xFF, media.vcnFirmware != nullptr ? media.vcnFirmware : "none",
           media.vcnRingSize, media.vcnFenceSize, static_cast<UInt32>(media.decode.h264),
           static_cast<UInt32>(this->isHEVCDecodeAllowed() ? media.decode.hevc : false), static_cast<UInt32>(media.decode.av1));

    if (!this->enabled) {
        NRed::singleton().setProp32("NRedVDARequested", 0);
        NRed::singleton().setProp32("NRedVDAAVCOnly", 0);
        NRed::singleton().setProp32("NRedVDAHEVCEncodeAllowed", 0);
        NRed::singleton().setProp32("NRedVDAHEVCDecodeAllowed", 0);
        NRed::singleton().setProp32("NRedVDAHEVC10Allowed", 0);
        NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
        return;
    }

    const char* modeName = this->hevcAllowed ?
        (this->hevc10Allowed ? "AVC + HEVC 8/10-bit" : "AVC + 8-bit HEVC") : "AVC only";

    // The bridge reports its own readiness under the VCN tag; VDA only publishes the requested profile and
    // generation here.  If the bridge fails, software decode stays in use.
    DBGLOG("VDA", "VDA active (%s) for %s; VCN bridge readiness is reported by the VCN module",
           modeName, profile->name);

    NRed::singleton().setProp32("NRedVDARequested", 1);
    NRed::singleton().setProp32("NRedVDAAVCOnly", static_cast<UInt32>(!this->hevcAllowed));
    NRed::singleton().setProp32("NRedVDAHEVCEncodeAllowed", static_cast<UInt32>(this->isHEVCEncodeAllowed()));
    NRed::singleton().setProp32("NRedVDAHEVCDecodeAllowed", static_cast<UInt32>(this->hevcAllowed));
    NRed::singleton().setProp32("NRedVDAHEVC10Allowed", static_cast<UInt32>(this->hevc10Allowed));
    // The VCN bridge owns the effective enablement and updates this property after engine allocation.
    NRed::singleton().setProp32("NRedVDAMediaEnabled", 0);
    NRed::singleton().setProp32("NRedVCNGeneration", vcnGeneration);
}

