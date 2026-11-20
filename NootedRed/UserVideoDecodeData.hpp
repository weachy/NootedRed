#pragma once
#include <IOKit/IOTypes.h>

namespace UserVideoDecodeData
{
    struct Patch
    {
        UInt64       offset;
        const UInt8* original;
        const UInt8* patched;
        UInt32       size;
    };
}    // namespace UserVideoDecodeData
