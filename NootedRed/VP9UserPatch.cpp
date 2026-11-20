#include <DeviceProfile.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatchMemory.hpp>
#include <UserSharedCache.hpp>
#include <UserVP9DecodeResolver.hpp>
#include <VCNBridge.hpp>
#include <VDA.hpp>
#include <VP9UserPatch.hpp>
#include <kern/task.h>
#include <mach/i386/vm_param.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <sys/errno.h>
#include <sys/proc.h>

extern "C" vm_map_t current_map(void);

static VP9UserPatch moduleInstance;

namespace
{

    constexpr const char* kBootArgNoVDA = "-NRedNoVDA";
    constexpr const char* kBootArgNoVP9 = "-NRedNoVP9";

    struct SharedRegionCheckArgs
    {
        user_addr_t startAddress;
    };
    static_assert(sizeof(SharedRegionCheckArgs) == 8);

    using SharedRegionCheck = int (*)(proc_t, SharedRegionCheckArgs*, int*);
    using GetTaskMap        = vm_map_t (*)(task_t);
    using RegionRecurse     = UserPatchMemory::RegionQuery;
    using CopyIn            = int (*)(user_addr_t, void*, size_t);
    using CopyOut           = int (*)(const void*, user_addr_t, size_t);
    using MapDestroy        = void (*)(vm_map_t);

    mach_vm_address_t orgSharedRegionCheck{0};
    MapDestroy        orgMapDestroy{nullptr};
    GetTaskMap        getTaskMap{nullptr};
    RegionRecurse     queryRegion{nullptr};
    CopyIn            readUser{nullptr};
    CopyOut           writeUser{nullptr};

    enum class State : UInt32
    {
        Off       = 0,
        Resolving = 1,
        Ready     = 2,
        Failed    = 3,
    };

    constexpr UInt32 MaxSites = UserVP9DecodeResolver::MaxSites;
    constexpr UInt32 MaxPages = MaxSites * 2;
    constexpr size_t MaxMaps  = 128;

    IOLock*                      stateLock{nullptr};
    State                        resolveState{State::Off};
    UInt64                       resolvedBase{0};
    UInt8                        resolvedUUID[16]{};
    UserVP9DecodeResolver::Plan* cachedPlan{nullptr};
    vm_map_t                     patchedMaps[MaxMaps]{};
    UInt32                       appliedCount{0};
    UInt32                       alreadyCount{0};
    UInt32                       darwinMajor{0};

    struct Workspace
    {
        struct Row
        {
            UInt8 bytes[UserVP9DecodeData::MaxPatchSize];
        } saved[MaxSites];
        UserPatchMemory::Page pages[MaxPages];
    };

    void publishState(const State s) { NRed::singleton().setProp32("NRedVP9State", static_cast<UInt32>(s)); }

    void reportVmError(const kern_return_t kr)
    {
        NRed::singleton().setProp32("NRedVP9LastVMError", static_cast<UInt32>(kr));
    }

    bool isMapPatched(const vm_map_t map)
    {
        IOLockLock(stateLock);
        bool found = false;
        for (const auto m : patchedMaps) {
            if (m == map) {
                found = true;
                break;
            }
        }
        IOLockUnlock(stateLock);
        return found;
    }

    void markMapPatched(const vm_map_t map)
    {
        IOLockLock(stateLock);
        for (auto& m : patchedMaps) {
            if (m == map) {
                IOLockUnlock(stateLock);
                return;
            }
        }
        for (auto& m : patchedMaps) {
            if (m == nullptr) {
                m = map;
                IOLockUnlock(stateLock);
                return;
            }
        }
        // Evict slot 0 round-robin if table is full
        patchedMaps[0] = map;
        IOLockUnlock(stateLock);
    }

    void forgetMap(const vm_map_t map)
    {
        if (stateLock == nullptr || map == nullptr) { return; }
        IOLockLock(stateLock);
        for (auto& m : patchedMaps) {
            if (m == map) { m = nullptr; }
        }
        IOLockUnlock(stateLock);
    }

    bool readCacheHeader(const UInt64 base, UInt8 (&uuid)[16])
    {
        UInt8 header[0x68]{};
        if (readUser(base, header, sizeof(header)) != 0) { return false; }
        static constexpr UInt8 kMagic[] = "dyld_v1 ";
        if (memcmp(header, kMagic, 8) != 0) { return false; }
        memcpy(uuid, header + 0x58, sizeof(uuid));
        return true;
    }

    bool ensureResolved(const UInt64 base, const UInt8 (&uuid)[16])
    {
        IOLockLock(stateLock);
        if (resolveState == State::Ready) {
            const bool same = resolvedBase == base && memcmp(resolvedUUID, uuid, sizeof(resolvedUUID)) == 0;
            IOLockUnlock(stateLock);
            return same;
        }
        if (resolveState != State::Off || cachedPlan == nullptr) {
            IOLockUnlock(stateLock);
            return false;
        }
        resolveState = State::Resolving;
        IOLockUnlock(stateLock);

        auto* const scan = IONew(UInt8, UserSharedCache::Input::ScanSize);
        bool        ok   = false;
        if (scan != nullptr) {
            memset(cachedPlan, 0, sizeof(*cachedPlan));
            UserSharedCache::Input input{readUser, {}};
            input.scanBuffer = scan;
            ok               = UserVP9DecodeResolver::resolve(input, base, *cachedPlan, darwinMajor);
            IODelete(scan, UInt8, UserSharedCache::Input::ScanSize);
        }

        IOLockLock(stateLock);
        resolveState = ok ? State::Ready : State::Off;
        resolvedBase = ok ? base : 0;
        if (ok) { memcpy(resolvedUUID, uuid, sizeof(resolvedUUID)); }
        IOLockUnlock(stateLock);

        publishState(ok ? State::Ready : State::Failed);
        if (!ok) { SYSLOG("VP9", "Shared cache did not match VP9 backend requirements; disabled"); }
        return ok;
    }

    bool executablePage(const vm_map_t map, const mach_vm_address_t pageAddress)
    {
        vm_prot_t protection = VM_PROT_NONE;
        return UserPatchMemory::leafProtection(map, pageAddress, protection, queryRegion, reportVmError) &&
               protection == (VM_PROT_READ | VM_PROT_EXECUTE);
    }

    bool applyTransaction(UserPatchMemory::ExecutorOps<CopyIn, CopyOut>& ops, const UInt64 base,
                          const UserVideoDecodeData::Patch* const patches, const UInt32 siteCount,
                          Workspace& work, const UInt32 pageCount)
    {
        for (UInt32 p = 0; p < pageCount; ++p) {
            if (!ops.check(work.pages[p])) { return false; }
        }
        for (UInt32 p = 0; p < pageCount; ++p) {
            if (!ops.makeWritable(work.pages[p])) {
                ops.restore(work.pages, pageCount);
                return false;
            }
        }

        UInt32 written = 0;
        bool   ok      = true;
        for (UInt32 i = 0; i < siteCount; ++i) {
            const auto& site    = patches[i];
            const auto  address = base + site.offset;
            if (ops.write(site.patched, address, site.size) != 0) {
                ok = false;
                break;
            }
            written = i + 1;
            UInt8 verify[UserVP9DecodeData::MaxPatchSize]{};
            if (ops.read(address, verify, site.size) != 0 || memcmp(verify, site.patched, site.size) != 0) {
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (UInt32 i = 0; i < written; ++i) {
                const auto& site = patches[i];
                ops.prepareRollback(work.pages, pageCount, base + site.offset, site.size);
                ops.write(work.saved[i].bytes, base + site.offset, site.size);
            }
            ops.restore(work.pages, pageCount);
            return false;
        }

        return ops.restore(work.pages, pageCount);
    }

    __attribute__((noinline)) void processSharedRegion(const UInt64 base)
    {
        if (base == 0 || base >= UserSharedCache::UserLimit) { return; }
        if (!VCNBridge::singleton().userPatchesAllowed()) { return; }
        if (resolveState == State::Failed) { return; }

        const auto map = getTaskMap != nullptr ? getTaskMap(current_task()) : current_map();
        if (map == nullptr || isMapPatched(map)) { return; }

        UInt8 uuid[16]{};
        if (!readCacheHeader(base, uuid)) { return; }
        if (!ensureResolved(base, uuid)) {
            VCNBridge::singleton().setVP9Ready(false);
            return;
        }

        auto* const work = IONew(Workspace, 1);
        if (work == nullptr) {
            reportVmError(KERN_RESOURCE_SHORTAGE);
            return;
        }
        memset(work, 0, sizeof(*work));

        const auto siteCount = cachedPlan->count;
        UInt32     pageCount = 0;
        UInt32     originals = 0;
        UInt32     patched   = 0;
        bool       valid     = siteCount > 0 && siteCount <= MaxSites;

        for (UInt32 i = 0; valid && i < siteCount; ++i) {
            const auto& site    = cachedPlan->patches[i];
            const auto  address = base + site.offset;
            if (site.size == 0 || site.size > UserVP9DecodeData::MaxPatchSize ||
                site.offset >= UserSharedCache::UserLimit - base ||
                readUser(address, work->saved[i].bytes, site.size) != 0) {
                valid = false;
                break;
            }
            if (memcmp(work->saved[i].bytes, site.original, site.size) == 0) {
                ++originals;
            } else if (memcmp(work->saved[i].bytes, site.patched, site.size) == 0) {
                ++patched;
            } else {
                valid = false;
            }
            if (valid && !UserPatchMemory::collectPages(work->pages, pageCount, address, site.size)) {
                valid = false;
            }
        }

        if (!valid || (originals != siteCount && patched != siteCount)) {
            NRed::singleton().setProp32("NRedVP9Mixed", 1);
            VCNBridge::singleton().setVP9Ready(false);
            IODelete(work, Workspace, 1);
            return;
        }

        if (patched == siteCount) {
            bool allExec = true;
            for (UInt32 p = 0; allExec && p < pageCount; ++p) {
                allExec = executablePage(map, work->pages[p].address);
            }
            IODelete(work, Workspace, 1);
            if (allExec) {
                markMapPatched(map);
                VCNBridge::singleton().setVP9Ready(true);
                NRed::singleton().setProp32("NRedVTDecoderAdmissionRevision", 1);
                const auto count = __atomic_fetch_add(&alreadyCount, 1U, __ATOMIC_RELAXED);
                if (count < 16) { NRed::singleton().setProp32("NRedVP9Already", count + 1); }
            } else {
                VCNBridge::singleton().setVP9Ready(false);
            }
            return;
        }

        UserPatchMemory::ExecutorOps<CopyIn, CopyOut> ops{map, queryRegion, reportVmError, readUser, writeUser};
        const bool installed = applyTransaction(ops, base, cachedPlan->patches, siteCount, *work, pageCount);
        IODelete(work, Workspace, 1);

        if (installed) {
            markMapPatched(map);
            VCNBridge::singleton().setVP9Ready(true);
            NRed::singleton().setProp32("NRedVTDecoderAdmissionRevision", 1);
            const auto count = __atomic_fetch_add(&appliedCount, 1U, __ATOMIC_RELAXED);
            if (count < 16) { NRed::singleton().setProp32("NRedVP9Applied", count + 1); }
        } else {
            IOLockLock(stateLock);
            resolveState = State::Failed;
            IOLockUnlock(stateLock);
            publishState(State::Failed);
            VCNBridge::singleton().setVP9Ready(false);
            SYSLOG("VP9", "COW transaction failed; VP9 decode disabled for this boot");
        }
    }

    int wrapSharedRegionCheck(proc_t proc, SharedRegionCheckArgs* const args, int* const retval)
    {
        const int result = FunctionCast(wrapSharedRegionCheck, orgSharedRegionCheck)(proc, args, retval);
        if (result == 0 && args != nullptr) {
            UInt64 base = 0;
            if (readUser != nullptr && readUser(args->startAddress, &base, sizeof(base)) == 0 && base != 0) {
                processSharedRegion(base);
            }
        }
        return result;
    }

    void wrapMapDestroy(const vm_map_t map)
    {
        forgetMap(map);
        if (orgMapDestroy != nullptr) { orgMapDestroy(map); }
    }

}    // namespace

VP9UserPatch& VP9UserPatch::singleton() { return moduleInstance; }

void VP9UserPatch::init()
{
    const auto major = currentKernelVersion().major();
    darwinMajor      = static_cast<UInt32>(major);

    if (checkKernelArgument(kBootArgNoVDA) || checkKernelArgument(kBootArgNoVP9)) {
        this->enabled = false;
        DBGLOG("VP9", "VP9 hardware decode disabled by boot argument");
        return;
    }

    if (major < 22 || major > 25) {
        this->enabled = false;
        SYSLOG("VP9", "VP9 inline backend supports Darwin 22–25 (macOS 13–26); disabled on Darwin %ld",
               static_cast<long>(major));
        return;
    }

    stateLock  = IOLockAlloc();
    cachedPlan = IONew(UserVP9DecodeResolver::Plan, 1);
    if (stateLock == nullptr || cachedPlan == nullptr) {
        this->enabled = false;
        if (cachedPlan != nullptr) {
            IODelete(cachedPlan, UserVP9DecodeResolver::Plan, 1);
            cachedPlan = nullptr;
        }
        if (stateLock != nullptr) {
            IOLockFree(stateLock);
            stateLock = nullptr;
        }
        SYSLOG("VP9", "Failed to allocate VP9 state; disabled");
        return;
    }

    this->enabled = true;
}

void VP9UserPatch::processPatcher(KernelPatcher& patcher)
{
    if (!this->enabled) {
        publishState(State::Off);
        return;
    }

    const auto* const profile = DeviceProfile::current();
    if (profile == nullptr || !profile->media.hasVCN0 || profile->media.vcnGeneration != DeviceProfile::kVcn2_2 ||
        NRed::singleton().hasExternalVideo()) {
        this->enabled = false;
        publishState(State::Off);
        return;
    }

    getTaskMap  = reinterpret_cast<GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
    queryRegion = reinterpret_cast<RegionRecurse>(patcher.solveSymbol(KernelPatcher::KernelID, "_mach_vm_region_recurse"));
    readUser    = reinterpret_cast<CopyIn>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyin"));
    writeUser   = reinterpret_cast<CopyOut>(patcher.solveSymbol(KernelPatcher::KernelID, "_copyout"));
    if (queryRegion == nullptr || readUser == nullptr || writeUser == nullptr) {
        patcher.clearError();
        this->enabled = false;
        publishState(State::Failed);
        SYSLOG("VP9", "Missing kernel VM helpers for VP9 COW patcher; disabled");
        return;
    }

    KernelPatcher::RouteRequest routes[] = {
        {"_shared_region_check_np", wrapSharedRegionCheck, orgSharedRegionCheck},
        {"_vm_map_destroy", wrapMapDestroy, orgMapDestroy},
    };
    if (!patcher.routeMultiple(KernelPatcher::KernelID, routes, arrsize(routes))) {
        patcher.clearError();
        this->enabled = false;
        publishState(State::Failed);
        SYSLOG("VP9", "Failed to route shared-region hooks for VP9; disabled");
        return;
    }

    SYSLOG("VP9", "VP9 inline backend patcher active for Darwin %u (macOS 13–26)", darwinMajor);
}
