// Unified Device and Media Capability Profiles
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

namespace
{

    constexpr auto kProfileMinVersion = MACOS_10_4_X;
    constexpr auto kProfileMaxVersion = MACOS_26_X;

    // VCN capability data for every supported APU family.  The engine enablement flag is set for the VCN 2.x
    // family: 0x1638 is the hardware-validated target and the other VCN 2.x APUs are experimental.  VCN 1.x
    // carries the data but stays disabled until a VCN 1.x path exists.
    //
    // The ring/fence sizes are the allocation sizes the future VCN submission path will request.  They are
    // carried in the profile now so the media path has a single source of truth; nothing allocates them yet.
    constexpr auto makeVcnMedia(const DeviceProfile::VcnGeneration generation, const char* const firmware,
                                const bool enableVcn0 = false) -> DeviceProfile::MediaPolicy
    {
        DeviceProfile::MediaPolicy media{};
        media.hasVCN0       = enableVcn0;
        media.vcnGeneration = generation;
        media.vcnFirmware   = firmware;
        media.vcnRingSize   = 0x10000;    // 64 KiB, planning default until the real size is validated
        media.vcnFenceSize  = 0x1000;     // 4 KiB, planning default until the real size is validated
        media.decode.h264   = true;
        media.decode.hevc   = true;
        media.decode.av1    = false;    // AV1 decode starts with VCN 4.0.
        return media;
    }

    // HWLibs embeds `Firmware/ativvaxy_rv.dat` for VCN 1.0 and `Firmware/ativvaxy_nv.dat` for VCN 2.2.
    constexpr auto kVcn1Media     = makeVcnMedia(DeviceProfile::kVcn1_0, "ativvaxy_rv.dat");
    constexpr auto kVcn2MediaVcn0 = makeVcnMedia(DeviceProfile::kVcn2_2, "ativvaxy_nv.dat", true);

    // Profiles for the APUs NootedRed supports today.  The VCN 2.x family (Renoir, Renoir/Green Sardine,
    // Cezanne and Barcelo) enables the media engine so the same VDA implementation can be tried on every
    // VCN 2.x APU; 0x1638 is the hardware-validated target and the others are experimental.  Raven, Raven2
    // and Picasso are VCN 1.x, which the X6000 VCN2 bridge cannot serve, so they keep media disabled until
    // a VCN 1.x strategy exists.
    //
    // The table is keyed by device ID only.  The [minVersion, maxVersion] window is metadata for now; the
    // authoritative selector for patch data becomes the identity of the Apple binary being patched
    // (docs/Compatibility-Architecture.md).  The support level is a coarse per-profile roll-up: the only
    // hardware-validated combination is the conditional GL/Metal fix on macOS 15.8.1 / 0x1638, but that is a
    // per-build fact and this entry is version-wide, so no profile is marked kValidated until the generated
    // manifest can express per-build levels.  Media enablement is limited to VCN 2.x, which also keeps the
    // roll-up at experimental.  Unified memory is allowed on the Renoir family, where the X5000 memory paths were
    // verified; Raven, Raven2 and Picasso are not tested.
    constexpr DeviceProfile::Profile kProfiles[] = {
        {"Renoir", 0x1636, DeviceProfile::kRenoir, DeviceProfile::kExperimental, kProfileMinVersion,
         kProfileMaxVersion, {}, kVcn2MediaVcn0, {true}},
        {"Renoir/Green Sardine", 0x1638, DeviceProfile::kGreenSardine, DeviceProfile::kExperimental,
         kProfileMinVersion, kProfileMaxVersion, {}, kVcn2MediaVcn0, {true}},
        {"Cezanne", 0x164C, DeviceProfile::kCezanne, DeviceProfile::kExperimental, kProfileMinVersion,
         kProfileMaxVersion, {}, kVcn2MediaVcn0, {true}},
        {"Barcelo", 0x15E7, DeviceProfile::kBarcelo, DeviceProfile::kExperimental, kProfileMinVersion,
         kProfileMaxVersion, {}, kVcn2MediaVcn0, {true}},
        {"Raven", 0x15DD, DeviceProfile::kRaven, DeviceProfile::kExperimental, kProfileMinVersion,
         kProfileMaxVersion, {}, kVcn1Media, {}},
        // 0x15D8 covers Raven2 and Picasso; NRed::hwLateInit() refines the distinction from devRevision.
        {"Raven2/Picasso", 0x15D8, DeviceProfile::kPicasso, DeviceProfile::kExperimental, kProfileMinVersion,
         kProfileMaxVersion, {}, kVcn1Media, {}},
    };

    constexpr const DeviceProfile::Profile kFallbackProfile = {
        "unknown/fallback", 0, DeviceProfile::kUnknown, DeviceProfile::kUnavailable, kProfileMinVersion,
        kProfileMaxVersion, {}, {}, {}};

    constexpr size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

    // Compile-time invariants for the table: adding an APU must not silently break selection, validation
    // accounting, or media enablement.  Adjust these asserts together with the table.
    constexpr auto uniqueDeviceIDs() -> bool
    {
        for (size_t i = 0; i < kProfileCount; i++) {
            for (size_t j = i + 1; j < kProfileCount; j++) {
                if (kProfiles[i].deviceID == kProfiles[j].deviceID) { return false; }
            }
        }

        return true;
    }

    constexpr auto versionRangesValid() -> bool
    {
        for (const auto& profile : kProfiles) {
            if (profile.maxVersion < profile.minVersion) { return false; }
        }

        return true;
    }

    constexpr auto graphicsPoliciesEnabled() -> bool
    {
        for (const auto& profile : kProfiles) {
            if (!profile.graphics.conditionalDualQuad || profile.graphics.numShaderEngines == 0) { return false; }
        }

        return true;
    }

    // Unified memory stays limited to the Renoir family, the only one whose X5000 memory paths were checked.
    constexpr auto unifiedMemoryOnRenoirFamily() -> bool
    {
        for (const auto& profile : kProfiles) {
            if (!profile.memory.unifiedMemory) { continue; }
            if (profile.family != DeviceProfile::kRenoir && profile.family != DeviceProfile::kGreenSardine
                && profile.family != DeviceProfile::kCezanne && profile.family != DeviceProfile::kBarcelo)
            {
                return false;
            }
        }

        return !kFallbackProfile.memory.unifiedMemory;
    }

    constexpr auto mediaDisabled(const DeviceProfile::MediaPolicy& media) -> bool
    {
        return !media.hasUVD0 && !media.hasVCE && !media.hasVCN0;
    }

    // Media enablement is limited to VCN 2.x profiles until a VCN 1.x path exists; the validated 0x1638
    // target must keep it on so a regression there cannot slip through silently.
    template<size_t N>
    constexpr auto onlyVcn2ProfilesEnableMedia(const DeviceProfile::Profile (&profiles)[N]) -> bool
    {
        for (const auto& profile : profiles) {
            if (mediaDisabled(profile.media)) { continue; }
            if (profile.media.vcnGeneration != DeviceProfile::kVcn2_2) { return false; }
        }

        return true;
    }

    template<size_t N>
    constexpr auto profileEnablesMedia(const DeviceProfile::Profile (&profiles)[N], const UInt16 deviceID) -> bool
    {
        for (const auto& profile : profiles) {
            if (profile.deviceID == deviceID) { return !mediaDisabled(profile.media); }
        }

        return false;
    }

    template<size_t N>
    constexpr auto allVcnDataPresent(const DeviceProfile::Profile (&profiles)[N]) -> bool
    {
        for (const auto& profile : profiles) {
            const auto& media = profile.media;
            if (media.vcnGeneration == DeviceProfile::kVcnNone || media.vcnFirmware == nullptr
                || media.vcnRingSize == 0 || media.vcnFenceSize == 0 || !media.decode.h264 || !media.decode.hevc
                || media.decode.av1)
            {
                return false;
            }
        }

        return true;
    }

    static_assert(kProfileCount == 6, "Update the profile tests when adding a device");
    static_assert(uniqueDeviceIDs(), "Device profile device IDs must be unique");
    static_assert(versionRangesValid(), "Device profile version ranges must be ordered");
    static_assert(graphicsPoliciesEnabled(), "Current profiles must keep the graphics policy enabled");
    static_assert(onlyVcn2ProfilesEnableMedia(kProfiles),
                  "Media enablement is limited to VCN 2.x profiles until a VCN 1.x path exists");
    static_assert(profileEnablesMedia(kProfiles, 0x1638),
                  "The hardware-validated 0x1638 profile must keep VCN0 enabled");
    static_assert(allVcnDataPresent(kProfiles),
                  "Every profile needs VCN data, H.264/HEVC decode and no AV1 before media can be enabled");
    static_assert(mediaDisabled(kFallbackProfile.media), "The fallback profile must not enable any media engine");
    static_assert(unifiedMemoryOnRenoirFamily(), "Unified memory is limited to the Renoir family profiles");

}    // namespace

const DeviceProfile::Profile* DeviceProfile::current()
{
    const auto deviceID = NRed::singleton().getDeviceID();
    if (deviceID == 0) { return &kFallbackProfile; }

    const auto* const profile = forDevice(deviceID, currentKernelVersion());
    return profile != nullptr ? profile : &kFallbackProfile;
}

const DeviceProfile::Profile* DeviceProfile::forDevice(const UInt16 deviceID)
{
    for (const auto& profile : kProfiles) {
        if (profile.deviceID == deviceID) { return &profile; }
    }

    return nullptr;
}

const DeviceProfile::Profile* DeviceProfile::forDevice(const UInt16 deviceID,
                                                       const PenguinWizardry::KernelVersion& version)
{
    for (const auto& profile : kProfiles) {
        if (profile.deviceID != deviceID) { continue; }
        if (version < profile.minVersion || profile.maxVersion < version) { continue; }

        return &profile;
    }

    return nullptr;
}

const char* DeviceProfile::familyName(const ApuFamily family)
{
    switch (family) {
        case kRaven: return "Raven";
        case kRaven2: return "Raven2";
        case kPicasso: return "Picasso";
        case kRenoir: return "Renoir";
        case kRenoirE: return "RenoirE";
        case kGreenSardine: return "Green Sardine";
        case kCezanne: return "Cezanne";
        case kBarcelo: return "Barcelo";
        case kUnknown: return "unknown";
    }

    return "unknown";
}

const char* DeviceProfile::levelName(const SupportLevel level)
{
    switch (level) {
        case kUnavailable: return "unavailable";
        case kExperimental: return "experimental";
        case kValidated: return "validated";
    }

    return "unavailable";
}

void DeviceProfile::logCurrent()
{
    const auto* const profile = current();
    const auto& version = currentKernelVersion();
    const auto deviceID = NRed::singleton().getDeviceID();
    const auto pciRevision = NRed::singleton().getPciRevision();
    const auto* const firmware = profile->media.vcnFirmware != nullptr ? profile->media.vcnFirmware : "none";

    SYSLOG("DeviceProfile",
           "Compat: profile=\"%s\" family=%s level=%s device=0x%04X rev=0x%02X darwin=%u.%u "
           "graphics{dualQuad=%u hwInfo=%u rbPlus=%u numSE=%u} "
           "media{gate=%u vcn=0x%04X fw=%s ring=0x%X fence=0x%X h264=%u hevc=%u av1=%u} memory{uma=%u}",
           profile->name, familyName(profile->family), levelName(profile->level), static_cast<unsigned>(deviceID),
           static_cast<unsigned>(pciRevision), static_cast<unsigned>(version.major()),
           static_cast<unsigned>(version.minor()), static_cast<unsigned>(profile->graphics.conditionalDualQuad),
           static_cast<unsigned>(profile->graphics.correctHwInfo),
           static_cast<unsigned>(profile->graphics.rbPlusAllowed),
           static_cast<unsigned>(profile->graphics.numShaderEngines),
           static_cast<unsigned>(profile->media.hasVCN0),
           static_cast<unsigned>(profile->media.vcnGeneration), firmware,
           static_cast<unsigned>(profile->media.vcnRingSize),
           static_cast<unsigned>(profile->media.vcnFenceSize),
           static_cast<unsigned>(profile->media.decode.h264),
           static_cast<unsigned>(profile->media.decode.hevc),
           static_cast<unsigned>(profile->media.decode.av1),
           static_cast<unsigned>(profile->memory.unifiedMemory));

    auto& nred = NRed::singleton();
    nred.setProp32("NRedCompatDeviceID", deviceID);
    nred.setProp32("NRedCompatFamily", profile->family);
    nred.setProp32("NRedCompatLevel", profile->level);
    nred.setProp32("NRedCompatNumShaderEngines", profile->graphics.numShaderEngines);
    nred.setProp32("NRedCompatVCNGeneration", profile->media.vcnGeneration);
    nred.setProp32("NRedCompatUnifiedMemory", profile->memory.unifiedMemory);
}
