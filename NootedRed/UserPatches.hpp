// User-Space Driver Patches
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <Headers/kern_user.hpp>
#include <libkern/OSTypes.h>

// Patches Apple's user-space OpenGL drivers, which is where the Chromium/GL corruption originates.
//
// Lilu's user patcher only operates on macOS 10.9 through 10.15; upstream Lilu hard-disables it on
// Big Sur and newer, and even where it is enabled it only wraps `_cs_validate_range`, while a
// vnode-backed bundle such as `AMDRadeonX5000GLDriver` is validated through `_cs_validate_page` on
// macOS 11 and newer.  This module therefore has two backends, selected by kernel version:
//
//  - macOS 10.15 and older: `lilu.onProcLoad` with `UserPatcher::BinaryModInfo`/`BinaryModPatch`
//    entries.  No process filter is registered on purpose, so that the patcher never injects the
//    `__RESTRICT` segment into user processes; page patches are global in Lilu and do not need one.
//  - macOS 11 and newer: a self-contained `_cs_validate_page`/`_cs_validate_range` hook.  A complete
//    `kPageSize` page image has to match byte for byte before `find` is replaced with `replace`, so a
//    patch can never hit an unrelated binary.  See analysis/lilu-userpatch.md for the details.
//
// GLFix is enabled by default (pass -NRedNoGLFix to opt out); VDA hardware video acceleration is also
// enabled by default (pass -NRedNoHEVC to disable HEVC, or -NRedNoVDA to disable VDA completely).
// Both log under the `UserPatch` tag.
class UserPatches
{
public:
    // APU families this module can target.  A patch set may be limited to a subset; `kApuAll` is used by
    // the shared page-image patches, which are safe for every supported gfx9 APU (the exact page image
    // still gates them, so an unsupported APU is a no-op).
    enum ApuMask : UInt8 {
        kApuRaven        = 1U << 0,
        kApuRaven2       = 1U << 1,
        kApuPicasso      = 1U << 2,
        kApuRenoir       = 1U << 3,
        kApuRenoirE      = 1U << 4,
        kApuGreenSardine = 1U << 5,
        kApuAll          = 0x3F,
    };

    struct PagePatch;
    struct SignaturePatch;

    // A patch set valid for a macOS version range and an APU-family subset.  The first matching set is
    // selected once per boot; page images remain the last line of defence, so a set whose images do not
    // match the running driver simply does nothing.
    struct PatchSet
    {
        const char*                     name;
        PenguinWizardry::KernelVersion  minVersion;
        PenguinWizardry::KernelVersion  maxVersion;
        ApuMask                         apus;
        const PagePatch*                patches;
        size_t                          count;

        // Optional resolver entries used when the exact page image does not match (for example another
        // macOS build).  Only active with the `-NRedGLFixResolve` boot argument.
        const SignaturePatch*           signatures{nullptr};
        size_t                          signatureCount{0};
    };

    // A masked-context signature used by the resolver backend.  `context` contains a window around the
    // patch site; 0xFF mask bytes must match, 0x00 bytes are wildcards.  `patchOffset` is relative to the
    // start of the matched context.  `pageOffset` can restrict the signature to a file/cache page offset;
    // pass UINT64_MAX for no restriction.
    struct SignaturePatch
    {
        const UInt8* context;
        const UInt8* mask;
        size_t       contextSize;
        size_t       patchOffset;
        const UInt8* replace;
        size_t       replaceSize;
        size_t       skip;
        size_t       count;
        UInt64       pageOffset;
        const UInt8* find{nullptr};
        size_t       findSize{0};
    };

    // Lookup-style patch for one user-space Mach-O image.
    struct PagePatch
    {
        // Mach segment/section containing the patch; only used by the Lilu backend.
        UserPatcher::FileSegment segment;

        // Exact `kPageSize` byte image of the file page that contains `find`, or `nullptr` when the
        // patch is only meant for the Lilu backend.  Required by the native backend.
        const UInt8* page;

        // Byte sequence to look for and the bytes written in its place (`size` bytes each).
        const UInt8* find;
        const UInt8* replace;

        // Number of bytes in `find` and `replace`.
        size_t size;

        // Number of occurrences of `find` to skip before patching each occurrence.
        size_t skip;

        // Number of occurrences of `find` to patch.
        size_t count;
    };

    // User-space OpenGL driver this module exists to patch.
    static constexpr const char* kGLDriverPath =
        "/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver";

    // Code signing page size used by both backends.
    static constexpr size_t kPageSize = 0x1000;

    static UserPatches& singleton();

    // Parses the boot arguments and records the enabled tables.  Must be called from `NRed::init`;
    // `processPatcher` installs the selected backend once the device profile is known.
    void init();

    // Replaces the built-in GL driver patch table.  `patchTable` has to point to static storage, as it
    // Replaces the built-in GL driver patch table (deprecated, dynamic resolver in use).
    void registerGLDriverFix(const PagePatch* = nullptr, size_t = 0) {}

    // Replaces the built-in version/family-aware patch-set table (deprecated, dynamic resolver in use).
    void registerPatchSets(const PatchSet* = nullptr, size_t = 0) {}

    // Adds the -NRedVDA user-space patch table (deprecated, dynamic resolver in use).
    void registerVDAFix(const PagePatch* = nullptr, size_t = 0) {}

    // Installs the dynamic signature patch backend. Called from `NRed::processPatcher` once the device ID and
    // profile are known (it must run before user processes validate pages).
    void processPatcher(KernelPatcher& patcher);

private:
    // Maximum amount of signature patches in a patch set.
    static constexpr size_t kMaxSignatures    = 8;
    static constexpr size_t kMaxVDASignatures = 16;

    const SignaturePatch* signatures{nullptr};
    size_t                signatureCount{0};
    size_t                signatureApplied[kMaxSignatures]{};
    const SignaturePatch* vdaSignatures{nullptr};
    size_t                vdaSignatureCount{0};
    size_t                vdaSignatureApplied[kMaxVDASignatures]{};
    const SignaturePatch* gvaSignature{nullptr};
    size_t                gvaSignatureApplied{0};
    bool                  resolverEnabled{false};
    bool                  resolverOnly{false};

    bool                  enabled{false};
    bool                  vdaEnabled{false};

    void selectPatchSet();
    auto currentApuMask() const -> ApuMask;

    SInt32                vdaAppliedCount{0};

    mach_vm_address_t orgCsValidatePage{0};
    mach_vm_address_t orgCsValidateRange{0};

    auto installNativePagePatcher(KernelPatcher& patcher) -> bool;
    void patchSignaturePage(const void* data, UInt64 pageOffset, void* vnode);

    static void      wrapCsValidatePage(void* vnode, void* pager, UInt64 pageOffset, const void* data, int* validated,
                                        int* tainted, int* nx);
    static boolean_t wrapCsValidateRange(void* vnode, void* pager, UInt64 pageOffset, const void* data, UInt64 dataSize,
                                         unsigned* tainted);
};
