// HDMI / DisplayPort Audio Topology & Engine
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.

#include <DeviceProfile.hpp>
#include <GPUDriversAMD/RavenIPOffset.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOService.h>
#include <NRed.hpp>
#include <NRedAudio.hpp>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>

namespace
{

    // DCN 2.1 (Renoir / Cezanne / Barcelo) verified audio register offsets relative to dcnBase / dccgBase.
    constexpr UInt32 kDcn21DccgBase              = 0x00C0;
    constexpr UInt32 kDcn21DccgAudioDtoSource    = 0x00AB;
    constexpr UInt32 kDcn21DccgAudioDto0Phase    = 0x00AC;
    constexpr UInt32 kDcn21DccgAudioDto0Module   = 0x00AD;

    constexpr UInt32 kDcn21OtgControl            = 0x1B41;
    constexpr UInt32 kDcn10OtgMasterEn           = 0x1B5F;
    constexpr UInt32 kDcn21OtgMasterEn           = 0x1B5C;

    constexpr UInt32 kDcn21AfmtPacketCtl2        = 0x207C;    // DIG0_AFMT_AUDIO_PACKET_CONTROL2, stride 0x100
    constexpr UInt32 kDcn21AfmtStatus            = 0x20A9;
    constexpr UInt32 kDcn21AfmtPacketCtl         = 0x20AA;
    constexpr UInt32 kDcn21AfmtInfoFrame0        = 0x20AC;    // DIG0_AFMT_INFOFRAME_CONTROL0, stride 0x100
    constexpr UInt32 kDcn21AfmtSrcCtl            = 0x20AD;
    constexpr UInt32 kDcn21DigBeCntl             = 0x20B0;
    constexpr UInt32 kDcn21AfmtCntl              = 0x20E6;

    constexpr UInt32 kDcn21DpSecCntl             = 0x212B;    // DP0_DP_SEC_CNTL, stride 0x100
    constexpr UInt32 kDcn21DpSecAudN             = 0x2131;    // DP0_DP_SEC_AUD_N, stride 0x100
    constexpr UInt32 kDcn21DpSecTimestamp        = 0x2135;    // DP0_DP_SEC_TIMESTAMP, stride 0x100
    constexpr UInt32 kDpSecAudioEnableMask       = 0x00001111U;    // STREAM | ASP | ATP | AIP

    constexpr UInt32 kDcn21AzEpIndex0            = 0x0386;
    constexpr UInt32 kDcn21AzEpStride            = 6;
    constexpr UInt32 kDcn21AzRegConverterFormat  = 0x02;
    constexpr UInt32 kDcn21AzRegSpeaker          = 0x25;
    constexpr UInt32 kDcn21AzRegHotPlug          = 0x54;

    UInt32 readPropU32(IOService* const svc, const char* const key)
    {
        if (svc == nullptr || key == nullptr) { return 0; }
        auto* const obj = svc->getProperty(key);
        if (auto* const num = OSDynamicCast(OSNumber, obj)) { return num->unsigned32BitValue(); }
        if (auto* const data = OSDynamicCast(OSData, obj)) {
            if (data->getLength() >= sizeof(UInt32)) {
                return *static_cast<const UInt32*>(data->getBytesNoCopy());
            }
        }
        return 0;
    }

    UInt32 readAzaliaEndpoint(const NRed& nred, const UInt32 dcnBase, const UInt32 ep, const UInt32 regIndex)
    {
        const UInt32 base = dcnBase + kDcn21AzEpIndex0 + (ep * kDcn21AzEpStride);
        nred.writeReg32(base, regIndex);
        return nred.readReg32(base + 1);
    }

    void writeAzaliaEndpoint(const NRed& nred, const UInt32 dcnBase, const UInt32 ep, const UInt32 regIndex,
                             const UInt32 value)
    {
        const UInt32 base = dcnBase + kDcn21AzEpIndex0 + (ep * kDcn21AzEpStride);
        nred.writeReg32(base, regIndex);
        nred.writeReg32(base + 1, value);
    }

    constexpr UInt32 kConnectorLVDS         = 2;
    constexpr UInt32 kConnectorDisplayPort  = 1024;
    constexpr UInt32 kConnectorHDMI         = 2048;
    constexpr UInt32 kDtoPhase24MHz         = 240000;
    constexpr UInt32 kAzAudioEnabledBit     = 31;
    constexpr UInt32 kAzClockGatingBit      = 0;
    constexpr UInt32 kDcn21AzRegDescriptor0 = 0x28;
    constexpr UInt32 kAzDescCount           = 4;

    void programAudioDto(const NRed& nred, const NRedAudio::DcnIpBlock& blk, const UInt32 otgIdx,
                         const UInt32 pixelClockHz)
    {
        if (pixelClockHz < 25000000U || pixelClockHz > 1200000000U) { return; }

        const UInt32 srcReg = blk.dccgBase + kDcn21DccgAudioDtoSource;
        UInt32       src    = nred.readReg32(srcReg);
        src &= ~0x00000007U;          // DTO0_SOURCE_SEL = otgIdx
        src |= (otgIdx & 0x7U);
        src &= ~0x00000030U;          // DTO_SEL = 0 (DTO0)
        src &= ~0x01000000U;          // DTO0_USE_512FBR = 0
        nred.writeReg32(srcReg, src);

        nred.writeReg32(blk.dccgBase + kDcn21DccgAudioDto0Module, pixelClockHz / 100U);
        nred.writeReg32(blk.dccgBase + kDcn21DccgAudioDto0Phase, kDtoPhase24MHz);
    }

    void configureAzaliaEndpoint(const NRed& nred, const NRedAudio::DcnIpBlock& blk, const UInt32 ep,
                                 const bool isDP)
    {
        const UInt32 hot = readAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegHotPlug);
        if (hot == 0xFFFFFFFFU) { return; }

        const UInt32 spkCur    = readAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegSpeaker);
        const bool   isEnabled = ((hot >> kAzAudioEnabledBit) & 1U) != 0;
        const bool   hasConn   = isDP ? ((spkCur & 0x00020000U) != 0 && (spkCur & 0x00010000U) == 0)
                                      : ((spkCur & 0x00010000U) != 0 && (spkCur & 0x00020000U) == 0);
        if (isEnabled && hasConn && (spkCur & 0x7U) == 0x3U) { return; }

        UInt32 gated = hot | (1U << kAzClockGatingBit);
        writeAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegHotPlug, gated);

        UInt32 spk = spkCur;
        spk &= ~0x0000007FU;
        spk |= 0x00000003U;             // SPEAKER_ALLOCATION: FL | FR
        spk &= ~0x0000FF00U;
        spk |= (0x00000003U << 8);      // CHANNEL_ALLOCATION: FL | FR
        if (isDP) {
            spk |= 0x00020000U;         // DP_CONNECTION = 1
            spk &= ~0x00010000U;        // HDMI_CONNECTION = 0
        } else {
            spk |= 0x00010000U;         // HDMI_CONNECTION = 1
            spk &= ~0x00020000U;        // DP_CONNECTION = 0
        }
        writeAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegSpeaker, spk);

        // LPCM, 2ch, 32/44.1/48 kHz, 16/20/24-bit
        const UInt32 desc = 1U | (0x07U << 8) | (0x07U << 16) | (0x07U << 24);
        for (UInt32 i = 0; i < kAzDescCount; i++) {
            writeAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegDescriptor0 + i, i == 0 ? desc : 0);
        }

        gated |= (1U << kAzAudioEnabledBit);
        writeAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegHotPlug, gated);
        gated &= ~(1U << kAzClockGatingBit);
        writeAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegHotPlug, gated);
    }

    void configureAfmt(const NRed& nred, const NRedAudio::DcnIpBlock& blk, const UInt32 dig, const UInt32 ep)
    {
        const UInt32 off    = dig * blk.digStride;
        const UInt32 srcCur = nred.readReg32(blk.dcnBase + kDcn21AfmtSrcCtl + off);
        const UInt32 srcNew = (srcCur & ~0x00000007U) | (ep & 0x7U);
        if (srcNew != srcCur) { nred.writeReg32(blk.dcnBase + kDcn21AfmtSrcCtl + off, srcNew); }

        // Enable 2ch (FL|FR = 0x3) in AFMT_AUDIO_PACKET_CONTROL2.AFMT_AUDIO_CHANNEL_ENABLE (bits 15:8)
        const UInt32 pkt2Cur = nred.readReg32(blk.dcnBase + kDcn21AfmtPacketCtl2 + off);
        if ((pkt2Cur & 0x0000FF00U) == 0) {
            nred.writeReg32(blk.dcnBase + kDcn21AfmtPacketCtl2 + off, (pkt2Cur & ~0x0000FF00U) | 0x00000300U);
        }

        const UInt32 cntlCur = nred.readReg32(blk.dcnBase + kDcn21AfmtCntl + off);
        if ((cntlCur & 0x1U) == 0) { nred.writeReg32(blk.dcnBase + kDcn21AfmtCntl + off, cntlCur | 0x1U); }

        const UInt32 pktCur = nred.readReg32(blk.dcnBase + kDcn21AfmtPacketCtl + off);
        if ((pktCur & 0x1U) == 0 || (pktCur & 0x04000000U) == 0) {
            UInt32 pkt = pktCur | 0x04000000U;    // AFMT_60958_CS_UPDATE
            if ((pktCur & 0x1U) == 0) {
                pkt |= 0x00800000U | 0x00000800U;
                nred.writeReg32(blk.dcnBase + kDcn21AfmtPacketCtl + off, pkt);
                pkt |= 0x1U;
            }
            nred.writeReg32(blk.dcnBase + kDcn21AfmtPacketCtl + off, pkt);
        }

        const UInt32 infoCur = nred.readReg32(blk.dcnBase + kDcn21AfmtInfoFrame0 + off);
        if ((infoCur & 0x00000080U) == 0) {
            nred.writeReg32(blk.dcnBase + kDcn21AfmtInfoFrame0 + off, infoCur | 0x00000080U);
        }
    }

    void configureDpSec(const NRed& nred, const NRedAudio::DcnIpBlock& blk, const UInt32 dig)
    {
        const UInt32 off     = dig * blk.digStride;
        const UInt32 audNCur = nred.readReg32(blk.dcnBase + kDcn21DpSecAudN + off);
        if ((audNCur & 0x00FFFFFFU) == 0) {
            nred.writeReg32(blk.dcnBase + kDcn21DpSecAudN + off, (audNCur & ~0x00FFFFFFU) | 0x00008000U);
        }

        const UInt32 tsCur = nred.readReg32(blk.dcnBase + kDcn21DpSecTimestamp + off);
        if ((tsCur & 0x1U) == 0) { nred.writeReg32(blk.dcnBase + kDcn21DpSecTimestamp + off, tsCur | 0x1U); }

        UInt32 secCur = nred.readReg32(blk.dcnBase + kDcn21DpSecCntl + off);
        if ((secCur & kDpSecAudioEnableMask) != kDpSecAudioEnableMask) {
            secCur |= 0x00000010U;    // DP_SEC_ASP_ENABLE
            nred.writeReg32(blk.dcnBase + kDcn21DpSecCntl + off, secCur);
            secCur |= 0x00001100U;    // DP_SEC_ATP_ENABLE | DP_SEC_AIP_ENABLE
            nred.writeReg32(blk.dcnBase + kDcn21DpSecCntl + off, secCur);
            secCur |= 0x00000001U;    // DP_SEC_STREAM_ENABLE last
            nred.writeReg32(blk.dcnBase + kDcn21DpSecCntl + off, secCur);
        }
    }

}    // namespace

static NRedAudio moduleInstance;

NRedAudio& NRedAudio::singleton() { return moduleInstance; }

auto NRedAudio::resolveDcnIpBlock() -> DcnIpBlock
{
    DcnIpBlock blk;
    const auto* const profile = DeviceProfile::current();
    if (profile == nullptr) { return blk; }

    switch (profile->family) {
        case DeviceProfile::kRaven:
        case DeviceProfile::kRaven2:
        case DeviceProfile::kPicasso:
            blk.gen               = DcnGen::kDcn10;
            blk.dccgBase          = kDcn21DccgBase;
            blk.dcnBase           = DCN_BASE_2;
            blk.otgMasterEnOffset = kDcn10OtgMasterEn;
            blk.maxDig            = 4;
            break;
        case DeviceProfile::kRenoir:
        case DeviceProfile::kRenoirE:
        case DeviceProfile::kGreenSardine:
        case DeviceProfile::kCezanne:
        case DeviceProfile::kBarcelo:
            blk.gen               = DcnGen::kDcn21;
            blk.dccgBase          = kDcn21DccgBase;
            blk.dcnBase           = DCN_BASE_2;
            blk.otgMasterEnOffset = kDcn21OtgMasterEn;
            blk.maxDig            = 5;
            break;
        default:
            break;
    }
    return blk;
}

void NRedAudio::delayedProbeCallback(thread_call_param_t, thread_call_param_t)
{
    static constexpr UInt32 kRetryDelaysMs[] = {1000, 1000, 1500, 1500};
    auto&                   audio            = singleton();
    const bool              settled          = audio.configureAudio(audio.postLinkPassDone);
    audio.probeTopology();
    if (!settled && audio.settleRetries < (sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]))) {
        const UInt32 delayMs = kRetryDelaysMs[audio.settleRetries++];
        audio.scheduleDelayedProbe(delayMs);
    } else if (settled && !audio.postLinkPassDone) {
        // One post-link-training confirmation pass 2s later to re-latch DP_SEC after mode-set settles.
        audio.postLinkPassDone = true;
        audio.scheduleDelayedProbe(2000);
    }
}

void NRedAudio::scheduleDelayedProbe(const UInt32 delayMs)
{
    if (this->delayedProbeCall == nullptr) {
        this->delayedProbeCall = thread_call_allocate(delayedProbeCallback, nullptr);
    }
    if (this->delayedProbeCall != nullptr) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(this->delayedProbeCall, deadline);
    }
}

void NRedAudio::onControllerPowerUp()
{
    this->settleRetries    = 0;
    this->postLinkPassDone = false;
    this->probeTopology();
    this->scheduleDelayedProbe(2000);
}

void NRedAudio::onLinkCapabilitiesReported()
{
    this->settleRetries    = 0;
    this->postLinkPassDone = false;
    this->probeTopology();
    this->scheduleDelayedProbe(2000);
}

bool NRedAudio::configureAudio(const bool pulseDpSec)
{
    const auto& nred = NRed::singleton();
    const auto  blk  = resolveDcnIpBlock();
    if (blk.gen == DcnGen::kUnknown || !nred.isMmioReady()) { return true; }

    // Discover active external IOFramebuffer (DP or HDMI) and whether an internal panel is present.
    bool   hasInternalPanel = false;
    bool   haveExtConn      = false;
    UInt32 extConnType      = 0;
    UInt32 extPixClk        = 0;
    UInt32 extEp            = 0;
    bool   haveExtFb        = false;

    if (auto* const match = IOService::serviceMatching("IOFramebuffer")) {
        if (auto* const iter = IOService::getMatchingServices(match)) {
            while (auto* const fb = OSDynamicCast(IOService, iter->getNextObject())) {
                const UInt32 connType = readPropU32(fb, "connector-type");
                const UInt32 pixClk   = readPropU32(fb, "IOFBCurrentPixelClock");
                if (connType == kConnectorLVDS) {
                    hasInternalPanel = true;
                    continue;
                }
                if (connType == kConnectorDisplayPort || connType == kConnectorHDMI) {
                    haveExtConn = true;
                }
                if ((connType == kConnectorDisplayPort || connType == kConnectorHDMI) && pixClk != 0 && !haveExtFb) {
                    UInt32 ep = 0;
                    if (auto* const codecData = OSDynamicCast(OSData, fb->getProperty("audio-codec-info"))) {
                        if (codecData->getLength() == 4) {
                            const auto* const b = static_cast<const UInt8*>(codecData->getBytesNoCopy());
                            if (b != nullptr && b[1] == 0x01 && b[2] >= 3) {
                                ep = static_cast<UInt32>((b[2] - 3U) / 2U);
                            }
                        }
                    }
                    extConnType = connType;
                    extPixClk   = pixClk;
                    extEp       = ep;
                    haveExtFb   = true;
                }
            }
            iter->release();
        }
    }

    if (!haveExtFb) { return !haveExtConn; }
    if (extEp >= blk.maxAzEp) { return true; }

    // Find the first enabled external DIG (skip DIG0 when internal panel is active).
    UInt32 extDig = 0xFFFFFFFFU;
    for (UInt32 dig = 0; dig < blk.maxDig; dig++) {
        if (hasInternalPanel && dig == 0) { continue; }
        const UInt32 digBe = nred.readReg32(blk.dcnBase + kDcn21DigBeCntl + (dig * blk.digStride));
        if ((digBe & 0x1U) != 0) {
            extDig = dig;
            break;
        }
    }
    if (extDig == 0xFFFFFFFFU) { return false; }

    // Find the first enabled external OTG (skip OTG0 when internal panel is active).
    UInt32 extOtg = 0;
    for (UInt32 otg = 0; otg < blk.maxOtg; otg++) {
        if (hasInternalPanel && otg == 0) { continue; }
        if ((nred.readReg32(blk.dcnBase + blk.otgMasterEnOffset + (otg * blk.otgStride)) & 1U) != 0) {
            extOtg = otg;
            break;
        }
    }

    // If AFMT is already enabled and routing the right endpoint (e.g. working HDMI), leave DTO/AFMT intact.
    const UInt32 digOff     = extDig * blk.digStride;
    const UInt32 afmtStat   = nred.readReg32(blk.dcnBase + kDcn21AfmtStatus + digOff);
    const UInt32 afmtSrc    = nred.readReg32(blk.dcnBase + kDcn21AfmtSrcCtl + digOff) & 0x7U;
    const bool   afmtActive = (afmtStat & 0x10U) != 0 && (afmtSrc == extEp);

    const bool isDP = (extConnType == kConnectorDisplayPort);
    configureAzaliaEndpoint(nred, blk, extEp, isDP);

    if (!afmtActive) {
        programAudioDto(nred, blk, extOtg, extPixClk);
        configureAfmt(nred, blk, extDig, extEp);
    } else if (isDP && (nred.readReg32(blk.dcnBase + kDcn21AfmtPacketCtl2 + digOff) & 0x0000FF00U) == 0) {
        configureAfmt(nred, blk, extDig, extEp);
    }

    if (isDP) {
        if (pulseDpSec && (afmtStat & 0x10U) == 0) {
            const UInt32 infoReg = blk.dcnBase + kDcn21AfmtInfoFrame0 + digOff;
            nred.writeReg32(infoReg, nred.readReg32(infoReg) | 0x00000080U);
            const UInt32 secReg = blk.dcnBase + kDcn21DpSecCntl + digOff;
            nred.writeReg32(secReg, nred.readReg32(secReg) & ~0x00001110U);
        }
        configureDpSec(nred, blk, extDig);
    }

    nred.setProp32("NRedAudioCfgDIG", extDig);
    nred.setProp32("NRedAudioCfgOTG", extOtg);
    nred.setProp32("NRedAudioCfgEP", extEp);
    return true;
}

void NRedAudio::probeTopology()
{
    const auto& nred  = NRed::singleton();
    const auto  blk   = resolveDcnIpBlock();
    const bool  debug = checkKernelArgument("-NRedDebug");

    nred.setProp32("NRedAudioDcnGen", static_cast<UInt32>(blk.gen));
    if (!debug) { return; }

    // Verbose diagnostics (enabled only with -NRedDebug)
    // 1. Read-only IOKit IOFramebuffer enumeration
    UInt32 fbCount = 0;
    if (auto* const match = IOService::serviceMatching("IOFramebuffer")) {
        if (auto* const iter = IOService::getMatchingServices(match)) {
            char key[48];
            while (auto* const fb = OSDynamicCast(IOService, iter->getNextObject())) {
                if (fbCount < 6) {
                    const UInt32 connType = readPropU32(fb, "connector-type");
                    const UInt32 pixClk   = readPropU32(fb, "IOFBCurrentPixelClock");
                    UInt32       codecPin = 0;
                    if (auto* const codecData = OSDynamicCast(OSData, fb->getProperty("audio-codec-info"))) {
                        if (codecData->getLength() == 4) {
                            const auto* const b = static_cast<const UInt8*>(codecData->getBytesNoCopy());
                            if (b != nullptr) {
                                codecPin = (static_cast<UInt32>(b[0]) << 24) | (static_cast<UInt32>(b[1]) << 16) |
                                           (static_cast<UInt32>(b[2]) << 8) | static_cast<UInt32>(b[3]);
                            }
                        }
                    }

                    snprintf(key, sizeof(key), "NRedAudioFB%uConn", fbCount);
                    nred.setProp32(key, connType);
                    snprintf(key, sizeof(key), "NRedAudioFB%uClk", fbCount);
                    nred.setProp32(key, pixClk);
                    snprintf(key, sizeof(key), "NRedAudioFB%uCodec", fbCount);
                    nred.setProp32(key, codecPin);
                }
                fbCount++;
            }
            iter->release();
        }
    }
    nred.setProp32("NRedAudioFBCount", fbCount);

    // 2. Hardware DCN register snapshot (DCN 1.0 / DCN 2.1)
    if (blk.gen == DcnGen::kUnknown || !nred.isMmioReady()) { return; }

    nred.setProp32("NRedAudioDTOSrc", nred.readReg32(blk.dccgBase + kDcn21DccgAudioDtoSource));
    nred.setProp32("NRedAudioDTOPhase", nred.readReg32(blk.dccgBase + kDcn21DccgAudioDto0Phase));
    nred.setProp32("NRedAudioDTOMod", nred.readReg32(blk.dccgBase + kDcn21DccgAudioDto0Module));

    UInt32 otgCtlMask = 0;
    UInt32 otgMenMask = 0;
    for (UInt32 i = 0; i < blk.maxOtg; i++) {
        const UInt32 off = i * blk.otgStride;
        if ((nred.readReg32(blk.dcnBase + kDcn21OtgControl + off) & 1U) != 0) { otgCtlMask |= (1U << i); }
        if ((nred.readReg32(blk.dcnBase + blk.otgMasterEnOffset + off) & 1U) != 0) { otgMenMask |= (1U << i); }
    }
    nred.setProp32("NRedAudioOTGMask", otgCtlMask);
    nred.setProp32("NRedAudioOTGMast", otgMenMask);

    char key[48];
    for (UInt32 dig = 0; dig < blk.maxDig; dig++) {
        const UInt32 off = dig * blk.digStride;
        snprintf(key, sizeof(key), "NRedAudioDIG%uBE", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21DigBeCntl + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uStat", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtStatus + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uPkt", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtPacketCtl + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uPkt2", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtPacketCtl2 + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uInfo0", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtInfoFrame0 + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uSrc", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtSrcCtl + off));
        snprintf(key, sizeof(key), "NRedAudioDIG%uCntl", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21AfmtCntl + off));
        snprintf(key, sizeof(key), "NRedAudioDP%uSec", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21DpSecCntl + off));
        snprintf(key, sizeof(key), "NRedAudioDP%uAudN", dig);
        nred.setProp32(key, nred.readReg32(blk.dcnBase + kDcn21DpSecAudN + off));
    }

    for (UInt32 ep = 0; ep < blk.maxAzEp; ep++) {
        snprintf(key, sizeof(key), "NRedAudioAZ%uHot", ep);
        nred.setProp32(key, readAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegHotPlug));
        snprintf(key, sizeof(key), "NRedAudioAZ%uSpk", ep);
        nred.setProp32(key, readAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegSpeaker));
        snprintf(key, sizeof(key), "NRedAudioAZ%uCfmt", ep);
        nred.setProp32(key, readAzaliaEndpoint(nred, blk.dcnBase, ep, kDcn21AzRegConverterFormat));
    }
}
