#pragma once

#include <stdint.h>

namespace X5000Memory
{
    enum class ResourceKind : uint8_t {
        DataBuffer = 0x0A,
        Texture = 0x40,
        SurfaceAlias = 0x82,
        Surface = 0xC0,
    };

    enum class BackingKind : uint8_t {
        Native,
        Buffer,
        Texture,
        Surface,
    };

    enum class Residency : uint8_t {
        Cold,
        Wired,
        Evicted,
    };

    struct ResidencyChange
    {
        Residency state;
        bool wired;
        bool evicted;
        bool rewired;
    };

    constexpr ResidencyChange updateResidency(const Residency state, const bool wired)
    {
        if (wired && state != Residency::Wired) {
            return {Residency::Wired, true, false, state == Residency::Evicted};
        }
        if (!wired && state == Residency::Wired) { return {Residency::Evicted, false, true, false}; }
        return {state, false, false, false};
    }

    struct Requirements
    {
        ResourceKind kind;
        bool existingAllocation;
        bool specialAddress;
        bool auxiliaryStorage;
        bool importsSurface;
        bool directDisplay;
        bool hasSurfaceCache;
        bool hasLinearClientBuffer;
        bool usesTaskVM;
    };

    constexpr BackingKind choosePrimaryBacking(const Requirements& usage)
    {
        if (!usage.usesTaskVM || usage.existingAllocation || usage.specialAddress || usage.auxiliaryStorage
            || usage.directDisplay)
        {
            return BackingKind::Native;
        }

        switch (usage.kind) {
            case ResourceKind::DataBuffer: return BackingKind::Buffer;
            case ResourceKind::Texture: return usage.importsSurface ? BackingKind::Native : BackingKind::Texture;
            case ResourceKind::Surface:
                if (usage.hasSurfaceCache) { return BackingKind::Surface; }
                if (usage.hasLinearClientBuffer && !usage.importsSurface) { return BackingKind::Buffer; }
                return BackingKind::Native;
            case ResourceKind::SurfaceAlias: return BackingKind::Native;
        }
        return BackingKind::Native;
    }

    constexpr uint64_t MiB = 1ULL << 20;

    constexpr bool validAlignment(const uint64_t alignment, const uint64_t pageSize)
    {
        return alignment >= pageSize && alignment <= UINT32_MAX && (alignment & (alignment - 1)) == 0;
    }

    constexpr uint64_t wiredBudget(const uint64_t native, const uint32_t requestedMiB)
    {
        const auto requested = static_cast<uint64_t>(requestedMiB) * MiB;
        return requestedMiB != 0 && requested < native ? requested : native;
    }

    constexpr uint64_t singleAllocationBudget(const uint64_t native, const uint64_t wired, const uint64_t pageSize)
    {
        const auto limit = (wired - (wired / 4 + (wired % 4 != 0))) & ~(pageSize - 1);
        return native < limit ? native : limit;
    }

    constexpr uint64_t recommendedWorkingSet(const uint64_t wired, const uint64_t pageSize)
    {
        return singleAllocationBudget(UINT64_MAX, wired, pageSize * 2);
    }

    constexpr uint32_t glCommandBudgetKiB(const uint64_t wired, const uint64_t pageSize)
    {
        const auto kib = recommendedWorkingSet(wired, pageSize) / (2 * 1024);
        return kib <= UINT32_MAX ? static_cast<uint32_t>(kib) : UINT32_MAX;
    }
}
