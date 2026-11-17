#!/usr/bin/env python3
"""Exercise the production VRAM fallback and resolved overload on CPU stubs.

The two allocator overloads below have the ABIs found in the kernel symbols.
The range overload consumes only its register arguments, so testing a bad
resolver cannot read a missing stack argument or perform a kernel allocation.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]


def function(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREAMBLE = r"""
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
using UInt32 = uint32_t;
using UInt64 = unsigned long long;
using SInt32 = int32_t;
constexpr UInt64 PAGE_SIZE = 4096, PAGE_MASK = 4095;
#define SYSLOG(...) do {} while (0)
SInt32 OSIncrementAtomic(volatile SInt32* p) { auto old=*p; *p=old+1; return old; }
class X5000 {
public:
"""

STUBS = r"""
    bool allocatorTraceEnabled{false};
    volatile SInt32 allocatorTraceCount{0};
    static X5000& singleton() { static X5000 p; return p; }
    static bool wrapAllocateLargeBlocks(void*,void*,UInt64,UInt64,UInt64,UInt32,UInt32,bool);
};
struct Call {
    UInt64 size, alignment, chunk;
    UInt32 type, origin, operation;
    bool contiguous;
};
std::vector<Call> calls;
UInt64 available = 0x20000000;
int successfulStage = 0;
bool wrongOverload = false;
UInt64 totalFree(void*) { return available; }
bool contiguous(void*,void*,UInt64 size,UInt64 align,UInt32 type,bool,UInt32 origin,UInt32 operation) {
    calls.push_back({size,align,0,type,origin,operation,true});
    return successfulStage == 1;
}
bool enumOverload(void*,void*,UInt64 size,UInt64 align,UInt32 type,bool,bool,UInt64 chunk,
                  UInt32 origin,UInt32 operation) {
    calls.push_back({size,align,chunk,type,origin,operation,false});
    return successfulStage == 2;
}
// Real yyyy ABI: the enum's place is a RANGE START and the next register is RANGE LENGTH.
// Do not consume stack arguments: the defective caller does not supply all of them.
bool rangeOverload(void*,void*,UInt64,UInt64,UInt64 rangeStart,UInt64 rangeLength,
                   bool,bool,UInt64,UInt32,UInt32) {
    wrongOverload = true;
    printf("wrong range overload: start=0x%llx length=0x%llx (expected VRAM range)\n",
           rangeStart, rangeLength);
    return false;
}
void* resolve(const char* symbol) {
    const std::string name(symbol);
    if (name.find("yy28_eAMD_MEMORY_ALLOCATION_TYPEbby") != std::string::npos)
        return (void*)&enumOverload;
    if (name.find("yyyybby") != std::string::npos)
        return (void*)&rangeOverload;
    return nullptr;
}
unsigned checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", label); }
}
void reset(int stage = 0) {
    calls.clear(); successfulStage = stage; available = 0x20000000; wrongOverload = false;
}
"""

TESTS = r"""
int main() {
    auto& p = X5000::singleton();
    p.orgTotalFreeMemory = totalFree;
    p.orgAllocate = contiguous;
    p.orgAllocateNonContiguous = reinterpret_cast<decltype(p.orgAllocateNonContiguous)>(resolve(RESOLVER_SYMBOL));
    check(p.orgAllocateNonContiguous != nullptr, "resolved allocator exists");
    int memory = 0, element = 0;
    reset(1);
    check(X5000::wrapAllocateLargeBlocks(&memory,&element,0x40000,0x10000,0x10000,7,9,false),
          "contiguous success retained");
    check(calls.size() == 1 && calls.front().contiguous, "contiguous success stops retries");
    reset(2);
    check(X5000::wrapAllocateLargeBlocks(&memory,&element,0x40000,0x10000,0x10000,7,9,false),
          "fragmented allocation can use non-contiguous fallback");
    check(!wrongOverload, "resolver matches declared allocation-type ABI");
    check(!calls.empty() && !calls.back().contiguous && calls.back().type == 1 &&
          calls.back().chunk == 0x20000 && calls.back().origin == 7 && calls.back().operation == 9,
          "fallback preserves type, chunk size, origin and operation");
    for (bool nonVisibleOnly : {false,true}) {
        for (UInt64 minimum : {0ULL,0x1000ULL,0x4000ULL,0x10000ULL}) {
            reset();
            check(!X5000::wrapAllocateLargeBlocks(&memory,&element,0x28000,0x200000,minimum,7,9,nonVisibleOnly),
                  "all allocation failures propagate");
            check(!wrongOverload, "every fallback uses allocation-type overload");
            for (const auto& c : calls) {
                check(c.alignment >= 0x200000, "fallback preserves requested start alignment");
                check(!nonVisibleOnly || c.type == 1, "non-visible-only excludes visible/full ranges");
                check(c.contiguous || c.chunk >= minimum, "fallback preserves minimum block size");
            }
        }
    }
    reset();
    available = 4096;
    check(!X5000::wrapAllocateLargeBlocks(&memory,&element,0x40000,0x10000,0x10000,7,9,false) && calls.empty(),
          "insufficient free memory fails without allocator calls");
    reset();
    check(!X5000::wrapAllocateLargeBlocks(nullptr,&element,4096,4096,4096,7,9,false) &&
          !X5000::wrapAllocateLargeBlocks(&memory,nullptr,4096,4096,4096,7,9,false) &&
          !X5000::wrapAllocateLargeBlocks(&memory,&element,0,4096,4096,7,9,false) && calls.empty(),
          "invalid inputs do not enter allocator");
    printf("%u checks, %u failures; CPU stubs only, no GPU/kernel allocation\n", checks, failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref")
    args = parser.parse_args()

    def read(name):
        if args.baseline_ref:
            return subprocess.check_output(
                ["git", "show", f"{args.baseline_ref}:NootedRed/{name}"], cwd=REPO, text=True,
            )
        return (REPO / "NootedRed" / name).read_text()

    source, header = read("X5000.cpp"), read("X5000.hpp")
    declarations = "\n".join(re.findall(
        r"^\s*(?:UInt64|bool)\s+\(\*org(?:TotalFreeMemory|Allocate|AllocateNonContiguous)\)[^;]+;",
        header, re.M,
    ))
    assert declarations.count("nullptr") == 3
    symbol = re.search(
        r"orgAllocateNonContiguous\s*=\s*reinterpret_cast.*?patcher\.solveSymbol\(\s*id,.*?"
        r'"([^"]*allocateNonContiguous[^"]*)"', source, re.S,
    ).group(1)
    method = function(source, "bool X5000::wrapAllocateLargeBlocks(")
    unit = PREAMBLE + declarations + STUBS + method + f'\n#define RESOLVER_SYMBOL "{symbol}"\n' + TESTS
    with tempfile.TemporaryDirectory(prefix="nred-allocator-test-") as temp:
        path, binary = Path(temp) / "test.cpp", Path(temp) / "test"
        path.write_text(unit)
        subprocess.run([
            "xcrun", "clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-Wno-unused-variable", "-fsanitize=address,undefined",
            # The baseline intentionally invokes the wrong function ABI. The
            # range stub safely records only register arguments to expose it.
            "-fno-sanitize=function", str(path), "-o", str(binary),
        ], check=True)
        print(f"resolved symbol: {symbol}", flush=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
