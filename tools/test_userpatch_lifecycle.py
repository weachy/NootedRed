#!/usr/bin/env python3
"""Exercise the production page resolver without loading a kext or using the GPU.

The header, signature tables, matcher and patch method are taken directly from
the checkout (or --baseline-ref). Only kernel services are stubbed. Fresh buffers
model fresh validated file pages; this does not simulate XNU page reclamation.
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


STUBS = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <CommonCrypto/CommonDigest.h>
using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using SInt32 = int32_t;
using mach_vm_address_t = uint64_t;
using boolean_t = int;
using vnode_t = const char*;
namespace PenguinWizardry { using KernelVersion = int; }
struct UserPatcher { enum FileSegment { SegmentTextText }; };
constexpr int KERN_SUCCESS = 0;
struct KernelPatcher { static inline int kernelWriteLock; };
struct IOSimpleLock {};
void IOSimpleLockLock(IOSimpleLock*) {}
void IOSimpleLockUnlock(IOSimpleLock*) {}
namespace MachInfo {
    bool rejectWrites = false;
    unsigned writes = 0, restores = 0;
    int setKernelWriting(bool enabled, int) {
        if (enabled && rejectWrites) return 1;
        enabled ? ++writes : ++restores;
        return KERN_SUCCESS;
    }
}
const char* vnode_getname(vnode_t p) { return p; }
void vnode_putname(const char*) {}
// MacKernelSDK OSAtomic.h specifies that OSIncrementAtomic returns the OLD value.
template<class T> T OSIncrementAtomic(volatile T* p) {
    const auto old = *p;
    *p = old + 1;
    return old;
}
bool OSCompareAndSwap(UInt32 old, UInt32 value, volatile UInt32* p) {
    if (*p != old) return false;
    *p = value; return true;
}
namespace Buffer {
    bool reject = false;
    template<class T> T* create(size_t count) { return reject ? nullptr : (T*)std::malloc(sizeof(T) * count); }
    template<class T> void deleter(T* p) { if (!p) std::abort(); std::free(p); }
}
namespace Crypto {
    bool reject = false;
    UInt8* hash(const UInt8* bytes, UInt32 size) {
        if (reject) return nullptr;
        auto* result = Buffer::create<UInt8>(32);
        CC_SHA256(bytes, size, result);
        return result;
    }
}
char osversion[32] = "25G332";
struct VCNBridge {
    bool ready = true, failed = false;
    unsigned failure = 0;
    static VCNBridge& singleton() { static VCNBridge b; return b; }
    bool userPatchesAllowed() const { return ready && !failed; }
    void failMedia(UInt32 reason) { if (!failed) { failed = true; failure = reason; } }
};
unsigned logCalls = 0;
#define SYSLOG(...) do { ++logCalls; } while (0)
#define arrsize(x) (sizeof(x) / sizeof((x)[0]))
struct NRed {
    std::map<std::string, UInt32> props;
    static NRed& singleton() { static NRed n; return n; }
    void setProp32(const char* key, UInt32 value) { props[key] = value; }
};
"""

TESTS = r"""
using Page = std::array<UInt8, UserPatches::kPageSize>;
using Sig = UserPatches::SignaturePatch;
unsigned checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", name.c_str()); }
}
void invoke(UserPatches& p, Page& page, const char* name, UInt64 offset = 0) {
    p.patchSignaturePage(page.data(), offset, (void*)name);
}
void put(Page& p, const Sig& sig, size_t offset) {
    std::memcpy(p.data() + offset, sig.context, sig.contextSize);
    if (sig.find) std::memcpy(p.data() + offset + sig.patchOffset, sig.find, sig.findSize);
}
bool replaced(const Page& p, const Sig& s, size_t offset) {
    return std::memcmp(p.data() + offset + s.patchOffset, s.replace, s.replaceSize) == 0;
}
UserPatches graphics(const Sig* sigs) {
    UserPatches p;
    p.signatures = sigs; p.signatureCount = 5; p.resolverEnabled = true;
    p.vdaEnabled = false;
    return p;
}
void lifecycle(UserPatches& p, const Page& original, const char* vnode,
               const Sig& sig, size_t offset, const std::string& label) {
    for (int fault = 0; fault < 4; ++fault) {
        auto page = original;
        invoke(p, page, vnode);
        check(replaced(page, sig, offset), label + " fresh fault " + std::to_string(fault));
        auto after = page;
        const auto writes = MachInfo::writes;
        invoke(p, page, vnode);
        check(page == after && writes == MachInfo::writes, label + " repeated callback");
    }
    auto unrelated = original;
    invoke(p, unrelated, "unrelated.bin");
    check(unrelated == original, label + " unrelated vnode");
    auto corrupt = original;
    size_t fixed = 0;
    while (fixed < sig.contextSize &&
           (sig.mask[fixed] == 0 ||
            (fixed >= sig.patchOffset && fixed < sig.patchOffset + sig.replaceSize))) ++fixed;
    if (fixed < sig.contextSize) {
        corrupt[offset + fixed] ^= sig.mask[fixed] & -sig.mask[fixed];
        const auto before = corrupt;
        invoke(p, corrupt, vnode);
        check(std::memcmp(corrupt.data() + offset, before.data() + offset, sig.contextSize) == 0,
              label + " corrupt context rejected");
    }
}
void graphicsTests(const Sig* sigs, const char* table) {
    for (size_t index = 0; index < 3; ++index) {
        auto p = graphics(sigs);
        Page page{}; put(page, sigs[index], 0x100);
        lifecycle(p, page, "AMDRadeonX5000GLDriver", sigs[index], 0x100,
                  std::string(table) + " GL " + std::to_string(index));
    }
    {
        auto p = graphics(sigs);
        Page original{}; put(original, sigs[0], 0x100);
        std::memcpy(original.data() + 0x500, kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind));
        std::memcpy(original.data() + 0x700, kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind));
        for (int fault = 0; fault < 4; ++fault) {
            auto page = original;
            invoke(p, page, "dyld_shared_cache_x86_64h.03");
            check(replaced(page, sigs[0], 0x100) &&
                  std::memcmp(page.data()+0x500, kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace)) == 0 &&
                  std::memcmp(page.data()+0x700, kGLDriverHwInfoReplace, sizeof(kGLDriverHwInfoReplace)) == 0,
                  std::string(table) + " Metal HwInfo all branches/fresh page");
            auto after = page; auto writes = MachInfo::writes;
            invoke(p, page, "dyld_shared_cache_x86_64h.03");
            check(page == after && writes == MachInfo::writes, "Metal HwInfo repeated callback");
        }
    }
    {
        auto p = graphics(sigs);
        Page original{}; put(original, sigs[3], 0x100); put(original, sigs[4], 0x280);
        for (int fault = 0; fault < 4; ++fault) {
            auto page = original;
            invoke(p, page, "dyld_shared_cache_x86_64h.03");
            check(replaced(page, sigs[3], 0x100) && replaced(page, sigs[4], 0x280),
                  std::string(table) + " Metal pair fresh page");
            auto after = page; auto writes = MachInfo::writes;
            invoke(p, page, "dyld_shared_cache_x86_64h.03");
            check(page == after && writes == MachInfo::writes, "Metal pair repeated callback");
        }
        auto absent = Page{}; put(absent, sigs[3], 0x100);
        const auto before = absent;
        invoke(p, absent, "dyld_shared_cache_x86_64h.03");
        check(absent == before, "Metal pair rejects missing partner");
        auto distant = Page{}; put(distant, sigs[3], 0x100); put(distant, sigs[4], 0x800);
        const auto beforeDistant = distant;
        invoke(p, distant, "dyld_shared_cache_x86_64h.03");
        check(distant == beforeDistant, "Metal pair rejects distant partner");
        auto retry = original;
        MachInfo::rejectWrites = true;
        invoke(p, retry, "dyld_shared_cache_x86_64h.03");
        check(retry == original, "Metal pair write failure changes neither partner");
        MachInfo::rejectWrites = false;
        invoke(p, retry, "dyld_shared_cache_x86_64h.03");
        check(replaced(retry,sigs[3],0x100) && replaced(retry,sigs[4],0x280), "Metal pair write retry");
    }
}
#ifndef NRED_HAS_MEDIA_MANIFEST
void mediaTests(const Sig* sigs, size_t count, const Sig* gva, const char* table) {
    for (size_t index = 0; index < count; ++index) {
        UserPatches p; p.resolverEnabled = false; p.vdaEnabled = true;
        p.vdaSignatures = sigs + index; p.vdaSignatureCount = 1;
        Page page{};
        for (size_t site = 0; site < sigs[index].count; ++site) put(page, sigs[index], 0x100 + site * 0x100);
        lifecycle(p, page, "AMDRadeonVADriver2", sigs[index], 0x100,
                  std::string(table) + " VDA " + std::to_string(index));
        auto fresh = page; invoke(p, fresh, "AMDRadeonVADriver2");
        for (size_t site = 0; site < sigs[index].count; ++site)
            check(replaced(fresh, sigs[index], 0x100 + site * 0x100), "VDA all sites on fresh page");
    }
    UserPatches p; p.resolverEnabled = false; p.vdaEnabled = true; p.gvaSignature = gva;
    Page page{}; put(page, *gva, 0x100);
    lifecycle(p, page, "AppleGVA", *gva, 0x100, std::string(table) + " AppleGVA");
}
#else
void resetBridge(bool ready = true) {
    auto& b = VCNBridge::singleton(); b.ready = ready; b.failed = false; b.failure = 0;
}
UserPatches media() {
    UserPatches p; p.vdaEnabled = true; p.nativeBackendReady = true; p.kernelIdentityMatches = true; return p;
}
MediaPatchPlan::Page fixturePlan(Page& original) {
    size_t index = 0;
    while (index < arrsize(MediaPatchManifest::Pages) &&
           MediaPatchManifest::Pages[index].siteCount < 3) ++index;
    if (index == arrsize(MediaPatchManifest::Pages)) {
        check(false, "manifest provides a multi-site guard fixture");
        std::abort();
    }
    auto plan = MediaPatchManifest::Pages[index];
    for (size_t i = 0; i < plan.siteCount; ++i) {
        const auto& site = plan.sites[i];
        std::memcpy(original.data() + site.offset, site.find, site.size);
    }
    Page normalized{}; bool needsWrite = false;
    check(MediaPatchPlan::normalize(original.data(), plan, normalized.data(), needsWrite), "valid multi-site fixture");
    CC_SHA256(normalized.data(), normalized.size(), plan.maskedSHA256);
    return plan;
}
void mediaGuardTests() {
    std::array<UInt8, 56> kernel{};
    auto word = [&](size_t at, UInt32 value) { std::memcpy(kernel.data() + at, &value, 4); };
    word(0, 0xFEEDFACF); word(16, 1); word(20, 24); word(32, 0x1B); word(36, 24);
    std::memcpy(kernel.data() + 40, MediaPatchManifest::KernelUUID, 16);
    check(MediaPatchPlan::matchesKernelUUID(kernel.data(), kernel.size(), MediaPatchManifest::KernelUUID),
          "bounded kernel Mach-O UUID recognized");
    for (size_t n = 0; n < kernel.size(); ++n)
        check(!MediaPatchPlan::matchesKernelUUID(kernel.data(), n, MediaPatchManifest::KernelUUID),
              "truncated kernel load commands rejected");
    for (size_t n = 40; n < kernel.size(); ++n) {
        kernel[n] ^= 1;
        check(!MediaPatchPlan::matchesKernelUUID(kernel.data(), kernel.size(), MediaPatchManifest::KernelUUID),
              "other kernel UUID rejected");
        kernel[n] ^= 1;
    }
    word(36, 0x10000);
    check(!MediaPatchPlan::matchesKernelUUID(kernel.data(), kernel.size(), MediaPatchManifest::KernelUUID),
          "out-of-bounds kernel load command rejected");
    Page original{}; auto plan = fixturePlan(original);
    auto allPatched = [&](const Page& p) {
        for (size_t i = 0; i < plan.siteCount; ++i) {
            const auto& s = plan.sites[i];
            if (std::memcmp(p.data() + s.offset, s.replace, s.size)) return false;
        }
        return true;
    };
    auto p = media(); resetBridge();
    for (int fault = 0; fault < 5; ++fault) {
        auto page = original;
        p.patchMediaPage(page.data(), plan, 2);
        check(allPatched(page), "all media sites patched on every fresh fault");
        auto writes = MachInfo::writes;
        p.patchMediaPage(page.data(), plan, 2);
        check(allPatched(page) && writes == MachInfo::writes, "media repeated callback does not write");
    }
    auto mixed = original;
    std::memcpy(mixed.data() + plan.sites[0].offset, plan.sites[0].replace, plan.sites[0].size);
    p.patchMediaPage(mixed.data(), plan, 2);
    check(allPatched(mixed), "mixed original/patched page finishes all required sites");
    for (bool siteCorruption : {false, true}) {
        resetBridge(); auto page = original;
        page[siteCorruption ? plan.sites[2].offset : 0x80] ^= 0x20;
        const auto before = page; auto writes = MachInfo::writes;
        p.patchMediaPage(page.data(), plan, 2);
        check(page == before && writes == MachInfo::writes, "mismatch writes none of the partners");
        check(VCNBridge::singleton().failed && VCNBridge::singleton().failure == 21, "mismatch latches media failure");
        page = original; p.patchMediaPage(page.data(), plan, 2);
        check(page == original, "failure also stops subsequent valid media pages");
    }
    resetBridge(false);
    auto page = original; p.patchMediaPage(page.data(), plan, 2);
    check(page == original, "unallocated/failed engine cannot patch pages");
    resetBridge(); // no successful initHardware is needed to bootstrap the first session
    p.patchMediaPage(page.data(), plan, 2);
    check(allPatched(page), "allocated engine can patch before first hardware session");
    for (const char* build : {"25G241", "25G333", "24H32"}) {
        resetBridge(); std::strcpy(osversion, build); page = original;
        p.patchMediaPage(page.data(), plan, 2);
        check(page == original && !p.mediaSupported(), "unknown build cannot patch media");
    }
    std::strcpy(osversion, MediaPatchManifest::Build);
    resetBridge(); p = media(); p.kernelIdentityMatches = false;
    page = original; p.patchMediaPage(page.data(), plan, 2);
    check(page == original && !p.mediaSupported(), "unknown kernel cannot enable media");
    p.kernelIdentityMatches = true; std::strcpy(osversion, "");
    p.patchMediaPage(page.data(), plan, 2);
    check(allPatched(page), "validated kernel UUID supports bootstrap before OS build is published");
    std::strcpy(osversion, MediaPatchManifest::Build);
    for (bool failHash : {false, true}) {
        resetBridge(); p = media(); page = original;
        Buffer::reject = !failHash; Crypto::reject = failHash;
        p.patchMediaPage(page.data(), plan, 2);
        Buffer::reject = Crypto::reject = false;
        check(page == original && VCNBridge::singleton().failure == 20, "allocation/hash failure contains media without null free");
    }
    resetBridge(); p = media();
    MachInfo::rejectWrites = true; page = original;
    p.patchMediaPage(page.data(), plan, 2); MachInfo::rejectWrites = false;
    check(page == original && VCNBridge::singleton().failure == 22, "write failure leaves entire media page unchanged");
    resetBridge();
    for (const char* name : {"dyld", "other-dyld_shared_cache_x86_64h.03", "dyld_shared_cache_x86_64h.03.bak",
                             "AMDRadeonVADriver2", "AppleGVA", "unrelated.bin"}) {
        page = original;
        invoke(p, page, name, plan.offset);
        check(page == original && !VCNBridge::singleton().failed, "reject unrelated basename before hashing");
    }
    page = original; invoke(p, page, plan.file, plan.offset + 4096);
    check(page == original && !VCNBridge::singleton().failed, "reject incorrect media file page");
    resetBridge();
}
void hostMediaPages(const char* path) {
    if (!path) return;
    auto p = media(); resetBridge();
    for (size_t i = 0; i < arrsize(MediaPatchManifest::Pages); ++i) {
        const auto& plan = MediaPatchManifest::Pages[i];
        std::ifstream file(std::string(path) + "/page-" + std::to_string(i) + ".bin", std::ios::binary);
        Page snapshot{}; file.read((char*)snapshot.data(), snapshot.size());
        check(file.gcount() == snapshot.size(), "host media snapshot has exactly one page");
        for (int fault = 0; fault < 4; ++fault) {
            auto page = snapshot;
            for (size_t j = 0; j < plan.siteCount; ++j) {
                const auto& site = plan.sites[j];
                std::memcpy(page.data() + site.offset, site.find, site.size);
            }
            invoke(p, page, plan.file, plan.offset);
            for (size_t j = 0; j < plan.siteCount; ++j) {
                const auto& site = plan.sites[j];
                check(std::memcmp(page.data() + site.offset, site.replace, site.size) == 0,
                      "actual host media page: every required site/fresh fault");
            }
            check(!VCNBridge::singleton().failed, "actual page fingerprint accepted");
        }
    }
    check(NRed::singleton().props["NRedMediaPatchComplete"] == 1, "all required manifest pages observed");
}
#endif
void hostPages(const char* path) {
    if (!path || !*path) return;
    std::ifstream f(path, std::ios::binary);
    std::vector<UInt8> image((std::istreambuf_iterator<char>(f)), {});
    check(image.size() == 1809872, "host GL snapshot size");
    if (image.size() != 1809872) return;
    for (size_t i = 0; i < 2; ++i) {
        const size_t site = i == 0 ? 0xe0c1 : 0x81ca;
        const size_t pageOffset = site & ~size_t(4095);
        const auto& sig = kResolverSignatures_Tahoe[i];
        Page page{};
        std::memcpy(page.data(), image.data() + pageOffset, page.size());
        check(std::memcmp(page.data() + site-pageOffset, sig.replace, sig.replaceSize) == 0,
              "host GL snapshot contains expected live patch");
        std::memcpy(page.data() + site-pageOffset, sig.find, sig.findSize);
        auto p = graphics(kResolverSignatures_Tahoe);
        lifecycle(p, page, "AMDRadeonX5000GLDriver", sig, site-pageOffset-sig.patchOffset,
                  "actual host GL page " + std::to_string(i));
    }
}
#ifdef NRED_HAS_GL_BUDGET
void budgetTests() {
    using namespace GLResourceBudgetPatch;
    for (size_t off : {size_t(0), size_t(0x72a), Page{}.size() - sizeof(Find)}) {
        Page original{};
        std::memcpy(original.data() + off, Find, sizeof(Find));
        auto p = graphics(kResolverSignatures_Tahoe);
        for (int fault = 0; fault < 4; ++fault) {
            auto page = original;
            invoke(p, page, "AMDRadeonX5000GLDriver", 0x5000);
            check(std::memcmp(page.data() + off, Replace, sizeof(Replace)) == 0,
                  "budget correction on each fresh validated GL page");
            auto after = page;
            const auto writes = MachInfo::writes;
            invoke(p, page, "AMDRadeonX5000GLDriver", 0x5000);
            check(page == after && MachInfo::writes == writes, "budget repeated callback is a no-op");
        }
        for (const char* name : {"unrelated.bin", "dyld_shared_cache_x86_64h.03", "AMDRadeonVADriver2"}) {
            auto page = original;
            invoke(p, page, name);
            check(page == original, "budget correction rejects non-GL image");
        }
        auto disabled = p;
        disabled.resolverEnabled = false;
        auto page = original;
        invoke(disabled, page, "AMDRadeonX5000GLDriver");
        check(page == original, "budget follows resolver opt-out");
        MachInfo::rejectWrites = true;
        invoke(p, page, "AMDRadeonX5000GLDriver");
        check(page == original, "budget write failure leaves page unchanged");
        MachInfo::rejectWrites = false;
        invoke(p, page, "AMDRadeonX5000GLDriver");
        check(std::memcmp(page.data() + off, Replace, sizeof(Replace)) == 0,
              "budget retries after write failure");
        for (size_t i = 0; i < sizeof(Find); ++i) {
            auto corrupt = original;
            corrupt[off + i] ^= 1;
            auto before = corrupt;
            invoke(p, corrupt, "AMDRadeonX5000GLDriver");
            check(corrupt == before, "budget rejects altered function byte " + std::to_string(i));
        }
    }
    Page ambiguous{};
    std::memcpy(ambiguous.data() + 0x100, Find, sizeof(Find));
    std::memcpy(ambiguous.data() + 0x300, Find, sizeof(Find));
    auto before = ambiguous;
    auto p = graphics(kResolverSignatures_Tahoe);
    invoke(p, ambiguous, "AMDRadeonX5000GLDriver");
    check(ambiguous == before, "budget rejects ambiguous match");
    for (size_t size = 0; size < sizeof(Find); ++size) {
        size_t off = 0;
        check(!find(Find, size, off), "budget rejects truncated input");
    }
    size_t off = 0;
    check(!find(nullptr, Page{}.size(), off), "budget rejects null input");
}
#endif
void vp9RegistryTests() {
#ifdef NRED_HAS_VP9_POLICY
    using namespace VP9PluginAllowlist;
    Page original{};
    std::memcpy(original.data() + Offset, Original, sizeof(Original));
    UserPatches p;
#ifdef NRED_HAS_MEDIA_MANIFEST
    p = media(); resetBridge();
#endif
    p.vp9RegistryEnabled = true;
    for (int fault = 0; fault < 4; ++fault) {
        auto page = original;
        invoke(p, page, "dyld_shared_cache_x86_64h", PageOffset);
        check(std::memcmp(page.data() + Offset, Replacement, sizeof(Replacement)) == 0,
              "VP9 policy applies to fresh page");
        auto after = page;
        const auto writes = MachInfo::writes;
        invoke(p, page, "dyld_shared_cache_x86_64h", PageOffset);
        check(page == after && MachInfo::writes == writes, "VP9 policy is idempotent");
    }
    for (size_t i = 0; i < sizeof(Original); ++i) {
        auto page = original;
        page[Offset + i] ^= 1;
        const auto before = page;
        invoke(p, page, "dyld_shared_cache_x86_64h", PageOffset);
        check(page == before, "VP9 rejects changed policy byte " + std::to_string(i));
    }
    for (const auto* name : {"dyld_shared_cache_x86_64h.01", "dyld_shared_cache_x86_64",
                             "dyld_shared_cache_other", "unrelated.bin"}) {
        auto page = original;
        invoke(p, page, name, PageOffset);
        check(page == original, std::string("VP9 rejects vnode ") + name);
    }
    for (auto offset : {0ULL, PageOffset + 4096, PageOffset - 4096}) {
        auto page = original;
        invoke(p, page, "dyld_shared_cache_x86_64h", offset);
        check(page == original, "VP9 rejects other cache pages");
    }
    auto page = original;
    p.vp9RegistryEnabled = false;
    invoke(p, page, "dyld_shared_cache_x86_64h", PageOffset);
    check(page == original, "VP9 policy defaults off");
    p.vp9RegistryEnabled = true;
    MachInfo::rejectWrites = true;
    invoke(p, page, "dyld_shared_cache_x86_64h", PageOffset);
    MachInfo::rejectWrites = false;
    check(page == original, "VP9 leaves policy intact if writing fails");
#endif
}
int main(int argc, char** argv) {
    graphicsTests(kResolverSignatures_Ventura, "Ventura");
    graphicsTests(kResolverSignatures_Sonoma, "Sonoma");
    graphicsTests(kResolverSignatures, "Sequoia");
    graphicsTests(kResolverSignatures_Tahoe, "Tahoe");
#ifndef NRED_HAS_MEDIA_MANIFEST
    mediaTests(kVDAResolverSignatures_Ventura, arrsize(kVDAResolverSignatures_Ventura),
               &kGVAResolverSignature_Ventura, "Ventura");
    mediaTests(kVDAResolverSignatures_Sonoma, arrsize(kVDAResolverSignatures_Sonoma),
               &kGVAResolverSignature_Sonoma, "Sonoma");
    mediaTests(kVDAResolverSignatures_Modern, arrsize(kVDAResolverSignatures_Modern),
               &kGVAResolverSignature_Sequoia, "Sequoia");
    mediaTests(kVDAResolverSignatures_Modern, arrsize(kVDAResolverSignatures_Modern),
               &kGVAResolverSignature_Tahoe, "Tahoe");
#else
    mediaGuardTests();
    hostMediaPages(argc > 2 ? argv[2] : nullptr);
#endif
    hostPages(argc > 1 ? argv[1] : nullptr);
#ifdef NRED_HAS_GL_BUDGET
    budgetTests();
#endif
    vp9RegistryTests();
    check(MachInfo::writes == MachInfo::restores, "every successful write restores protection");
    printf("%u checks, %u failures; no GPU work submitted\n", checks, failures);
    return failures ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", help="test repository source at a Git ref")
    parser.add_argument("--host-gl-snapshot", type=Path)
    parser.add_argument("--media-pages", type=Path)
    args = parser.parse_args()

    def read(name):
        if args.baseline_ref:
            return subprocess.check_output(
                ["git", "show", f"{args.baseline_ref}:NootedRed/{name}"],
                cwd=REPO, text=True,
            )
        return (REPO / "NootedRed" / name).read_text()

    header = re.sub(r"^#(?:include|pragma).*$", "", read("UserPatches.hpp"), flags=re.M)
    header = header.replace("private:", "public:")
    source = read("UserPatches.cpp")
    media_header = ""
    if "MediaPatchManifest::" in source:
        media_header = "#define NRED_HAS_MEDIA_MANIFEST 1\n" + re.sub(
            r"^#(?:include|pragma).*$", "", read("MediaPatchPlan.hpp"), flags=re.M)
        media_header += re.sub(r"^#pragma.*$", "", read("MediaPatchManifest.inc"), flags=re.M)
    budget_header = ""
    if "GLResourceBudgetPatch::find" in source:
        budget_header = "#define NRED_HAS_GL_BUDGET 1\n" + re.sub(
            r"^#(?:include|pragma).*$", "",
            read("GLResourceBudgetPatch.hpp"), flags=re.M,
        )
    policy_header = ""
    if "VP9PluginAllowlist::" in source:
        policy_header = "#define NRED_HAS_VP9_POLICY 1\n" + re.sub(
            r"^#pragma.*$", "", read("VP9PluginAllowlist.inc"), flags=re.M,
        )
    start = source.index("namespace\n{")
    end = source.index("}    // namespace", start) + 1
    tables = source[start:end].replace('#include "VDASignatures.inc"', read("VDASignatures.inc"))
    methods = function(source, "static bool matchSignature(") + "\n"
    if media_header:
        methods += function(source, "bool UserPatches::mediaSupported(") + "\n"
        methods += function(source, "void UserPatches::patchMediaPage(") + "\n"
    if "void UserPatches::tracePatch(" in source:
        methods += function(source, "void UserPatches::tracePatch(") + "\n"
    if "void UserPatches::recordCachedSite(" in source:
        methods += function(source, "void UserPatches::recordCachedSite(") + "\n"
        methods += function(source, "void UserPatches::reapplyCachedSites(") + "\n"
    methods += function(source, "void UserPatches::patchSignaturePage(")
    with tempfile.TemporaryDirectory(prefix="nred-page-test-") as temp:
        unit, binary = Path(temp) / "test.cpp", Path(temp) / "test"
        unit.write_text(STUBS + media_header + budget_header + policy_header + header + tables + methods + TESTS)
        subprocess.run([
            "xcrun", "clang++", "-std=c++17", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
            "-Wno-unused-const-variable", "-Wno-deprecated-declarations", str(unit), "-o", str(binary),
        ], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", str(binary)], check=True)
        cmd = [str(binary)]
        if args.host_gl_snapshot or args.media_pages:
            cmd.append(str(args.host_gl_snapshot.resolve()) if args.host_gl_snapshot else "")
        if args.media_pages:
            cmd.append(str(args.media_pages.resolve()))
        return subprocess.run(cmd).returncode


if __name__ == "__main__":
    raise SystemExit(main())
