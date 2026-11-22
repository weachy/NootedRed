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

class IORegistryEntry;
class OSDictionary;

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
    bool vcn1DecodeRequested{false};
    bool vcn1HEVCRequested{false};
    bool vcn1HEVC10Requested{false};
    bool vcn1Decode{false};
    bool raven2AVCEncodeTest{false};
    bool raven2HEVCEncodeTest{false};

public:
    static VDA& singleton();

    // Parses -NRedNoVDA, -NRedNoHEVC, and -NRedNoHEVC10 boot arguments. Called from NRed::init().
    void init();

    // Resolves the running device profile and logs the media policy. Called from NRed::processPatcher().
    void processPatcher();

    // True when hardware video acceleration is enabled (default true, disabled by -NRedNoVDA).
    bool isRequested() const { return this->enabled; }
    bool isEnabled() const { return this->enabled; }
    bool isSupported() const;
    bool canAdvertise() const;
    bool shouldStripCapabilities() const;

    static void stripCapabilities(OSDictionary* dictionary);
    static void withdrawCapabilities(IORegistryEntry* entry);
    bool isVcn1Mode() const { return this->vcn1Decode; }
    bool isVcn1DecodeRequested() const { return this->isVcn1Mode(); }
    bool isBridgeRequested() const;
    bool isRaven2AVCEncodeTestRequested() const { return this->raven2AVCEncodeTest; }
    bool isRaven2HEVCEncodeTestRequested() const { return this->raven2HEVCEncodeTest; }

    // True when only AVC is enabled and HEVC is disabled (-NRedNoHEVC).
    bool isAVCOnly() const { return this->enabled && !this->hevcAllowed; }

    // VCN1 HEVC Main encode requires the Raven2 packet translation.
    bool isHEVCEncodeAllowed() const
    {
        return this->enabled && this->hevcAllowed
            && (!this->isVcn1DecodeRequested() || this->raven2HEVCEncodeTest);
    }

    // True when HEVC hardware decode (8-bit) is allowed (default true, disabled by -NRedNoHEVC or -NRedNoVDA).
    bool isHEVCDecodeAllowed() const { return this->enabled && this->hevcAllowed; }

    // True when HEVC 10-bit (Profile 2 / Main 10) hardware decode is allowed.
    // Raven2 requires -NRedRaven2HEVC10 in addition to its decode and HEVC opt-ins.
    // -NRedNoHEVC10 disables Main 10 on all devices.
    bool isHEVC10Allowed() const { return this->enabled && this->hevcAllowed && this->hevc10Allowed; }
    bool isHEVC10EncodeAllowed() const;
    bool isHEVC8KDecodeAllowed() const;
};
