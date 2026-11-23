#!/usr/bin/env python3
"""Check the X5000 unified memory placement, residency and budget policy on the host."""

from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]

TESTS = r"""
#include <X5000MemoryPolicy.hpp>
#include <cassert>
#include <cstdio>
#include <initializer_list>

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

void testPoolBuckets() {
    assert(!largePoolBuckets(0, 0));
    assert(!largePoolBuckets(8 * GiB, 0));
    assert(!largePoolBuckets(8 * GiB - 512 * MiB, 0));
    assert(largePoolBuckets(16 * GiB - 512 * MiB, 0));
    assert(largePoolBuckets(64 * GiB, 0));
    assert(largePoolBuckets(64 * GiB, 4096));
    assert(!largePoolBuckets(64 * GiB, 4095));
    assert(!largePoolBuckets(64 * GiB, 768));
}

int main() {
    testPlacement();
    testResidency();
    testBudgets();
    testPoolBuckets();
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
