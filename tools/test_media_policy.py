#!/usr/bin/env python3
"""Exercise production media readiness/failure policy, without loading a kext."""
from pathlib import Path
import re
import subprocess
import tempfile

from test_userpatch_lifecycle import function

ROOT = Path(__file__).resolve().parents[1]

STUBS = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "MediaHardware.h"
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using mach_vm_address_t = uint64_t;
struct KernelPatcher {};
struct OSMetaClass {};
struct OSMetaClassBase {};
struct IORegistryEntry {
    std::map<std::string, int> properties;
    void removeProperty(const char* key) { properties.erase(key); }
};
struct IOService : IORegistryEntry {
    std::atomic<int> refs{1};
    void retain() { assert(refs.fetch_add(1) > 0); }
    void release() { assert(refs.fetch_sub(1) > 0); }
};
struct OSDictionary {
    std::map<std::string, int> properties;
    void removeObject(const char* key) { properties.erase(key); }
};
using IOLock = std::mutex;
IOLock* IOLockAlloc() { return new IOLock; }
void IOLockLock(IOLock* lock) { lock->lock(); }
void IOLockUnlock(IOLock* lock) { lock->unlock(); }
bool OSCompareAndSwap(UInt32 old, UInt32 value, volatile UInt32* target) {
    return __sync_bool_compare_and_swap(target, old, value);
}
UInt32 rejected = 0;
void publishReject(UInt32 reason) { rejected = reason; }
void publishStage(UInt32) {}
#define SYSLOG(...) do {} while (0)
#define FunctionCast(fn, address) reinterpret_cast<decltype(&fn)>(address)
struct X5000 {
    bool capability = true;
    static X5000& singleton() { static X5000 x; return x; }
    void setVCN0Capability(void*, bool enabled) { capability = enabled; }
};
struct NRed {
    bool external = false;
    unsigned device = 0x1636;
    std::map<std::string, UInt32> properties;
    static NRed& singleton() { static NRed n; return n; }
    bool hasExternalVideo() const { return external; }
    unsigned getDeviceID() const { return device; }
    void setProp32(const char* key, UInt32 value) { properties[key] = value; }
};
struct UserPatches {
    bool supported = true;
    static UserPatches& singleton() { static UserPatches p; return p; }
    bool mediaSupported() const { return supported; }
};
struct DeviceProfile {
    static constexpr int kVcn2_2 = 0x202;
    struct { bool hasVCN0 = true; int vcnGeneration = kVcn2_2; } media;
    static DeviceProfile* current() { static DeviceProfile p; return &p; }
};
"""

TESTS = r"""
unsigned checks = 0, failures = 0;
void check(bool value, const char* text) {
    ++checks; if (!value) { ++failures; std::printf("FAIL %s\n", text); }
}
bool initResult = true;
bool originalInit(void*, UInt32) { return initResult; }
int main() {
    auto& v = VDA::singleton();
    auto& b = VCNBridge::singleton();
    auto* profile = DeviceProfile::current();
    check(v.canAdvertise() && !b.userPatchesAllowed(), "personality bootstrap does not require allocated engine");
    check(v.isHEVC10EncodeAllowed(), "validated Renoir can advertise Main10 before engine allocation");
    check(v.isHEVC8KDecodeAllowed(), "validated Renoir can advertise 8K before engine allocation");
    v.hevc10Allowed = false;
    check(v.isHEVC8KDecodeAllowed(), "Main-only policy retains 8K Main decode");
    v.hevc10Allowed = true;
    for (bool* flag : {&v.enabled, &v.hevcAllowed}) {
        *flag = false;
        check(!v.isHEVC8KDecodeAllowed(), "media/HEVC opt-outs block 8K advertisement");
        *flag = true;
    }
    for (bool* flag : {&v.enabled, &v.hevcAllowed, &v.hevc10Allowed}) {
        *flag = false;
        check(!v.isHEVC10EncodeAllowed(), "each media/HEVC/Main10 boot opt-out blocks Main10 encode");
        *flag = true;
    }
    for (unsigned device : {0x1636, 0x1638, 0x164C, 0x15E7}) {
        NRed::singleton().device = device;
        check(!v.shouldStripCapabilities(), "accepted VCN 2.2 target keeps codec properties");
        check(v.isHEVC10EncodeAllowed(), "same VCN 2.2 IP enables Main10 with reviewed backend");
        check(v.isHEVC8KDecodeAllowed(), "same VCN 2.2 IP enables bounded 8K with reviewed backend");
        for (int blocked = 0; blocked < 4; ++blocked) {
            UserPatches::singleton().supported = blocked != 0;
            NRed::singleton().external = blocked == 1;
            profile->media.hasVCN0 = blocked != 2;
            profile->media.vcnGeneration = blocked == 3 ? 0x100 : DeviceProfile::kVcn2_2;
            check(!v.isHEVC10EncodeAllowed() && !v.isHEVC8KDecodeAllowed(),
                  "same-IP expansion cannot bypass backend, profile or external-GPU policy");
            check(v.shouldStripCapabilities(), "rejected VCN 2.2 target withholds codec properties");
        }
        UserPatches::singleton().supported = true;
        NRed::singleton().external = false;
        profile->media.hasVCN0 = true;
        profile->media.vcnGeneration = DeviceProfile::kVcn2_2;
    }
    for (unsigned device : {0, 0x15DD, 0x15D8, 0x73BF, 0x73AF, 0xFFFF, 0x11636}) {
        NRed::singleton().device = device;
        check(!v.canAdvertise() && !v.isHEVC10EncodeAllowed() && !v.isHEVC8KDecodeAllowed(),
              "unsupported IP cannot use the bridge even with a stale VCN2 profile");
        check(!v.shouldStripCapabilities(), "other GPUs retain upstream default personality");
    }
    NRed::singleton().device = 0x1636;
    v.enabled = false;
    check(!v.canAdvertise(), "NoVDA withdraws capabilities");
    for (unsigned device : {0x1636, 0x15DD, 0x15D8, 0xFFFF}) {
        NRed::singleton().device = device;
        check(v.shouldStripCapabilities(), "explicit NoVDA strips codec properties on every device");
    }
    NRed::singleton().device = 0x1636;
    v.enabled = true;
    for (int blocked = 0; blocked < 4; ++blocked) {
        UserPatches::singleton().supported = blocked != 0;
        NRed::singleton().external = blocked == 1;
        profile->media.hasVCN0 = blocked != 2;
        profile->media.vcnGeneration = blocked == 3 ? 0x100 : DeviceProfile::kVcn2_2;
        check(!v.canAdvertise(), "unsupported build/backend/device/dGPU never advertises bridge");
        check(!v.isHEVC10EncodeAllowed(), "unsupported media prerequisites also block Main10 encode");
        check(!v.isHEVC8KDecodeAllowed(), "unsupported media prerequisites also block 8K decode");
    }
    UserPatches::singleton().supported = true; NRed::singleton().external = false;
    profile->media.hasVCN0 = true; profile->media.vcnGeneration = DeviceProfile::kVcn2_2;
    b.requested = b.x5000Prepared = b.x6000Prepared = b.loadCompleted = b.engineAllocated = true;
    check(b.userPatchesAllowed(), "allocated/prepared bridge enables pages before first hardware init");
    for (bool* flag : {&b.requested, &b.x5000Prepared, &b.x6000Prepared, &b.loadCompleted, &b.engineAllocated}) {
        *flag = false; check(!b.userPatchesAllowed(), "every preparation/engine prerequisite gates pages"); *flag = true;
    }
    b.orgInitHardware = reinterpret_cast<mach_vm_address_t>(&originalInit);
    b.hardwareHandle = reinterpret_cast<void*>(4);
    check(VCNBridge::initHardware(nullptr, 0) && b.userPatchesAllowed(), "successful native initialization preserves media");
    initResult = false;
    check(!VCNBridge::initHardware(nullptr, 1), "native channel initialization failure propagated");
    check(!b.userPatchesAllowed() && !v.canAdvertise() && b.mediaFailure == 8,
          "hardware failure also disables page patching and advertisement");
    check(!v.isHEVC10EncodeAllowed(), "latched hardware failure blocks Main10 encode");
    check(b.engineEnabled() && !X5000::singleton().capability,
          "failed channel keeps allocated object ABI while clearing VCN hardware capability");
    b.mediaFailure = 0; b.hardwareHandle = nullptr;
    OSDictionary dictionary;
    IOService accelerator;
    for (const auto* key : kMediaCapabilities) {
        dictionary.properties[key] = 1; accelerator.properties[key] = 1;
    }
    dictionary.properties["graphics"] = 1; accelerator.properties["graphics"] = 1;
    VDA::stripCapabilities(&dictionary);
    check(dictionary.properties.size() == 1 && dictionary.properties.count("graphics"), "strip only codec properties");
    b.acceleratorLock = IOLockAlloc();
    b.setAccelerator(&accelerator);
    check(accelerator.refs == 2, "accelerator reference held while registered");
    b.setAccelerator(&accelerator);
    check(accelerator.refs == 2, "repeated attach does not leak");
    b.failMedia(21);
    check(!v.canAdvertise() && !b.userPatchesAllowed(), "late failure gates properties and all future page writes");
    check(b.engineEnabled(), "existing bridged objects retain ABI routing after late page failure");
    check(accelerator.properties.size() == 1 && accelerator.properties.count("graphics"), "live codec properties withdrawn");
    check(rejected == 21 && NRed::singleton().properties["NRedVCNMediaFailure"] == 21, "failure reason published");
    b.failMedia(22);
    check(b.mediaFailure == 21 && rejected == 21, "first failure is latched for the boot");
    b.setAccelerator(nullptr);
    check(accelerator.refs == 1, "stop releases reference");
    // Exercise the retained-object lifetime when failure races accelerator stop.
    for (int i = 0; i < 100; ++i) {
        b.mediaFailure = 0; b.setAccelerator(&accelerator);
        std::thread failure([&] { b.failMedia(8); });
        std::thread stop([&] { b.setAccelerator(nullptr); });
        failure.join(); stop.join();
        check(accelerator.refs == 1 && b.acceleratorHandle == nullptr, "concurrent stop/failure retains no stale reference");
    }
    delete b.acceleratorLock; b.acceleratorLock = nullptr;
    std::printf("%u checks, %u failures; no GPU work submitted\n", checks, failures);
    return failures ? 1 : 0;
}
"""


def main():
    headers = ""
    for name, cls in (("VDA.hpp", "VDA"), ("VCNBridge.hpp", "VCNBridge")):
        text = (ROOT / "NootedRed" / name).read_text()
        text = re.sub(r"^#(?:include|pragma).*$", "", text, flags=re.M)
        text = text.replace("private:", "public:")
        headers += text.replace(f"class {cls}\n{{", f"class {cls}\n{{\npublic:")
    va = (ROOT / "NootedRed/VDA.cpp").read_text()
    bridge = (ROOT / "NootedRed/VCNBridge.cpp").read_text()
    keys = re.search(r"const char\* const kMediaCapabilities\[\] = \{.*?\};", va, re.S)[0]
    methods = """
VDA& VDA::singleton() { static VDA v; return v; }
VCNBridge& VCNBridge::singleton() { static VCNBridge b; return b; }
"""
    for marker in ("bool VDA::isSupported(", "bool VDA::canAdvertise(", "bool VDA::isHEVC10EncodeAllowed(",
                   "bool VDA::isHEVC8KDecodeAllowed(", "bool VDA::shouldStripCapabilities(",
                   "void VDA::stripCapabilities(", "void VDA::withdrawCapabilities("):
        methods += function(va, marker) + "\n"
    for marker in ("bool VCNBridge::loadReady(", "void VCNBridge::setAccelerator(",
                   "void VCNBridge::failMedia(", "bool VCNBridge::initHardware("):
        methods += function(bridge, marker) + "\n"
    with tempfile.TemporaryDirectory(prefix="nred-media-policy-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(STUBS + headers + keys + methods + TESTS)
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-I", str(ROOT / "NootedRed"),
                        str(unit), "-o", str(binary)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
