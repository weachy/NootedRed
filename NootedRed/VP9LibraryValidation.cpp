// Adapted from wh0ami7/vcn-video-decoders vcnvtkext/kern_amfi.cpp.
// Copyright (c) 2026 DrogaBox and Izzy. MIT license; see Media/VP9/LICENSE.
// This optional route admits one exact VP9 plugin cdhash, never AV1 or other code.
#include <VP9LibraryValidation.hpp>
#include <VP9PluginAllowlist.inc>
#include <UserPatches.hpp>
#include <NRed.hpp>
#include <VDA.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <PenguinWizardry/KernelVersion.hpp>

namespace {
    const char* kAMFIPaths[] = {
        "/System/Library/Extensions/AppleMobileFileIntegrity.kext/Contents/MacOS/AppleMobileFileIntegrity"
    };
    KernelPatcher::KextInfo kextAMFI {
        "com.apple.driver.AppleMobileFileIntegrity", kAMFIPaths, arrsize(kAMFIPaths),
        {true}, {}, KernelPatcher::KextInfo::Unloaded
    };
    bool enabled = false;
    using LibraryValidation = int (*)(void*, void*, void*, UInt64, UInt64, UInt64, UInt64);
    using CDHash = const UInt8* (*)(void*);
    LibraryValidation originalValidation = nullptr;
    CDHash copyCDHash = nullptr;

    int validateLibrary(void* process, void* file, void* facts,
                        UInt64 arg4, UInt64 arg5, UInt64 arg6, UInt64 arg7)
    {
        if (file != nullptr && copyCDHash != nullptr) {
            const auto* hash = copyCDHash(file);
            if (hash != nullptr &&
                memcmp(hash, VP9PluginAllowlist::CDHashBytes, sizeof(VP9PluginAllowlist::CDHashBytes)) == 0) {
                // AMFI's existing "Library Validation overridden" return value.
                return 1;
            }
        }
        return originalValidation != nullptr
            ? originalValidation(process, file, facts, arg4, arg5, arg6, arg7) : 0;
    }
}

void VP9LibraryValidation::init()
{
    enabled = checkKernelArgument("-NRedVP9") && !checkKernelArgument("-NRedNoVDA")
              && getKernelVersion() == KernelVersion::Tahoe;
    if (enabled) {
        lilu.onKextLoadForce(&kextAMFI);
    }
}

void VP9LibraryValidation::processKext(KernelPatcher& patcher, const size_t index,
                                      const mach_vm_address_t address, const size_t size)
{
    if (!enabled || index != kextAMFI.loadIndex ||
        !VDA::singleton().canAdvertise()) {
        return;
    }
    // Both symbols verified in 25G332's BootKernelExtensions.kc.
    copyCDHash = patcher.solveSymbol<CDHash>(
        index, "__ZN22AbstractCSBlobAccessor6cdHashEv", address, size);
    if (copyCDHash == nullptr) {
        SYSLOG("VP9", "Cannot resolve AMFI cdhash accessor; plugin library support remains disabled");
        patcher.clearError();
        return;
    }
    KernelPatcher::RouteRequest request {
        "__ZL27_library_validation_failureR15ProcessAccessorR16FileGlobAccessorR11SystemFactsbPKcym",
        validateLibrary, originalValidation
    };
    if (patcher.routeMultiple(index, &request, 1, address, size)) {
        NRed::singleton().setProp32("NRedVP9LibraryValidation", 1);
        SYSLOG("VP9", "Enabled library validation for paired VP9 cdhash %s", VP9PluginAllowlist::CDHash);
    } else {
        SYSLOG("VP9", "Cannot route AMFI library validation (%d); leaving existing policy in place",
               static_cast<int>(patcher.getError()));
        patcher.clearError();
    }
}
