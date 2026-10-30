// Invariant high-entropy context signatures for dynamic VDA user-space driver patching.
// Target binary: /System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2

#ifndef VDASignatures_inc
#define VDASignatures_inc

namespace
{
    // 1. IdentifyForceFamily5: VAAcceleratorInfo::identify -> classify device 0x1638 as VA family 5 (Navi10/VCN2)
    static const UInt8 kVDACtx_Identify[] = {
        0x3D, 0x60, 0x73, 0x00, 0x00, 0x75, 0x0A, 0xBB, 0x06, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00, // Wildcard jump offset
        0xBB, 0x0B, 0x00, 0x00, 0x00,
        0xE9, 0x00, 0x00, 0x00, 0x00  // Wildcard jump offset
    };
    static const UInt8 kVDAMask_Identify[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0x00, 0x00, 0x00, 0x00
    };
    static const UInt8 kVDAFind_Identify[]    = {0xBB, 0x0B, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_Identify[] = {0xBB, 0x05, 0x00, 0x00, 0x00};

    // 2. AddrLibInitVegaToRV: VAAddrLibInterface::init -> map family 0x8E (RV/Renoir) to addrlib family 0xD
    static const UInt8 kVDACtx_AddrLibInit[] = {
        0x41, 0x81, 0xFC, 0x82, 0x00, 0x00, 0x00, 0x74, 0x0E,
        0x41, 0x81, 0xFC, 0x8D, 0x00, 0x00, 0x00, 0x75, 0x09, 0xB8, 0x0D, 0x00, 0x00, 0x00
    };
    static const UInt8 kVDAMask_AddrLibInit[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_AddrLibInit[]    = {0x41, 0x81, 0xFC, 0x8D, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_AddrLibInit[] = {0x41, 0x81, 0xFC, 0x8E, 0x00, 0x00, 0x00};

    // 3. AddrLibCreateVegaToRV: Addr::Lib::Create -> accept family 0x8E and take Addr::Gfx9HwlInit
    static const UInt8 kVDACtx_AddrLibCreate[] = {
        0x74, 0x21, 0x3D, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00
    };
    static const UInt8 kVDAMask_AddrLibCreate[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
    };
    static const UInt8 kVDAFind_AddrLibCreate[]    = {0x3D, 0x8D, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_AddrLibCreate[] = {0x3D, 0x8E, 0x00, 0x00, 0x00};

    // 4a. Gfx9ConvertChipFamilyVegaToRV (Ventura): configure 0x8E via Gfx9 (26 bytes)
    static const UInt8 kVDACtx_ConvertFamily_Ventura[] = {
        0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x00, 0x01, 0x00, 0x00,
        0x4C, 0x8D, 0x8F, 0x10, 0x5B, 0x00, 0x00, 0x44, 0x8B, 0x97, 0x18, 0x5B, 0x00, 0x00
    };
    static const UInt8 kVDAMask_ConvertFamily_Ventura[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_ConvertFamily[]    = {0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_ConvertFamily[] = {0x81, 0xFE, 0x8E, 0x00, 0x00, 0x00};

    // 4b. Gfx9ConvertChipFamilyVegaToRV (Modern): configure 0x8E via Gfx9 (22 bytes)
    static const UInt8 kVDACtx_ConvertFamily_Modern[] = {
        0x81, 0xFE, 0x8D, 0x00, 0x00, 0x00, 0x0F, 0x85, 0x88, 0x00, 0x00, 0x00,
        0x55, 0x48, 0x89, 0xE5, 0x8B, 0x87, 0x18, 0x5B, 0x00, 0x00
    };
    static const UInt8 kVDAMask_ConvertFamily_Modern[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };

    // 5. ForceGfx9EngineAndBlt: VAFactory::createGraphicsEngine/createImageBlt -> force GFX9 engine
    static const UInt8 kVDACtx_EngineAndBlt[] = {
        0x48, 0x8B, 0x86, 0x60, 0x04, 0x00, 0x00, 0x8B, 0x40, 0x0C, 0x8D, 0x48, 0xFF, 0x83
    };
    static const UInt8 kVDAMask_EngineAndBlt[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_EngineAndBlt[]    = {0x48, 0x8B, 0x86, 0x60, 0x04, 0x00, 0x00, 0x8B, 0x40, 0x0C, 0x8D, 0x48, 0xFF, 0x83};
    static const UInt8 kVDAReplace_EngineAndBlt[] = {0xB8, 0x02, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x8D, 0x48, 0xFF, 0x83};

    // 6. ForceGfx9VideoProcessors: VAFactory color format/dithering/deblocker -> Gfx9 branch (23-byte high-entropy context)
    static const UInt8 kVDACtx_VideoProcessors[] = {
        0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53,
        0x8D, 0x46, 0xFF, 0x83, 0xF8, 0x02, 0x72, 0x1F,
        0x8D, 0x46, 0xFD, 0x83, 0xF8, 0x02, 0x73, 0x2E
    };
    static const UInt8 kVDAMask_VideoProcessors[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_VideoProcessors[]    = {0x8D, 0x46, 0xFF, 0x83, 0xF8, 0x02, 0x72, 0x1F};
    static const UInt8 kVDAReplace_VideoProcessors[] = {0x8D, 0x46, 0xFF, 0x83, 0xF8, 0x02, 0xEB, 0x1F};

    // 7. Vcn2EncCommandFeedbackStride: Vcn2EncCommand::getFeedbackSize -> 48-byte (0x30) stride
    static const UInt8 kVDACtx_EncFeedbackStride[] = {
        0x55, 0x48, 0x89, 0xE5, 0xB8, 0xA4, 0x00, 0x00, 0x00, 0x5D, 0xC3
    };
    static const UInt8 kVDAMask_EncFeedbackStride[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_EncFeedbackStride[]    = {0xB8, 0xA4, 0x00, 0x00, 0x00, 0x5D, 0xC3};
    static const UInt8 kVDAReplace_EncFeedbackStride[] = {0xB8, 0x30, 0x00, 0x00, 0x00, 0x5D, 0xC3};

    // 8a. ProcessEncodeStatusStride (Ventura): AVDVcn2(Avc/Hevc)EncDataProcessor::ProcessEncodeStatus (20-byte context)
    static const UInt8 kVDACtx_StatusStride_Ventura[] = {
        0x8B, 0x40, 0x08,
        0x4C, 0x69, 0xE8, 0xA4, 0x00, 0x00, 0x00,
        0x42, 0x8B, 0x44, 0x2A, 0x0C,
        0x3D, 0x01, 0x00, 0x00, 0x10
    };
    static const UInt8 kVDAMask_StatusStride_Ventura[] = {
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_StatusStride_Ventura[]    = {0x4C, 0x69, 0xE8, 0xA4, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_StatusStride_Ventura[] = {0x4C, 0x69, 0xE8, 0x30, 0x00, 0x00, 0x00};

    // 8b. ProcessEncodeStatusStride (Sonoma, Sequoia, Tahoe): imul rax, rax, 0x30 (21-byte context)
    static const UInt8 kVDACtx_StatusStride_Modern[] = {
        0x8B, 0x40, 0x08,
        0x48, 0x69, 0xC0, 0xA4, 0x00, 0x00, 0x00,
        0x4C, 0x8D, 0x3C, 0x02,
        0x8B, 0x44, 0x02, 0x0C,
        0x3D, 0x01, 0x00, 0x00, 0x10
    };
    static const UInt8 kVDAMask_StatusStride_Modern[] = {
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_StatusStride_Modern[]    = {0x48, 0x69, 0xC0, 0xA4, 0x00, 0x00, 0x00};
    static const UInt8 kVDAReplace_StatusStride_Modern[] = {0x48, 0x69, 0xC0, 0x30, 0x00, 0x00, 0x00};

    // 9a. Vcn2EncoderSourcePitch (macOS 13 Ventura, macOS 14 Sonoma):
    // Vcn2Encoder::setEncodeParams -> read true texture pitch instead of reconstructed width
    static const UInt8 kVDACtx_SourcePitch[] = {
        0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x8B, 0x97, 0x24, 0x04, 0x00, 0x00, 0x0F, 0xAF, 0xD1, 0x0F,
        0xB6, 0xFA, 0xB8, 0x00, 0x01, 0x00, 0x00, 0x29, 0xF8, 0x85, 0xFF, 0x0F, 0x44, 0xC7, 0x01, 0xD0
    };
    static const UInt8 kVDAMask_SourcePitch[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_SourcePitch[] = {
        0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x8B, 0x97, 0x24, 0x04, 0x00, 0x00, 0x0F, 0xAF, 0xD1, 0x0F,
        0xB6, 0xFA, 0xB8, 0x00, 0x01, 0x00, 0x00, 0x29, 0xF8, 0x85, 0xFF, 0x0F, 0x44, 0xC7, 0x01, 0xD0
    };
    static const UInt8 kVDAReplace_SourcePitch[] = {
        0x8B, 0x82, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    // 9b. Vcn2EncoderSourcePitch (macOS 15 Sequoia, macOS 26 Tahoe):
    // Vcn2Encoder::setEncodeParams -> read true texture pitch instead of 256-byte aligned width (25 bytes)
    static const UInt8 kVDACtx_SourcePitch_Modern[] = {
        0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x8B, 0x87, 0x24, 0x04, 0x00, 0x00, 0x0F, 0xAF, 0xC1, 0x05,
        0xFF, 0x00, 0x00, 0x00, 0x25, 0x00, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAMask_SourcePitch_Modern[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAFind_SourcePitch_Modern[] = {
        0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x8B, 0x87, 0x24, 0x04, 0x00, 0x00, 0x0F, 0xAF, 0xC1, 0x05,
        0xFF, 0x00, 0x00, 0x00, 0x25, 0x00, 0xFF, 0xFF, 0xFF
    };
    static const UInt8 kVDAReplace_SourcePitch_Modern[] = {
        0x8B, 0x82, 0x80, 0x00, 0x00, 0x00, 0x8B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    // Resolver table for macOS 13 (Ventura)
    static const UserPatches::SignaturePatch kVDAResolverSignatures_Ventura[] = {
        {kVDACtx_Identify, kVDAMask_Identify, sizeof(kVDACtx_Identify), 17,
         kVDAReplace_Identify, sizeof(kVDAReplace_Identify), 0, 1, ~0ULL,
         kVDAFind_Identify, sizeof(kVDAFind_Identify)},
        {kVDACtx_AddrLibInit, kVDAMask_AddrLibInit, sizeof(kVDACtx_AddrLibInit), 9,
         kVDAReplace_AddrLibInit, sizeof(kVDAReplace_AddrLibInit), 0, 1, ~0ULL,
         kVDAFind_AddrLibInit, sizeof(kVDAFind_AddrLibInit)},
        {kVDACtx_AddrLibCreate, kVDAMask_AddrLibCreate, sizeof(kVDACtx_AddrLibCreate), 2,
         kVDAReplace_AddrLibCreate, sizeof(kVDAReplace_AddrLibCreate), 0, 1, ~0ULL,
         kVDAFind_AddrLibCreate, sizeof(kVDAFind_AddrLibCreate)},
        {kVDACtx_ConvertFamily_Ventura, kVDAMask_ConvertFamily_Ventura, sizeof(kVDACtx_ConvertFamily_Ventura), 0,
         kVDAReplace_ConvertFamily, sizeof(kVDAReplace_ConvertFamily), 0, 1, ~0ULL,
         kVDAFind_ConvertFamily, sizeof(kVDAFind_ConvertFamily)},
        {kVDACtx_EngineAndBlt, kVDAMask_EngineAndBlt, sizeof(kVDACtx_EngineAndBlt), 0,
         kVDAReplace_EngineAndBlt, sizeof(kVDAReplace_EngineAndBlt), 0, 2, ~0ULL,
         kVDAFind_EngineAndBlt, sizeof(kVDAFind_EngineAndBlt)},
        {kVDACtx_VideoProcessors, kVDAMask_VideoProcessors, sizeof(kVDACtx_VideoProcessors), 7,
         kVDAReplace_VideoProcessors, sizeof(kVDAReplace_VideoProcessors), 0, 3, ~0ULL,
         kVDAFind_VideoProcessors, sizeof(kVDAFind_VideoProcessors)},
        {kVDACtx_EncFeedbackStride, kVDAMask_EncFeedbackStride, sizeof(kVDACtx_EncFeedbackStride), 4,
         kVDAReplace_EncFeedbackStride, sizeof(kVDAReplace_EncFeedbackStride), 0, 1, ~0ULL,
         kVDAFind_EncFeedbackStride, sizeof(kVDAFind_EncFeedbackStride)},
        {kVDACtx_StatusStride_Ventura, kVDAMask_StatusStride_Ventura, sizeof(kVDACtx_StatusStride_Ventura), 3,
         kVDAReplace_StatusStride_Ventura, sizeof(kVDAReplace_StatusStride_Ventura), 0, 2, ~0ULL,
         kVDAFind_StatusStride_Ventura, sizeof(kVDAFind_StatusStride_Ventura)},
        {kVDACtx_SourcePitch, kVDAMask_SourcePitch, sizeof(kVDACtx_SourcePitch), 0,
         kVDAReplace_SourcePitch, sizeof(kVDAReplace_SourcePitch), 0, 1, ~0ULL,
         kVDAFind_SourcePitch, sizeof(kVDAFind_SourcePitch)},
    };
    constexpr size_t kVDAResolverSignatureCount_Ventura = arrsize(kVDAResolverSignatures_Ventura);

    // Resolver table for macOS 14 (Sonoma)
    static const UserPatches::SignaturePatch kVDAResolverSignatures_Sonoma[] = {
        {kVDACtx_Identify, kVDAMask_Identify, sizeof(kVDACtx_Identify), 17,
         kVDAReplace_Identify, sizeof(kVDAReplace_Identify), 0, 1, ~0ULL,
         kVDAFind_Identify, sizeof(kVDAFind_Identify)},
        {kVDACtx_AddrLibInit, kVDAMask_AddrLibInit, sizeof(kVDACtx_AddrLibInit), 9,
         kVDAReplace_AddrLibInit, sizeof(kVDAReplace_AddrLibInit), 0, 1, ~0ULL,
         kVDAFind_AddrLibInit, sizeof(kVDAFind_AddrLibInit)},
        {kVDACtx_AddrLibCreate, kVDAMask_AddrLibCreate, sizeof(kVDACtx_AddrLibCreate), 2,
         kVDAReplace_AddrLibCreate, sizeof(kVDAReplace_AddrLibCreate), 0, 1, ~0ULL,
         kVDAFind_AddrLibCreate, sizeof(kVDAFind_AddrLibCreate)},
        {kVDACtx_ConvertFamily_Modern, kVDAMask_ConvertFamily_Modern, sizeof(kVDACtx_ConvertFamily_Modern), 0,
         kVDAReplace_ConvertFamily, sizeof(kVDAReplace_ConvertFamily), 0, 1, ~0ULL,
         kVDAFind_ConvertFamily, sizeof(kVDAFind_ConvertFamily)},
        {kVDACtx_ConvertFamily_Ventura, kVDAMask_ConvertFamily_Ventura, sizeof(kVDACtx_ConvertFamily_Ventura), 0,
         kVDAReplace_ConvertFamily, sizeof(kVDAReplace_ConvertFamily), 0, 1, ~0ULL,
         kVDAFind_ConvertFamily, sizeof(kVDAFind_ConvertFamily)},
        {kVDACtx_EngineAndBlt, kVDAMask_EngineAndBlt, sizeof(kVDACtx_EngineAndBlt), 0,
         kVDAReplace_EngineAndBlt, sizeof(kVDAReplace_EngineAndBlt), 0, 2, ~0ULL,
         kVDAFind_EngineAndBlt, sizeof(kVDAFind_EngineAndBlt)},
        {kVDACtx_VideoProcessors, kVDAMask_VideoProcessors, sizeof(kVDACtx_VideoProcessors), 7,
         kVDAReplace_VideoProcessors, sizeof(kVDAReplace_VideoProcessors), 0, 3, ~0ULL,
         kVDAFind_VideoProcessors, sizeof(kVDAFind_VideoProcessors)},
        {kVDACtx_EncFeedbackStride, kVDAMask_EncFeedbackStride, sizeof(kVDACtx_EncFeedbackStride), 4,
         kVDAReplace_EncFeedbackStride, sizeof(kVDAReplace_EncFeedbackStride), 0, 1, ~0ULL,
         kVDAFind_EncFeedbackStride, sizeof(kVDAFind_EncFeedbackStride)},
        {kVDACtx_StatusStride_Modern, kVDAMask_StatusStride_Modern, sizeof(kVDACtx_StatusStride_Modern), 3,
         kVDAReplace_StatusStride_Modern, sizeof(kVDAReplace_StatusStride_Modern), 0, 2, ~0ULL,
         kVDAFind_StatusStride_Modern, sizeof(kVDAFind_StatusStride_Modern)},
        {kVDACtx_SourcePitch, kVDAMask_SourcePitch, sizeof(kVDACtx_SourcePitch), 0,
         kVDAReplace_SourcePitch, sizeof(kVDAReplace_SourcePitch), 0, 1, ~0ULL,
         kVDAFind_SourcePitch, sizeof(kVDAFind_SourcePitch)},
    };
    constexpr size_t kVDAResolverSignatureCount_Sonoma = arrsize(kVDAResolverSignatures_Sonoma);

    // Resolver table for modern macOS (15 Sequoia, 26 Tahoe)
    static const UserPatches::SignaturePatch kVDAResolverSignatures_Modern[] = {
        {kVDACtx_Identify, kVDAMask_Identify, sizeof(kVDACtx_Identify), 17,
         kVDAReplace_Identify, sizeof(kVDAReplace_Identify), 0, 1, ~0ULL,
         kVDAFind_Identify, sizeof(kVDAFind_Identify)},
        {kVDACtx_AddrLibInit, kVDAMask_AddrLibInit, sizeof(kVDACtx_AddrLibInit), 9,
         kVDAReplace_AddrLibInit, sizeof(kVDAReplace_AddrLibInit), 0, 1, ~0ULL,
         kVDAFind_AddrLibInit, sizeof(kVDAFind_AddrLibInit)},
        {kVDACtx_AddrLibCreate, kVDAMask_AddrLibCreate, sizeof(kVDACtx_AddrLibCreate), 2,
         kVDAReplace_AddrLibCreate, sizeof(kVDAReplace_AddrLibCreate), 0, 1, ~0ULL,
         kVDAFind_AddrLibCreate, sizeof(kVDAFind_AddrLibCreate)},
        {kVDACtx_ConvertFamily_Modern, kVDAMask_ConvertFamily_Modern, sizeof(kVDACtx_ConvertFamily_Modern), 0,
         kVDAReplace_ConvertFamily, sizeof(kVDAReplace_ConvertFamily), 0, 1, ~0ULL,
         kVDAFind_ConvertFamily, sizeof(kVDAFind_ConvertFamily)},
        {kVDACtx_ConvertFamily_Ventura, kVDAMask_ConvertFamily_Ventura, sizeof(kVDACtx_ConvertFamily_Ventura), 0,
         kVDAReplace_ConvertFamily, sizeof(kVDAReplace_ConvertFamily), 0, 1, ~0ULL,
         kVDAFind_ConvertFamily, sizeof(kVDAFind_ConvertFamily)},
        {kVDACtx_EngineAndBlt, kVDAMask_EngineAndBlt, sizeof(kVDACtx_EngineAndBlt), 0,
         kVDAReplace_EngineAndBlt, sizeof(kVDAReplace_EngineAndBlt), 0, 2, ~0ULL,
         kVDAFind_EngineAndBlt, sizeof(kVDAFind_EngineAndBlt)},
        {kVDACtx_VideoProcessors, kVDAMask_VideoProcessors, sizeof(kVDACtx_VideoProcessors), 7,
         kVDAReplace_VideoProcessors, sizeof(kVDAReplace_VideoProcessors), 0, 3, ~0ULL,
         kVDAFind_VideoProcessors, sizeof(kVDAFind_VideoProcessors)},
        {kVDACtx_EncFeedbackStride, kVDAMask_EncFeedbackStride, sizeof(kVDACtx_EncFeedbackStride), 4,
         kVDAReplace_EncFeedbackStride, sizeof(kVDAReplace_EncFeedbackStride), 0, 1, ~0ULL,
         kVDAFind_EncFeedbackStride, sizeof(kVDAFind_EncFeedbackStride)},
        {kVDACtx_StatusStride_Modern, kVDAMask_StatusStride_Modern, sizeof(kVDACtx_StatusStride_Modern), 3,
         kVDAReplace_StatusStride_Modern, sizeof(kVDAReplace_StatusStride_Modern), 0, 2, ~0ULL,
         kVDAFind_StatusStride_Modern, sizeof(kVDAFind_StatusStride_Modern)},
        {kVDACtx_SourcePitch_Modern, kVDAMask_SourcePitch_Modern, sizeof(kVDACtx_SourcePitch_Modern), 0,
         kVDAReplace_SourcePitch_Modern, sizeof(kVDAReplace_SourcePitch_Modern), 0, 1, ~0ULL,
         kVDAFind_SourcePitch_Modern, sizeof(kVDAFind_SourcePitch_Modern)},
    };
    constexpr size_t kVDAResolverSignatureCount_Modern = arrsize(kVDAResolverSignatures_Modern);

    // -------------------------------------------------------------------------
    // AppleGVA: AVDGetGPUAccelerators -> branch redirect to force AMD accelerator
    // -------------------------------------------------------------------------

    // macOS 13 Ventura: `mov (%rbx), %rbx; test %rbx, %rbx; je <empty-return>`
    static const UInt8 kGVACtx_Ventura[] = {
        0x48, 0x8B, 0x1B, 0x48, 0x85, 0xDB,
        0x0F, 0x84, 0xA0, 0x00, 0x00, 0x00,
        0x48, 0x8D
    };
    static const UInt8 kGVAMask_Ventura[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF
    };
    static const UInt8 kGVAFind_Ventura[]    = {0x0F, 0x84, 0xA0, 0x00, 0x00, 0x00};
    static const UInt8 kGVAReplace_Ventura[] = {0x0F, 0x84, 0x70, 0x00, 0x00, 0x00};
    static const UserPatches::SignaturePatch kGVAResolverSignature_Ventura = {
        kGVACtx_Ventura, kGVAMask_Ventura, sizeof(kGVACtx_Ventura), 6,
        kGVAReplace_Ventura, sizeof(kGVAReplace_Ventura), 0, 1, ~0ULL,
        kGVAFind_Ventura, sizeof(kGVAFind_Ventura)
    };

    // macOS 14 Sonoma: `mov (%rdi), %r15; test %r15, %r15; je <empty-return>`
    static const UInt8 kGVACtx_Sonoma[] = {
        0x4D, 0x8B, 0x3F, 0x4D, 0x85, 0xFF,
        0x0F, 0x84, 0x99, 0x00, 0x00, 0x00,
        0x48, 0x8D
    };
    static const UInt8 kGVAMask_Sonoma[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF
    };
    static const UInt8 kGVAFind_Sonoma[]    = {0x0F, 0x84, 0x99, 0x00, 0x00, 0x00};
    static const UInt8 kGVAReplace_Sonoma[] = {0x0F, 0x84, 0x70, 0x00, 0x00, 0x00};
    static const UserPatches::SignaturePatch kGVAResolverSignature_Sonoma = {
        kGVACtx_Sonoma, kGVAMask_Sonoma, sizeof(kGVACtx_Sonoma), 6,
        kGVAReplace_Sonoma, sizeof(kGVAReplace_Sonoma), 0, 1, ~0ULL,
        kGVAFind_Sonoma, sizeof(kGVAFind_Sonoma)
    };

    // macOS 15 Sequoia: `test %r15, %r15; je <empty-return>; lea -0x158(%rbp), %rsi; mov %rbx, %rdi`
    static const UInt8 kGVACtx_Sequoia[] = {
        0x4D, 0x85, 0xFF,
        0x0F, 0x84, 0xD6, 0x03, 0x00, 0x00,
        0x48, 0x8D, 0xB5, 0xA8, 0xFE, 0xFF, 0xFF,
        0x48, 0x89, 0xDF
    };
    static const UInt8 kGVAMask_Sequoia[] = {
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF
    };
    static const UInt8 kGVAFind_Sequoia[]    = {0x0F, 0x84, 0xD6, 0x03, 0x00, 0x00};
    static const UInt8 kGVAReplace_Sequoia[] = {0x0F, 0x84, 0x2A, 0x00, 0x00, 0x00};
    static const UserPatches::SignaturePatch kGVAResolverSignature_Sequoia = {
        kGVACtx_Sequoia, kGVAMask_Sequoia, sizeof(kGVACtx_Sequoia), 3,
        kGVAReplace_Sequoia, sizeof(kGVAReplace_Sequoia), 0, 1, ~0ULL,
        kGVAFind_Sequoia, sizeof(kGVAFind_Sequoia)
    };

    // macOS 26 Tahoe: `mov -0x158(%rbp), %r12; test %r12, %r12; je <empty-return>`
    static const UInt8 kGVACtx_Tahoe[] = {
        0x4C, 0x8B, 0xA5, 0xA8, 0xFE, 0xFF, 0xFF,
        0x4D, 0x85, 0xE4,
        0x0F, 0x84, 0x8C, 0x00, 0x00, 0x00,
        0x48, 0x8D
    };
    static const UInt8 kGVAMask_Tahoe[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF
    };
    static const UInt8 kGVAFind_Tahoe[]    = {0x0F, 0x84, 0x8C, 0x00, 0x00, 0x00};
    static const UInt8 kGVAReplace_Tahoe[] = {0x0F, 0x84, 0x78, 0x00, 0x00, 0x00};
    static const UserPatches::SignaturePatch kGVAResolverSignature_Tahoe = {
        kGVACtx_Tahoe, kGVAMask_Tahoe, sizeof(kGVACtx_Tahoe), 10,
        kGVAReplace_Tahoe, sizeof(kGVAReplace_Tahoe), 0, 1, ~0ULL,
        kGVAFind_Tahoe, sizeof(kGVAFind_Tahoe)
    };
}

#endif // VDASignatures_inc
