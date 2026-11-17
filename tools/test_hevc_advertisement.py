#!/usr/bin/env python3
"""Run the production Main10 personality update with retained-object stubs."""
from pathlib import Path
import subprocess
import tempfile

from test_userpatch_lifecycle import function

ROOT = Path(__file__).resolve().parents[1]
STUBS = r"""
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
int live = 0, allocation = -1, insertion = -1;
bool permitted(int& countdown) {
    if (countdown < 0) return true;
    return countdown-- != 0;
}
struct OSObject {
    int refs = 1;
    OSObject() { ++live; }
    virtual ~OSObject() { --live; }
    void retain() { ++refs; }
    void release() { if (!--refs) delete this; }
};
#define OSDynamicCast(type, value) dynamic_cast<type*>(value)
struct OSDictionary : OSObject {
    std::map<std::string, OSObject*> values;
    ~OSDictionary() { for (auto& kv : values) kv.second->release(); }
    OSObject* getObject(const char* key) {
        auto it = values.find(key); return it == values.end() ? nullptr : it->second;
    }
    bool setObject(const char* key, OSObject* value) {
        if (!permitted(insertion)) return false;
        value->retain();
        auto old = getObject(key); if (old) old->release();
        values[key] = value; return true;
    }
    static OSDictionary* withDictionary(OSDictionary* source) {
        if (!permitted(allocation)) return nullptr;
        auto* copy = new OSDictionary;
        for (auto& kv : source->values) { kv.second->retain(); copy->values[kv.first] = kv.second; }
        return copy;
    }
};
struct OSArray : OSObject {
    std::vector<OSObject*> values;
    ~OSArray() { for (auto* value : values) value->release(); }
    static OSArray* withCapacity(unsigned) { return permitted(allocation) ? new OSArray : nullptr; }
    bool setObject(OSObject* value) {
        if (!permitted(insertion)) return false;
        value->retain(); values.push_back(value); return true;
    }
};
struct OSNumber : OSObject {
    unsigned value;
    explicit OSNumber(unsigned v) : value(v) {}
    static OSNumber* withNumber(unsigned long long v, unsigned) {
        return permitted(allocation) ? new OSNumber(v) : nullptr;
    }
};
struct OSString : OSObject {
    std::string value;
    explicit OSString(const char* v) : value(v) {}
    static OSString* withCString(const char* value) {
        return permitted(allocation) ? new OSString(value) : nullptr;
    }
};
"""
TESTS = r"""
void put(OSDictionary* dictionary, const char* key, OSObject* value) {
    assert(dictionary->setObject(key, value)); value->release();
}
int main() {
    assert(!enableHEVC10Encode(nullptr));
    assert(!enableHEVC8KDecode(nullptr));
    unsigned checks = 0;
    for (int failAllocation = -1; failAllocation < 6; ++failAllocation) {
        for (int failInsertion = -1; failInsertion < 7; ++failInsertion) {
            allocation = insertion = -1;
            auto* personality = new OSDictionary;
            auto* caps = new OSDictionary;
            auto* details = new OSDictionary;
            auto* main = new OSDictionary;
            put(main, "VTMaxEncodeLevel", OSNumber::withNumber(153, 32));
            put(details, "1", main);
            put(caps, "VTPerProfileDetails", details);
            auto* profiles = OSArray::withCapacity(1);
            auto* one = OSNumber::withNumber(1, 32);
            assert(profiles->setObject(one)); one->release();
            put(caps, "VTSupportedProfileArray", profiles);
            // Retain the old caps to model another personality sharing them.
            caps->retain();
            put(personality, "IOGVAHEVCEncodeCapabilities", caps);
            put(personality, "IOGVAHEVCEncode", OSString::withCString("1"));
            allocation = failAllocation; insertion = failInsertion;
            bool result = enableHEVC10Encode(personality);
            auto* level = OSDynamicCast(OSString, personality->getObject("IOGVAHEVCEncode"));
            assert(level && level->value == (result ? "2" : "1"));
            assert(details->getObject("2") == nullptr && profiles->values.size() == 1);
            if (result) {
                auto* updated = OSDynamicCast(OSDictionary, personality->getObject("IOGVAHEVCEncodeCapabilities"));
                auto* newDetails = OSDynamicCast(OSDictionary, updated->getObject("VTPerProfileDetails"));
                auto* newProfiles = OSDynamicCast(OSArray, updated->getObject("VTSupportedProfileArray"));
                assert(newDetails->getObject("1") == main && newDetails->getObject("2") == main);
                assert(newProfiles->values.size() == 2);
                assert(OSDynamicCast(OSNumber, newProfiles->values[0])->value == 1);
                assert(OSDynamicCast(OSNumber, newProfiles->values[1])->value == 2);
                assert(OSDynamicCast(OSNumber, main->getObject("VTMaxEncodeLevel"))->value == 153);
            }
            personality->release(); caps->release();
            assert(live == 0);
            ++checks;
        }
    }
    for (bool mainOnly : {false, true}) {
        for (int failAllocation = -1; failAllocation < 7; ++failAllocation) {
            for (int failInsertion = -1; failInsertion < 9; ++failInsertion) {
                allocation = insertion = -1;
                auto* personality = new OSDictionary;
                auto* caps = new OSDictionary;
                auto* details = new OSDictionary;
                for (const auto* key : {"1", "2", "3"}) {
                    if (mainOnly && std::string(key) != "1") continue;
                    auto* entry = new OSDictionary;
                    put(entry, "VTMaxDecodeLevel", OSNumber::withNumber(153, 32));
                    put(details, key, entry);
                }
                put(caps, "VTPerProfileDetails", details);
                caps->retain();
                put(personality, "IOGVAHEVCDecodeCapabilities", caps);
                allocation = failAllocation; insertion = failInsertion;
                bool result = enableHEVC8KDecode(personality);
                auto* updated = OSDynamicCast(OSDictionary, personality->getObject("IOGVAHEVCDecodeCapabilities"));
                auto* newDetails = OSDynamicCast(OSDictionary, updated->getObject("VTPerProfileDetails"));
                assert(result || updated == caps);
                for (const auto* key : {"1", "2", "3"}) {
                    auto* oldEntry = OSDynamicCast(OSDictionary, details->getObject(key));
                    auto* newEntry = OSDynamicCast(OSDictionary, newDetails->getObject(key));
                    assert((oldEntry == nullptr) == (newEntry == nullptr));
                    if (oldEntry) {
                        assert(OSDynamicCast(OSNumber, oldEntry->getObject("VTMaxDecodeLevel"))->value == 153);
                        assert(OSDynamicCast(OSNumber, newEntry->getObject("VTMaxDecodeLevel"))->value ==
                               (result ? 186 : 153));
                    }
                }
                personality->release(); caps->release();
                assert(live == 0);
                ++checks;
            }
        }
    }
    std::printf("PASS %u capability/ownership/failure cases; no GPU work\n", checks);
}
"""


def main():
    source = (ROOT / "NootedRed/DriverInjector.cpp").read_text()
    helper = (function(source, "static bool enableHEVC10Encode(") +
              function(source, "static bool enableHEVC8KDecode("))
    with tempfile.TemporaryDirectory(prefix="nred-hevc-capabilities-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(STUBS + helper + TESTS)
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", str(unit), "-o", str(binary)], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
