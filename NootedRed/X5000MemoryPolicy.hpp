// X5000 Unified Memory placement, residency and budget rules. Free of kernel headers so that
// tools/test_x5000_memory_policy.py can check them on the host.

#pragma once

#include <stdint.h>

namespace X5000Memory
{

    enum class ResourceKind : uint8_t
    {
        DataBuffer   = 0x0A,
        Texture      = 0x40,
        SurfaceAlias = 0x82,
        Surface      = 0xC0,
    };

    enum class BackingKind : uint8_t
    {
        Native,
        Buffer,
        Texture,
        Surface,
    };

    enum class Residency : uint8_t
    {
        Cold,
        Wired,
        Evicted,
    };

    struct ResidencyChange
    {
        Residency state;
        bool      wired;
        bool      evicted;
        bool      rewired;
    };

    constexpr ResidencyChange updateResidency(const Residency state, const bool wired)
    {
        if (wired && state != Residency::Wired) { return {Residency::Wired, true, false, state == Residency::Evicted}; }
        if (!wired && state == Residency::Wired) { return {Residency::Evicted, false, true, false}; }
        return {state, false, false, false};
    }

    struct PurgeableMapping
    {
        bool owned;
        bool committed;
    };

    template<typename Lookup>
    constexpr bool exclusiveMappingWires(const int16_t mappings, const int references, int32_t wires,
                                         const Lookup& lookup)
    {
        if (mappings < 0 || references != mappings + 1 || wires < 0 || wires > mappings) { return false; }
        for (int16_t index = 0; index < mappings; ++index) {
            const auto mapping = lookup(index);
            if (!mapping.owned) { return false; }
            if (mapping.committed) { --wires; }
        }
        return wires == 0;
    }

    struct Requirements
    {
        ResourceKind kind;
        bool         existingAllocation;
        bool         specialAddress;
        bool         auxiliaryStorage;
        bool         importsSurface;
        bool         directDisplay;
        bool         hasSurfaceCache;
        bool         hasLinearClientBuffer;
        bool         usesTaskVM;
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
            case ResourceKind::Texture   : return usage.importsSurface ? BackingKind::Native : BackingKind::Texture;
            case ResourceKind::Surface:
                if (usage.hasSurfaceCache) { return BackingKind::Surface; }
                if (usage.hasLinearClientBuffer && !usage.importsSurface) { return BackingKind::Buffer; }
                return BackingKind::Native;
            case ResourceKind::SurfaceAlias: return BackingKind::Native;
        }
        return BackingKind::Native;
    }

    constexpr uint64_t MiB = 1ULL << 20;

    template<typename VideoShare>
    constexpr BackingKind placeLinearTexture(const uint32_t copyFlags, const bool hasSurfaceLayout,
                                             const VideoShare& usesVideoShare)
    {
        return copyFlags == 0x10002000 && !hasSurfaceLayout && usesVideoShare() ? BackingKind::Native :
                                                                                 BackingKind::Texture;
    }

    constexpr bool validAlignment(const uint64_t alignment, const uint64_t pageSize)
    {
        return alignment >= pageSize && alignment <= UINT32_MAX && (alignment & (alignment - 1)) == 0;
    }

    constexpr bool validFixedRange(const uint64_t address, const uint64_t length)
    {
        return address && length && length <= UINT64_MAX - address;
    }

    constexpr bool matchesReservation(const uint32_t flags, const uint64_t mapped, const uint64_t reserved,
                                      const uint64_t address, const uint64_t length)
    {
        return (flags & 0x4001) == 0x4001 && mapped == address && reserved == length;
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

    // The large pool buckets keep up to about 200 MiB wired (the pool refills each bucket to 24 descriptors and keeps
    // returned ones up to its capacity). They are used on 16 GB machines and larger, unless the residency budget was
    // lowered below 4 GiB. The threshold sits at 12 GiB because firmware may report RAM minus the carve-out; an
    // unknown RAM size keeps the native buckets.
    constexpr uint64_t largeBucketMinimumRAM       = 12ULL << 30;
    constexpr uint32_t largeBucketMinimumBudgetMiB = 4096;
    constexpr bool     largePoolBuckets(const uint64_t physicalBytes, const uint32_t requestedBudgetMiB)
    {
        return physicalBytes >= largeBucketMinimumRAM
               && (requestedBudgetMiB == 0 || requestedBudgetMiB >= largeBucketMinimumBudgetMiB);
    }

    constexpr uint32_t poolBuckets = 5;
    constexpr uint32_t poolRefill = 24;
    constexpr uint32_t nativeWiredInfo[3 * poolBuckets] = {20, 64, 24, 18, 32, 24, 16, 32, 24, 14, 32, 24, 12, 32, 24};
    constexpr uint32_t largeWiredInfo[3 * poolBuckets] = {22, 32, 24, 20, 64, 24, 18, 32, 24, 16, 32, 24, 12, 32, 24};

    constexpr uint64_t poolRequestLimit(const bool large, const uint32_t refill = poolRefill)
    {
        return static_cast<uint64_t>(refill) << (large ? largeWiredInfo[0] : nativeWiredInfo[0]);
    }

    constexpr bool poolAccepts(const uint64_t bytes, const bool large, const uint32_t refill)
    {
        return bytes && (bytes & 4095) == 0 && bytes <= poolRequestLimit(large, refill);
    }

    constexpr uint32_t poolWatermark(const uint32_t pressure, const uint64_t wired, const uint64_t budget)
    {
        if (pressure >= 3 || (budget && wired >= budget)) { return 0; }
        if (pressure || (budget && wired >= budget - budget / 8)) { return 4; }
        return poolRefill;
    }

    constexpr uint64_t textureCacheLimit(const uint64_t budget)
    {
        return budget / 32 < 256 * MiB ? budget / 32 : 256 * MiB;
    }

    constexpr bool fitsBudget(const uint64_t used, const uint64_t bytes, const uint64_t limit)
    {
        return bytes != 0 && used <= limit && bytes <= limit - used;
    }

    constexpr uint64_t gpuPageSize = 4096;
    constexpr uint64_t fragmentSize = 16 * gpuPageSize;

    struct PhysicalSegment
    {
        uint64_t address;
        uint64_t bytes;

        constexpr bool valid() const
        {
            constexpr uint64_t limit = UINT64_C(1) << 48;
            return address && address < limit && bytes && bytes <= limit - address
                   && ((address | bytes) & (gpuPageSize - 1)) == 0;
        }
    };

    template<typename Lookup>
    constexpr uint64_t fragmentSegmentLength(const uint64_t va, const uint64_t remaining, const PhysicalSegment first,
                                             const Lookup& lookup, const uint64_t vmBase = 0)
    {
        if (!first.valid() || !remaining || remaining > UINT64_MAX - va || va < vmBase || (vmBase & (fragmentSize - 1))
            || ((va | remaining) & (gpuPageSize - 1))) { return first.bytes; }
        const auto bytes = first.bytes < remaining ? first.bytes : remaining;
        const auto phase = (va ^ first.address) | vmBase;
        const auto commonAlignment = phase & -phase;
        if (phase && bytes / 2 >= commonAlignment) { return commonAlignment; }

        const auto head = -va & (fragmentSize - 1);
        if (remaining < head + fragmentSize || ((va ^ first.address) & (fragmentSize - 1))) { return first.bytes; }
        if (head) { return bytes > head ? head : first.bytes; }
        if (bytes >= fragmentSize) { return first.bytes; }

        for (auto covered = bytes; covered < fragmentSize;) {
            const PhysicalSegment next = lookup(covered);
            if (!next.valid() || next.address != first.address + covered) { return first.bytes; }
            covered += next.bytes;
        }
        return fragmentSize;
    }

    template<typename Piece, typename Qualifies>
    constexpr uint32_t preferFragmentPieces(Piece* const stack, const uint32_t count, const uint32_t wanted,
                                            const Qualifies& qualifies)
    {
        if (!stack || !wanted || wanted >= count) { return 0; }
        constexpr uint32_t probeLimit = 8;
        const auto firstSelected = count - wanted;
        auto selected = count;
        auto candidate = firstSelected;
        uint32_t probes = 0;
        uint32_t swaps = 0;
        while (selected > firstSelected && candidate && probes + 1 < probeLimit) {
            ++probes;
            if (qualifies(stack[--selected])) { continue; }
            do {
                ++probes;
                if (qualifies(stack[--candidate])) {
                    const auto previous = stack[selected];
                    stack[selected] = stack[candidate];
                    stack[candidate] = previous;
                    ++swaps;
                    break;
                }
            } while (candidate && probes < probeLimit);
        }
        return swaps;
    }

    template<typename Allocate>
    constexpr uint64_t allocateFragmentVA(uint64_t& alignment, const Allocate& allocate, bool& retried)
    {
        const auto required = alignment;
        alignment = fragmentSize;
        const auto address = allocate();
        alignment = required;
        retried = address == 0;
        return address ? address : allocate();
    }

    constexpr uint64_t alignedFragments(const uint64_t va, const uint64_t physical, const uint64_t length)
    {
        const auto mask = fragmentSize - 1;
        if (((va ^ physical) & mask) != 0) { return 0; }
        const auto skip = -va & mask;
        return length > skip ? (length - skip) / fragmentSize : 0;
    }

    struct FragmentRuns
    {
        uint64_t nextVA;
        uint64_t startVA{};
        uint64_t startPhysical{};
        uint64_t length{};
        uint64_t contiguous{};
        uint64_t mapped{};
        uint64_t basePages{};

        constexpr void append(const uint64_t physical, const uint64_t bytes, const uint64_t flags)
        {
            const auto order = (flags >> 7) & 31;
            if (order >= 4) { mapped += alignedFragments(nextVA, nextVA, bytes); }
            if (!order) { basePages += bytes / gpuPageSize; }
            if (!length || physical != startPhysical + length) {
                contiguous += alignedFragments(startVA, startPhysical, length);
                startVA = nextVA;
                startPhysical = physical;
                length = 0;
            }
            length += bytes;
            nextVA += bytes;
        }

        constexpr uint64_t complete() const { return contiguous + alignedFragments(startVA, startPhysical, length); }
    };

    constexpr uint32_t glCommandBudgetKiB(const uint64_t wired, const uint64_t pageSize)
    {
        const auto kib = recommendedWorkingSet(wired, pageSize) / (2 * 1024);
        return kib <= UINT32_MAX ? static_cast<uint32_t>(kib) : UINT32_MAX;
    }

}    // namespace X5000Memory
