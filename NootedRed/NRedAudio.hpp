// HDMI / DisplayPort Audio Topology & Engine
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.

#pragma once
#include <IOKit/IOTypes.h>
#include <kern/thread_call.h>

class NRedAudio
{
public:
    enum class DcnGen : UInt8 {
        kUnknown = 0,
        kDcn10,
        kDcn21,
    };

    struct DcnIpBlock
    {
        DcnGen gen{DcnGen::kUnknown};
        UInt32 dccgBase{0};
        UInt32 dcnBase{0};
        UInt32 otgMasterEnOffset{0};
        UInt32 digStride{0x100};
        UInt32 otgStride{0x080};
        UInt32 maxDig{5};
        UInt32 maxOtg{4};
        UInt32 maxAzEp{4};
    };

    static NRedAudio& singleton();

    // Event-driven entry points (no background polling thread).
    void onControllerPowerUp();
    void onLinkCapabilitiesReported();

    // Read-only hardware & IOKit topology snapshot published to IORegistry (`NRedAudio*`).
    void probeTopology();

    // Event-driven DCN audio configuration (DCCG DTO + Azalia endpoint + AFMT).
    // Returns false if an external framebuffer is active but its DIG backend is not yet enabled.
    bool configureAudio(bool pulseDpSec = false);

private:
    thread_call_t delayedProbeCall{nullptr};
    UInt32        settleRetries{0};
    bool          postLinkPassDone{false};

    void        scheduleDelayedProbe(UInt32 delayMs = 2000);
    static void delayedProbeCallback(thread_call_param_t param0, thread_call_param_t param1);
    static auto resolveDcnIpBlock() -> DcnIpBlock;
};
