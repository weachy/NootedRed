#pragma once

namespace UserCacheSectionData
{
    struct Target
    {
        const char*        path;
        unsigned long long rva;
        const char*        segment;
        const char*        section;
        unsigned long long offset;
        unsigned           command;
        unsigned           index;
    };
    inline constexpr Target Targets[1] = {{nullptr, 0, nullptr, nullptr, 0, 0, 0}};
}    // namespace UserCacheSectionData
