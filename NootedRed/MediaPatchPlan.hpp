// Bounded, build-specific VA/GVA page validation. No filesystem IO in pager hooks.
#pragma once
#include <libkern/OSTypes.h>
#include <libkern/libkern.h>

namespace MediaPatchPlan
{
    inline constexpr size_t PageSize = 4096;
    struct Site
    {
        size_t offset;
        const UInt8* find;
        const UInt8* replace;
        size_t size;
    };
    struct Page
    {
        const char* file;
        UInt64 offset;
        UInt8 maskedSHA256[32];
        const Site* sites;
        size_t siteCount;
    };

    // The kernel Mach-O UUID exists before launchd publishes kern.osversion.
    // Only parse a bounded load-command area; never treat an empty build string
    // by itself as permission to enable media.
    inline bool matchesKernelUUID(const UInt8* header, const size_t size, const UInt8 (&expected)[16])
    {
        if (header == nullptr || size < 32) { return false; }
        UInt32 magic, count, commandBytes;
        memcpy(&magic, header, 4);
        memcpy(&count, header + 16, 4);
        memcpy(&commandBytes, header + 20, 4);
        if (magic != 0xFEEDFACF || commandBytes > 0x10000 || commandBytes > size - 32) { return false; }
        size_t offset = 32;
        const size_t end = offset + commandBytes;
        bool found = false;
        for (UInt32 i = 0; i < count; ++i) {
            if (offset > end || end - offset < 8) { return false; }
            UInt32 command, length;
            memcpy(&command, header + offset, 4);
            memcpy(&length, header + offset + 4, 4);
            if (length < 8 || length > end - offset) { return false; }
            if (command == 0x1B) { // LC_UUID
                if (found || length != 24 || memcmp(header + offset + 8, expected, 16) != 0) { return false; }
                found = true;
            }
            offset += length;
        }
        return found && offset == end;
    }

    // A mixed page (some sites already patched) is legal only when EVERY site
    // is either its exact original or exact replacement. Hash all other bytes.
    // Do not use process/global counters to suppress a newly faulted clean page.
    inline bool normalize(const UInt8* bytes, const Page& page, UInt8* normalized, bool& needsWrite)
    {
        if (bytes == nullptr || normalized == nullptr || page.sites == nullptr || page.siteCount == 0) { return false; }
        memcpy(normalized, bytes, PageSize);
        needsWrite = false;
        size_t previousEnd = 0;
        for (size_t i = 0; i < page.siteCount; ++i) {
            const auto& site = page.sites[i];
            if (site.find == nullptr || site.replace == nullptr || site.size == 0 ||
                site.offset < previousEnd || site.offset >= PageSize || site.size > PageSize - site.offset) {
                return false;
            }
            const bool original = memcmp(bytes + site.offset, site.find, site.size) == 0;
            const bool patched = memcmp(bytes + site.offset, site.replace, site.size) == 0;
            if (!original && !patched) { return false; }
            needsWrite |= original && !patched;
            previousEnd = site.offset + site.size;
            memset(normalized + site.offset, 0, site.size);
        }
        return true;
    }
}
