// Shared PCI-to-media-IP policy for the kernel bridge and the VP9 plugin.
// Hardware classification is independent of Apple binary compatibility and
// of hardware test coverage. Callers must check those prerequisites as well.
#pragma once

#ifdef __cplusplus
#define NRED_MEDIA_INLINE static constexpr
#else
#include <stdbool.h>
#define NRED_MEDIA_INLINE static inline
#endif

enum NRedMediaIP {
    NRedMediaIPUnknown = 0,
    NRedMediaIPVCN1 = 0x0100,
    NRedMediaIPVCN22 = 0x0202,
    NRedMediaIPVCN3 = 0x0300,
};

// IDs are AMD GPU PCI IDs, never CPU model names or the spoofed VA chip ID.
// Source: Linux v6.18 amdgpu_drv.c PCI IDs and amdgpu_ucode.c legacy VCN
// naming (Renoir/Green Sardine -> 2.2.0; Raven/Picasso -> 1.0.0/1.0.1).
// See analysis/media-portability-20261116.md for sources and test coverage.
NRED_MEDIA_INLINE enum NRedMediaIP nredMediaIP(unsigned deviceID)
{
    switch (deviceID) {
        case 0x15DD:
        case 0x15D8: return NRedMediaIPVCN1;
        case 0x1636:
        case 0x1638:
        case 0x164C:
        case 0x15E7: return NRedMediaIPVCN22;
        // Original standalone VP9 plugin targets. These are not bridge APUs.
        case 0x73BF:
        case 0x73AF: return NRedMediaIPVCN3;
        default: return NRedMediaIPUnknown;
    }
}

NRED_MEDIA_INLINE bool nredMediaUsesVCN2Bridge(unsigned deviceID)
{
    return nredMediaIP(deviceID) == NRedMediaIPVCN22;
}

NRED_MEDIA_INLINE bool nredMediaVP9Supported(unsigned deviceID)
{
    return nredMediaUsesVCN2Bridge(deviceID) || nredMediaIP(deviceID) == NRedMediaIPVCN3;
}

// Keep the original Navi tier-0 layout. VCN 2.2 needs explicit reference plane
// dimensions at the 8192x4352 corner and must not receive VCN3 write combining.
NRED_MEDIA_INLINE unsigned nredMediaVP9DPBTier(unsigned deviceID)
{
    return nredMediaUsesVCN2Bridge(deviceID) ? 1U : 0U;
}

NRED_MEDIA_INLINE unsigned nredMediaVP9WriteCombine(unsigned deviceID)
{
    return nredMediaIP(deviceID) == NRedMediaIPVCN3 ? 1U : 0U;
}

#undef NRED_MEDIA_INLINE
