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

#pragma once

class OSDictionary;
class IORegistryEntry;

// Runtime policy layer for the VDA/VCN hardware video decode work.
//
// Hardware video acceleration is enabled by default (AVC + HEVC).
// Pass -NRedNoHEVC to disable HEVC while keeping AVC hardware acceleration.
// Pass -NRedNoVDA to disable all hardware video acceleration completely.
class VDA
{
    bool enabled{true};
    bool hevcAllowed{true};
    bool hevc10Allowed{true};

public:
    static VDA& singleton();

    // Parses -NRedNoVDA, -NRedNoHEVC, and -NRedNoHEVC10 boot arguments. Called from NRed::init().
    void init();

    // Resolves the running device profile and logs the media policy. Called from NRed::processPatcher().
    void processPatcher();

    // True when hardware video acceleration is enabled (default true, disabled by -NRedNoVDA).
    bool isRequested() const { return this->enabled; }
    bool isEnabled() const { return this->enabled; }

    // Supported device + exact media manifest + installed page backend.
    // Does not require an allocated engine; personality injection happens first.
    bool isSupported() const;
    bool canAdvertise() const;
    // Preserve the original personality on VCN 1.x/other devices. Only a
    // requested VCN 2.2 bridge needs its codec properties withheld when the
    // exact backend/build checks reject it.
    bool shouldStripCapabilities() const;
    // Main10 encode requires the VCN 2.2 bridge and the exact manifest
    // containing the P010/header fixes. Boot opt-outs apply.
    bool isHEVC10EncodeAllowed() const;
    // 8K decode requires the matching bounded resolution-check manifest.
    bool isHEVC8KDecodeAllowed() const;
    static void stripCapabilities(OSDictionary* dictionary);
    static void withdrawCapabilities(IORegistryEntry* entry);

    // True when only AVC is enabled and HEVC is disabled (-NRedNoHEVC).
    bool isAVCOnly() const { return this->enabled && !this->hevcAllowed; }

    // True when HEVC 8-bit hardware encode is allowed (default true, disabled by -NRedNoHEVC or -NRedNoVDA).
    bool isHEVCEncodeAllowed() const { return this->enabled && this->hevcAllowed; }

    // True when HEVC hardware decode (8-bit) is allowed (default true, disabled by -NRedNoHEVC or -NRedNoVDA).
    bool isHEVCDecodeAllowed() const { return this->enabled && this->hevcAllowed; }

    // True when HEVC 10-bit (Profile 2 / Main 10) is requested.
    // Default true: AMD VCN 2.2 fully supports 10-bit HEVC decode via AMDRadeonVADriver2.
    // Can be disabled with -NRedNoHEVC10 if explicitly desired.
    bool isHEVC10Allowed() const { return this->enabled && this->hevcAllowed && this->hevc10Allowed; }
};
