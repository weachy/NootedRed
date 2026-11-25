#!/usr/bin/env python3
"""Check the X5000 unified memory placement, residency and budget policy on the host."""

from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]

TESTS = r"""
#include <X5000MemoryPolicy.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace X5000Memory;

constexpr uint64_t GiB = 1024 * MiB;
constexpr uint64_t page = 4096;

Requirements usable(ResourceKind kind) {
    return {kind, false, false, false, false, false, false, false, true};
}

void testPlacement() {
    auto buffer = usable(ResourceKind::DataBuffer);
    assert(choosePrimaryBacking(buffer) == BackingKind::Buffer);
    auto texture = usable(ResourceKind::Texture);
    assert(choosePrimaryBacking(texture) == BackingKind::Texture);
    unsigned lookups = 0;
    auto video = [&] { ++lookups; return true; };
    assert(placeLinearTexture(0x10002000, false, video) == BackingKind::Native);
    assert(placeLinearTexture(0x10002000, true, video) == BackingKind::Texture);
    assert(placeLinearTexture(0x10082000, false, video) == BackingKind::Texture);
    assert(lookups == 1);
    assert(placeLinearTexture(0x10002000, false, [] { return false; }) == BackingKind::Texture);
    texture.importsSurface = true;
    assert(choosePrimaryBacking(texture) == BackingKind::Native);

    auto surface = usable(ResourceKind::Surface);
    assert(choosePrimaryBacking(surface) == BackingKind::Native);
    surface.hasLinearClientBuffer = true;
    assert(choosePrimaryBacking(surface) == BackingKind::Buffer);
    surface.importsSurface = true;
    assert(choosePrimaryBacking(surface) == BackingKind::Native);
    surface.hasSurfaceCache = true;
    assert(choosePrimaryBacking(surface) == BackingKind::Surface);
    assert(choosePrimaryBacking(usable(ResourceKind::SurfaceAlias)) == BackingKind::Native);

    // Anything the native driver must place itself stays native for every resource kind.
    for (const auto kind : {ResourceKind::DataBuffer, ResourceKind::Texture, ResourceKind::Surface}) {
        for (int reason = 0; reason < 5; ++reason) {
            auto usage = usable(kind);
            usage.hasSurfaceCache = true;
            (reason == 0 ? usage.existingAllocation : reason == 1 ? usage.specialAddress
                : reason == 2 ? usage.auxiliaryStorage : reason == 3 ? usage.directDisplay
                : usage.usesTaskVM) = reason != 4;
            assert(choosePrimaryBacking(usage) == BackingKind::Native);
        }
    }
}

void testResidency() {
    auto change = updateResidency(Residency::Cold, true);
    assert(change.state == Residency::Wired && change.wired && !change.evicted && !change.rewired);
    change = updateResidency(Residency::Wired, false);
    assert(change.state == Residency::Evicted && !change.wired && change.evicted && !change.rewired);
    change = updateResidency(Residency::Evicted, true);
    assert(change.state == Residency::Wired && change.wired && !change.evicted && change.rewired);
    for (const auto state : {Residency::Cold, Residency::Wired, Residency::Evicted}) {
        change = updateResidency(state, state == Residency::Wired);
        assert(change.state == state && !change.wired && !change.evicted && !change.rewired);
    }
    assert(updateResidency(Residency::Cold, false).state == Residency::Cold);
    assert(updateResidency(Residency::Evicted, false).state == Residency::Evicted);
}

void testBudgets() {
    assert(validAlignment(page, page) && validAlignment(2 * page, page) && validAlignment(UINT32_MAX / 2 + 1, page));
    assert(!validAlignment(page / 2, page) && !validAlignment(3 * page, page) && !validAlignment(0, page));
    assert(!validAlignment(UINT64_C(1) << 32, page));

    assert(wiredBudget(22 * GiB, 0) == 22 * GiB);
    assert(wiredBudget(22 * GiB, 768) == 768 * MiB);
    assert(wiredBudget(22 * GiB, 22 * 1024) == 22 * GiB && wiredBudget(22 * GiB, UINT32_MAX) == 22 * GiB);

    // A single allocation may use at most three quarters of the wired limit, rounded down to a page.
    assert(singleAllocationBudget(2 * GiB, 22 * GiB, page) == 2 * GiB);
    assert(singleAllocationBudget(UINT64_MAX, 22 * GiB, page) == 16 * GiB + 512 * MiB);
    assert(singleAllocationBudget(UINT64_MAX, 4 * page + 1, page) == 3 * page);
    assert(singleAllocationBudget(UINT64_MAX, 3, page) == 0);

    assert(recommendedWorkingSet(22 * GiB, page) == 16 * GiB + 512 * MiB);
    assert(recommendedWorkingSet(8 * page + 2 * page - 1, page) % (2 * page) == 0);
    assert(glCommandBudgetKiB(22 * GiB, page) == 8650752);
    assert(glCommandBudgetKiB(UINT64_MAX, page) == UINT32_MAX);
}

void testPurgeableWires() {
    std::array<PurgeableMapping, 2> mappings{{{true, true}, {true, false}}};
    const auto lookup = [&](int16_t index) { return mappings.at(index); };
    assert(exclusiveMappingWires(0, 1, 0, lookup));
    assert(exclusiveMappingWires(2, 3, 1, lookup));
    assert(!exclusiveMappingWires(2, 4, 1, lookup));
    assert(!exclusiveMappingWires(2, 3, 2, lookup));
    assert(!exclusiveMappingWires(2, 3, 0, lookup));
    assert(!exclusiveMappingWires(0, 1, 1, lookup));
    assert(!exclusiveMappingWires(-1, 0, 0, lookup));
    mappings[1].owned = false;
    assert(!exclusiveMappingWires(2, 3, 1, lookup));
}

void testPoolBuckets() {
    assert(!largePoolBuckets(0, 0));
    assert(!largePoolBuckets(8 * GiB, 0));
    assert(!largePoolBuckets(8 * GiB - 512 * MiB, 0));
    assert(largePoolBuckets(16 * GiB - 512 * MiB, 0));
    assert(largePoolBuckets(64 * GiB, 0));
    assert(largePoolBuckets(64 * GiB, 4096));
    assert(!largePoolBuckets(64 * GiB, 4095));
    assert(!largePoolBuckets(64 * GiB, 768));
    for (const bool large : {false, true}) {
        const auto* table = large ? largeWiredInfo : nativeWiredInfo;
        assert(poolRequestLimit(large) == (large ? 96 : 24) * MiB);
        for (const uint32_t refill : {0U, 4U, poolRefill}) {
            const auto limit = poolRequestLimit(large, refill);
            assert(!poolAccepts(0, large, refill) && !poolAccepts(limit + page, large, refill));
            assert(!poolAccepts(page + 1, large, refill));
            unsigned maximum = 0;
            for (uint64_t bytes = page; bytes <= limit; bytes += page) {
                assert(poolAccepts(bytes, large, refill));
                auto remaining = bytes;
                unsigned pieces = 0;
                for (unsigned bucket = 0; bucket < poolBuckets; ++bucket) {
                    const auto shift = table[3 * bucket];
                    assert(table[3 * bucket + 1] >= poolRefill && table[3 * bucket + 2] == poolRefill);
                    pieces += remaining >> shift;
                    remaining &= (UINT64_C(1) << shift) - 1;
                }
                assert(!remaining && pieces <= 64);
                if (pieces > maximum) { maximum = pieces; }
            }
            if (refill == poolRefill) { assert(maximum == (large ? 47U : 35U)); }
        }
    }
}

void testPressureAndCache() {
    assert(poolWatermark(0, 0, 0) == poolRefill);
    assert(poolWatermark(0, 7 * GiB - 1, 8 * GiB) == poolRefill);
    assert(poolWatermark(0, 7 * GiB, 8 * GiB) == 4);
    for (const uint32_t level : {1U, 2U}) { assert(poolWatermark(level, 0, 8 * GiB) == 4); }
    for (const uint32_t level : {3U, 4U, 5U}) { assert(poolWatermark(level, 0, 0) == 0); }
    assert(poolWatermark(0, 8 * GiB, 8 * GiB) == 0);
    assert(poolWatermark(1, UINT64_MAX, 8 * GiB) == 0);
    assert(textureCacheLimit(0) == 0 && textureCacheLimit(768 * MiB) == 24 * MiB);
    assert(textureCacheLimit(8 * GiB) == 256 * MiB && textureCacheLimit(UINT64_MAX) == 256 * MiB);
    const auto perTask = textureCacheLimit(4 * GiB) / 2;
    assert(fitsBudget(0, perTask, perTask) && fitsBudget(perTask - page, page, perTask));
    assert(!fitsBudget(0, 0, perTask) && !fitsBudget(perTask - page + 1, page, perTask));
    assert(!fitsBudget(UINT64_MAX, page, perTask) && !fitsBudget(UINT64_MAX, 1, UINT64_MAX));
}

void testMappings() {
    assert(validFixedRange(page, page) && validFixedRange(UINT64_MAX - page, page));
    assert(!validFixedRange(0, page) && !validFixedRange(page, 0) && !validFixedRange(UINT64_MAX - page + 1, page));
    assert(matchesReservation(0x400D, page, 2 * page, page, 2 * page));
    assert(!matchesReservation(0xD, page, page, page, page));
    assert(!matchesReservation(0x4000, page, page, page, page));
    assert(!matchesReservation(0x400D, 2 * page, page, page, page));
    assert(!matchesReservation(0x400D, page, 2 * page, page, page));
}

void testFragments() {
    constexpr auto window = fragmentSize;
    assert(alignedFragments(page, page, window - page) == 0);
    assert(alignedFragments(page, page, 2 * window - page) == 1);
    assert(alignedFragments(page, 0, 4 * window) == 0);
    FragmentRuns runs{page};
    runs.append(2 * window + page, window - page, 0x77);
    for (uint64_t offset = 0; offset < window; offset += page) { runs.append(3 * window + offset, page, 0x77); }
    assert(runs.complete() == 1 && runs.nextVA == 2 * window && !runs.mapped && runs.basePages == 31);
    runs.append(16 * window, 2 * window, 0x277);
    assert(runs.complete() == 3 && runs.mapped == 2 && runs.basePages == 31);
    FragmentRuns scattered{0};
    for (unsigned index = 0; index < 16; ++index) { scattered.append((16 - index) * page, page, 0x77); }
    assert(scattered.complete() == 0 && alignedFragments(0, 0, scattered.nextVA) == 1);
}

void testFragmentAllocations() {
    for (unsigned mask = 0; mask < 256; ++mask) {
        for (unsigned wanted = 0; wanted <= 9; ++wanted) {
            std::array<unsigned, 8> pieces{0, 1, 2, 3, 4, 5, 6, 7};
            const auto qualifies = [mask](unsigned id) { return (mask & (1U << id)) != 0; };
            const auto taken = std::min<unsigned>(wanted, pieces.size());
            const auto initial = std::count_if(pieces.end() - taken, pieces.end(), qualifies);
            unsigned probes = 0;
            const auto swaps = preferFragmentPieces(pieces.data(), pieces.size(), wanted, [&](unsigned id) {
                ++probes;
                return qualifies(id);
            });
            assert(probes <= 8);
            if (wanted == 0 || wanted >= pieces.size()) { assert(!probes && !swaps); }
            const auto eligible = std::count_if(pieces.end() - taken, pieces.end(), qualifies);
            assert(eligible == std::min<int>(taken, std::popcount(mask)) && eligible - initial == swaps);
            std::sort(pieces.begin(), pieces.end());
            for (unsigned index = 0; index < pieces.size(); ++index) { assert(pieces[index] == index); }
        }
    }
    std::array<unsigned, 24> largePieces{};
    for (unsigned index = 0; index < largePieces.size(); ++index) { largePieces[index] = index; }
    unsigned probes = 0;
    const auto swaps = preferFragmentPieces(largePieces.data(), largePieces.size(), 16, [&](unsigned id) {
        ++probes;
        return id < 8;
    });
    assert(probes == 8 && swaps == 4);
    assert(std::count_if(largePieces.begin() + 8, largePieces.end(), [](unsigned id) { return id < 8; }) == swaps);
    std::sort(largePieces.begin(), largePieces.end());
    for (unsigned index = 0; index < largePieces.size(); ++index) { assert(largePieces[index] == index); }
    for (unsigned successAttempt : {1U, 2U, 3U}) {
        uint64_t alignment = 2 * page;
        unsigned attempts = 0;
        bool retried = false;
        const auto address = allocateFragmentVA(alignment, [&]() -> uint64_t {
            ++attempts;
            assert(alignment == (attempts == 1 ? fragmentSize : 2 * page));
            return attempts == successAttempt ? 8 * fragmentSize : 0;
        }, retried);
        assert(alignment == 2 * page && retried == (successAttempt > 1));
        assert(attempts == std::min(successAttempt, 2U) && (address != 0) == (successAttempt <= 2));
    }
}

FragmentRuns replayMap(const std::vector<PhysicalSegment>& segments, const uint64_t va, const unsigned pages,
                       const bool promote, const bool joinSegments = true, const uint64_t vmBase = 0) {
    std::vector<uint64_t> expected;
    for (const auto& segment : segments) {
        for (uint64_t offset = 0; offset < segment.bytes; offset += page) { expected.push_back(segment.address + offset); }
    }
    const auto lookup = [&](const uint64_t offset) -> PhysicalSegment {
        uint64_t begin = 0;
        for (const auto& segment : segments) {
            if (offset - begin < segment.bytes) { return {segment.address + offset - begin, segment.bytes - (offset - begin)}; }
            begin += segment.bytes;
        }
        assert(false);
        return {};
    };
    assert(pages <= expected.size());
    const uint64_t length = pages * page;
    FragmentRuns runs{va};
    for (uint64_t offset = 0; offset < length;) {
        auto span = lookup(offset);
        if (promote) {
            span.bytes = fragmentSegmentLength(va + offset, length - offset, span, [&](const uint64_t ahead) {
                assert(ahead < length - offset);
                return joinSegments ? lookup(offset + ahead) : PhysicalSegment{};
            }, vmBase);
        }
        auto bytes = std::min(span.bytes, length - offset);
        assert(bytes && bytes % page == 0);
        while (bytes) {
            const uint64_t leafSize = 2 * MiB;
            const auto address = va + offset;
            const auto relative = address - vmBase;
            const auto available = std::min(bytes, leafSize - (relative & (leafSize - 1)));
            const auto granule = std::min(std::bit_floor(available), relative ? relative & -relative : leafSize);
            const auto batch = available & -granule;
            assert(granule >= page && batch);
            if (promote) { assert(!((span.address | address) & (granule - 1))); }
            for (uint64_t delta = 0; delta < batch; delta += page) {
                assert(expected[(offset + delta) / page] == span.address + delta);
            }
            const auto order = std::countr_zero(granule / page);
            runs.append(span.address, batch, 0x77 | (static_cast<uint64_t>(order) << 7));
            offset += batch;
            span.address += batch;
            bytes -= batch;
        }
    }
    assert(runs.nextVA == va + length);
    return runs;
}

void testFragmentSegments() {
    constexpr uint64_t physical = 64 * fragmentSize;
    std::vector<PhysicalSegment> split;
    for (unsigned index = 0; index < 32; ++index) { split.push_back({physical + index * page, page}); }
    const auto native = replayMap(split, 0, 32, false);
    const auto joined = replayMap(split, 0, 32, true);
    const auto dma = replayMap(split, 0, 32, true, false);
    assert(native.complete() == 2 && !native.mapped && native.basePages == 32);
    assert(joined.complete() == 2 && joined.mapped == 2 && !joined.basePages);
    assert(dma.complete() == 2 && !dma.mapped && dma.basePages == 32);
    assert(replayMap({{physical, 2 * fragmentSize}}, 0, 32, true, false).mapped == 2);
    auto rewired = split;
    std::reverse(rewired.begin(), rewired.end());
    const auto fallback = replayMap(rewired, 0, 32, true);
    assert(!fallback.complete() && !fallback.mapped && fallback.basePages == 32);
    auto hole = split;
    hole[15].address += 8 * fragmentSize;
    assert(replayMap(hole, 0, 32, true).mapped == 1);
    assert(replayMap(split, 2 * MiB - fragmentSize, 31, true).mapped == 1);
    assert(replayMap(split, page, 32, true).mapped == 0);
    assert(replayMap({{physical + page, 17 * page}, {physical + 18 * page, 18 * page}}, page, 33, true).mapped == 1);
    assert(replayMap({{physical, 4 * fragmentSize}}, 0, 64, true).mapped == 4);
    assert(replayMap({{physical, 2 * MiB}}, fragmentSize, 512, true, true, fragmentSize).mapped == 32);
    assert(replayMap({{physical + page, 4 * fragmentSize}}, 0, 64, true).mapped == 0);
    for (unsigned bad = 1; bad < 16; ++bad) {
        for (const auto invalid : {PhysicalSegment{0, page}, PhysicalSegment{physical + bad * page, 0},
                                   PhysicalSegment{physical + bad * page + 1, page}, PhysicalSegment{UINT64_MAX - page + 1, page},
                                   PhysicalSegment{physical + bad * page, page - 1}}) {
            unsigned probes = 0;
            assert(fragmentSegmentLength(0, fragmentSize, {physical, page}, [&](uint64_t offset) {
                ++probes;
                return offset == bad * page ? invalid : PhysicalSegment{physical + offset, page};
            }) == page);
            assert(probes == bad);
        }
    }
    auto unexpected = [](uint64_t) -> PhysicalSegment { assert(false); return {}; };
    assert(fragmentSegmentLength(0, fragmentSize - page, {physical, page}, unexpected) == page);
    assert(fragmentSegmentLength(UINT64_MAX - page + 1, fragmentSize, {physical, page}, unexpected) == page);
    assert(fragmentSegmentLength(0, fragmentSize, {physical, page}, unexpected, fragmentSize) == page);
    assert(fragmentSegmentLength(page, fragmentSize, {physical + page, 2 * page}, unexpected, page) == 2 * page);
    assert(fragmentSegmentLength(0, fragmentSize, {physical, page}, [](uint64_t) { return PhysicalSegment{}; }) == page);
    for (unsigned phase = 0; phase < 16; ++phase) {
        for (unsigned paPhase = 0; paPhase < 16; ++paPhase) {
            std::vector<PhysicalSegment> layout;
            uint64_t cursor = physical + paPhase * page;
            for (unsigned index = 0; index < 23; ++index) {
                const auto bytes = (1 + (index * 7 + phase) % 39) * page;
                layout.push_back({cursor, bytes});
                cursor += bytes + (index % 7 == 5 ? 11 * page : 0);
            }
            replayMap(layout, 2 * MiB - fragmentSize + phase * page, 239, true);
        }
    }
    std::puts("128 KiB replay: 0 -> 2 encoded 64 KiB windows; 32 base pages after rewire");
}

int main() {
    testPlacement();
    testResidency();
    testPurgeableWires();
    testBudgets();
    testPoolBuckets();
    testPressureAndCache();
    testMappings();
    testFragments();
    testFragmentAllocations();
    testFragmentSegments();
    std::puts("memory policy checks passed");
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="nred-memory-policy-") as directory:
        unit = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        unit.write_text(TESTS)
        subprocess.run([
            "xcrun", "clang++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(REPO / "NootedRed"), str(unit), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
