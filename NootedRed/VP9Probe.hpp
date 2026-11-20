// 仅在 VP9 后端已安装并恢复 RX 权限后发布解码能力。
// Pubblica sull'acceleratore le tre chiavi che servono (vedi docs/vp9-v0-findings.md):
//   IOGVAXDecode                  -> carica il plugin AppleGVAVPXDecoder (CMDependencies)
//   IOGVAVP9Decode                -> AppleGVA trova il codec VP9 della GPU
//   IOGVAVP9DecodeCapabilities    -> AppleGVA legge i profili (VTSupportedProfileArray)
// 后端不可用时撤回能力键，让客户端继续使用原有解码选择。
// Tutto o niente: se una chiave non si scrive, quelle già scritte vengono tolte.
#pragma once
#include <IOKit/IOTypes.h>
#include <stddef.h>

namespace VP9Probe
{

    constexpr const char* StatusKey       = "NRedVP9Probe";
    constexpr const char* PluginKey       = "IOGVAXDecode";
    constexpr const char* PluginValue     = "2";    // stesso valore dei Mac Intel che hanno il VP9
    constexpr const char* DecodeKey       = "IOGVAVP9Decode";
    constexpr const char* DecodeValue     = "1";    // come IOGVAHEVCDecode nella personality
    constexpr const char* CapabilitiesKey = "IOGVAVP9DecodeCapabilities";
    constexpr const char* ProfilesKey     = "VTSupportedProfileArray";
    constexpr UInt32      Profiles[]      = {0, 2};
    constexpr size_t      ProfileCount    = sizeof(Profiles) / sizeof(Profiles[0]);

    enum Status : UInt32
    {
        Failed      = 0,    // scrittura non riuscita, nessuna chiave lasciata
        Published   = 1,
        NotEligible = 2,    // ponte VCN non pronto o acceleratore non avviato
    };

    // Sink: bool string(key, value); bool profiles(key, arrayKey, values, count); void remove(key).
    template<typename Sink>
    Status publish(Sink& sink, bool eligible)
    {
        if (!eligible) {
            sink.remove(CapabilitiesKey);
            sink.remove(DecodeKey);
            sink.remove(PluginKey);
            return NotEligible;
        }
        if (!sink.string(PluginKey, PluginValue)) { return Failed; }
        if (!sink.string(DecodeKey, DecodeValue)) {
            sink.remove(PluginKey);
            return Failed;
        }
        if (!sink.profiles(CapabilitiesKey, ProfilesKey, Profiles, ProfileCount)) {
            sink.remove(DecodeKey);
            sink.remove(PluginKey);
            return Failed;
        }
        return Published;
    }

}    // namespace VP9Probe
