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

#pragma once
#include <MediaHardware.h>
#include <PenguinWizardry/KernelVersion.hpp>
#include <libkern/OSTypes.h>

// A single source of truth for per-device graphics and media capabilities.
//
// This is the hardware half of the compatibility model described in docs/Compatibility-Architecture.md:
// the other half is the identity of the Apple binary being patched.  A profile answers "what can this APU
// do" (graphics policy, media policy, validation level); it deliberately does not contain patch bytes or
// per macOS build data, so adding an APU never duplicates patch data and adding a macOS build never
// duplicates hardware facts.
//
// Graphics uses this profile to decide whether the Chromium dual-quad policy should be installed.
// Media describes the VCN/UVD/VDA capabilities of the device (generation, firmware, ring sizes, decode
// formats). VCN 2.2 profiles may use the bridge when its Apple binary guards pass;
// this does not mark an untested machine as hardware-validated.
class DeviceProfile
{
public:
    enum ApuFamily : UInt8 {
        kUnknown = 0,
        kRaven,
        kRaven2,
        kPicasso,
        kRenoir,
        kRenoirE,
        kGreenSardine,
        kCezanne,
        kBarcelo,
    };

    // Coarse validation roll-up for a profile.  The authoritative per-feature level is generated from the
    // compatibility manifest (docs/Compatibility-Architecture.md, Milestone 4); until then this only
    // controls logging and diagnostics.
    enum SupportLevel : UInt8 {
        kUnavailable = 0,
        kExperimental,
        kValidated,
    };

    struct GraphicsPolicy
    {
        // Install the conditional dual-quad fix (normal pipelines keep RB+, dual-source disables it).
        bool conditionalDualQuad{true};

        // Whether the existing HwInfo/CU corrections for this family should be applied.
        bool correctHwInfo{true};

        // Whether the APU is allowed to use RB+ at all.  This is the capability the conditional dual-quad
        // policy consults; it is logged today and becomes a patch-site precondition once the target identity
        // layer lands.  Mesa's predicate: Stoney/Vega12/Raven/Raven2/Renoir or gfx10.3+.
        bool rbPlusAllowed{true};

        // Shader-engine count written by the GL HwInfo correction.  The site lives in the driver binary
        // (version axis); the value is an APU fact, so it must never be baked into the page image.
        UInt8 numShaderEngines{1};
    };

    // VCN IP generation, encoded as major << 8 | minor so it can double as the `ipVersion` handed to the
    // firmware loader.  Raven/Picasso/Raven2 are VCN 1.0-class, Renoir/Cezanne/Barcelo are VCN 2.2.
    enum VcnGeneration : UInt32 {
        kVcnNone = NRedMediaIPUnknown,
        kVcn1_0 = NRedMediaIPVCN1,
        kVcn2_2 = NRedMediaIPVCN22,
    };

    // Formats the VCN generation can decode.  Hardware capabilities, not enablement.
    struct DecodeCaps
    {
        bool h264{false};
        bool hevc{false};
        bool av1{false};
    };

    struct MediaPolicy
    {
        // Media engine enablement flags.  The VCN 2.x profiles enable VCN0 so the same VDA implementation
        // can be tried on every VCN 2.2 APU. Hardware media acceptance was
        // measured on 0x1636; the other IDs remain experimental.
        // VCN 1.x profiles keep the engine disabled because the X6000 VCN2 bridge cannot serve them.
        bool hasUVD0{false};
        bool hasVCE{false};
        bool hasVCN0{false};

        // VCN generation of this device.  kVcnNone means the media policy has no VCN data (fallback).
        VcnGeneration vcnGeneration{kVcnNone};

        // Name of the VCN firmware blob embedded by `HWLibs.cpp` (`Firmware/`), or nullptr.  The actual
        // firmware load/PSP bring-up is a later phase; this only records which blob belongs to the device.
        const char* vcnFirmware{nullptr};

        // Ring and fence allocation sizes for the future VCN submission path, in bytes.
        UInt32 vcnRingSize{0};
        UInt32 vcnFenceSize{0};

        // Decode formats available on this VCN generation.
        DecodeCaps decode{};
    };

    struct Profile
    {
        const char*                    name;
        UInt16                         deviceID;
        ApuFamily                      family;
        SupportLevel                   level;
        PenguinWizardry::KernelVersion minVersion;
        PenguinWizardry::KernelVersion maxVersion;
        GraphicsPolicy                 graphics;
        MediaPolicy                    media;
    };

    // Returns the profile for the running device and kernel version, or the generic fallback when the
    // device is not known.  The fallback keeps the current graphics policy and disables all media engines.
    static const Profile* current();

    // Returns nullptr for an unsupported device ID, ignoring the kernel version.
    static const Profile* forDevice(UInt16 deviceID);

    // Returns nullptr when the device is unknown or the running kernel version is outside the profile's
    // [minVersion, maxVersion] window.  The version window is metadata for now: per-build patch data is
    // keyed by binary identity, not by this range.
    static const Profile* forDevice(UInt16 deviceID, const PenguinWizardry::KernelVersion& version);

    static const char* familyName(ApuFamily family);

    static const char* levelName(SupportLevel level);

    // Emits the canonical per-boot compatibility line and publishes the same facts as IORegistry
    // properties for field triage.  Called once from NRed::processPatcher() after device detection.
    static void logCurrent();
};
