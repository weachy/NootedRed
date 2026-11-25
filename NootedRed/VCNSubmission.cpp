#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <RenoirDriverBuild.hpp>
#include <VCNBridge.hpp>
#include <VCNSubmission.hpp>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <sys/sysctl.h>

namespace VCNSubmission
{
    namespace
    {
        IOSimpleLock* historyLock{};
        bool hardwareLibraryMatched{};
        bool routesReady{};
        bool unmapTraceReady{};
        bool writebackTraceReady{};
        void* activeEngine{};
        void* activeHardware{};
        uint64_t engineStart{};
        uint32_t videoVmids{};
        uintptr_t hardwareTable{};
        uintptr_t channelTables[QueueCount]{};
        mach_vm_address_t orgEncode{}, orgDecode{}, orgReset{}, orgReport{}, orgFreeMappings{}, orgTimestampUpdated{};
        mach_vm_address_t orgEngineInit{}, orgEngineFree{}, orgUnmapVA{};
        Timeline live{};
        FirstReset firstReset{};
        ActiveTaskReleases activeReleases{};

        struct QueueState
        {
            Failure failure{};
            uint64_t failures{};
            Writebacks writebacks{};
        };
        QueueState queues[QueueCount]{};

        int readSubmissions(struct sysctl_oid* oid, void*, int, struct sysctl_req* request)
        {
            uint64_t records[QueueCount], failures[QueueCount], writebacks[QueueCount];
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            for (size_t index = 0; index < QueueCount; ++index) {
                records[index] = live.submissions[index].next;
                failures[index] = __atomic_load_n(&queues[index].failures, __ATOMIC_RELAXED);
                writebacks[index] = queues[index].writebacks.updates;
            }
            const bool captured = firstReset.captured;
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            char text[320];
            snprintf(text, sizeof(text), "VCN0Dec=%llu/%llu VCN0Enc=%llu/%llu VCN0EncLLQ=%llu/%llu "
                     "writebacks=%llu/%llu/%llu writebackTrace=%u firstReset=%u unmapVA=%u",
                     records[0], failures[0], records[1], failures[1], records[2], failures[2],
                     writebacks[0], writebacks[1], writebacks[2], writebackTraceReady ? 1U : 0U, captured ? 1U : 0U,
                     unmapTraceReady ? 1U : 0U);
            return sysctl_handle_string(oid, text, sizeof(text), request);
        }
        SYSCTL_PROC(_debug, OID_AUTO, nred_vcn_submissions, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
                    readSubmissions, "A", "VCN submission records and failures per queue");

        int queueIndex(void* channel)
        {
            const auto engine = __atomic_load_n(&activeEngine, __ATOMIC_ACQUIRE);
            if (!engine || getMember<void*>(channel, 0x28) != engine || !VCNBridge::singleton().engineEnabled()) {
                return -1;
            }
            const auto table = getMember<uintptr_t>(channel, 0);
            for (size_t index = 0; index < QueueCount; ++index) {
                if (table == channelTables[index]) { return static_cast<int>(index); }
            }
            return -1;
        }

        struct CommandInfo { uint32_t opcode, bytes, reserved; };
        struct IBUpdate
        {
            uint32_t opcode, reserved, fields, vmid;
            uint64_t address;
            uint32_t bytes, padding;
        };
        static_assert(sizeof(IBUpdate) == 32 && offsetof(IBUpdate, bytes) == 0x18);

        using Commit = uint32_t (*)(void*, const Submission*);
        struct Port
        {
            void* channel;
            uint32_t queue;
            Commit original;

            static uint32_t invalid() { return static_cast<uint32_t>(kIOReturnBadArgument); }
            uint32_t replay(const Submission& job) const { return original(channel, &job); }
            Failure failure() const
            {
                Failure value;
                __atomic_load(&queues[queue].failure, &value, __ATOMIC_ACQUIRE);
                return value;
            }

            void record(const Submission& job, Path path, uint32_t status) const
            {
                SubmitEvent event{};
                event.time = mach_absolute_time();
                event.stamp = job.stamp;
                event.softwareStamp = job.softwareStamp;
                event.softwareChannel = job.softwareChannel;
                event.flags = job.flags;
                event.vmid = job.vmid;
                event.count = job.count;
                event.path = path;
                event.error = status;
                for (size_t index = 0; index < job.count && index < MaxIBs; ++index) {
                    event.words[index] = job.ib[index].dwords;
                    event.addresses[index] = job.ib[index].gpuAddress;
                }
                const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                live.submissions[queue].append(event);
                if ((path == Path::Primary || path == Path::Auxiliary) && job.vmid && job.vmid < 32) {
                    const auto known = __atomic_load_n(&videoVmids, __ATOMIC_RELAXED);
                    const auto mask = 1U << job.vmid;
                    if (!(known & mask)) { __atomic_store_n(&videoVmids, known | mask, __ATOMIC_RELAXED); }
                }
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            }

            uint32_t reject(const Submission& job, uint32_t status) const
            {
                Failure previous{};
                Failure first{status, job.stamp - 1U};
                const bool stored = __atomic_compare_exchange(&queues[queue].failure, &previous, &first, false,
                                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                __atomic_add_fetch(&queues[queue].failures, uint64_t{1}, __ATOMIC_RELAXED);
                record(job, Path::Rejected, status);
                return stored ? first.precedingStamp : previous.precedingStamp;
            }

            uint32_t updateIB(bool decode, const IndirectBuffer& ib, uint32_t vmid, uint32_t* destination) const
            {
                auto* ttl = getMember<void*>(channel, 0x140);
                const auto instance = getMember<uint32_t>(channel, 0x138);
                const auto table = getMember<const uintptr_t*>(ttl, 0);
                using Query = uint32_t (*)(void*, uint32_t, uint32_t, uint32_t, CommandInfo*);
                using Build = uint32_t (*)(void*, uint32_t, uint32_t, IBUpdate*, uint32_t*);
                CommandInfo info{};
                auto status = reinterpret_cast<Query>(table[0x90 / sizeof(void*)])(
                    ttl, queue, instance, decode ? 1U : 0x14U, &info);
                if (status) { return status; }
                if (info.bytes != (decode ? Decode.ib : Encode.ib) * sizeof(uint32_t)) { return invalid(); }
                IBUpdate update{info.opcode, 0, 0xE, vmid, ib.gpuAddress, ib.dwords * 4, 0};
                return reinterpret_cast<Build>(table[0x98 / sizeof(void*)])(ttl, queue, instance, &update, destination);
            }

            uint32_t submit(uint32_t* frame) const
            {
                auto* ttl = getMember<void*>(channel, 0x140);
                const auto table = getMember<const uintptr_t*>(ttl, 0);
                using Submit = uint32_t (*)(void*, uint32_t, uint32_t, void*);
                return reinterpret_cast<Submit>(table[0xA0 / sizeof(void*)])(
                    ttl, queue, getMember<uint32_t>(channel, 0x138), frame);
            }

            uint32_t queueFrame(const Submission& job, Path path, bool decode)
            {
                if (!getMember<void*>(channel, 0x140) || getMember<uint32_t>(channel, 0x138) != 0
                    || getMember<uint32_t>(channel, 0x150) != queue) { return invalid(); }
                return submitFrame(job, path, decode, getMember<const uint32_t*>(channel, 0x158),
                                   getMember<uint32_t>(channel, 0xD8), getMember<uint32_t>(channel, 0x14C),
                                   getMember<uint64_t>(channel, 0xC8), *this);
            }
        };

        template<bool DecodeChannel>
        uint32_t commitChannel(void* channel, const Submission* job)
        {
            auto original = reinterpret_cast<Commit>(DecodeChannel ? orgDecode : orgEncode);
            const auto index = queueIndex(channel);
            if (!job || index < 0 || (index == 0) != DecodeChannel) { return original(channel, job); }
            Port port{channel, static_cast<uint32_t>(index), original};
            return commit(*job, DecodeChannel, port);
        }

        void timestampUpdated(void* channel)
        {
            const auto index = queueIndex(channel);
            if (index >= 0) {
                const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                const auto* writeback = getMember<const volatile uint32_t*>(channel, 0xC0);
                const auto& cached = getMember<volatile uint32_t>(channel, 0x84);
                if (writeback && writeback != &cached) { queues[index].writebacks.observe(cached, *writeback); }
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            }
            FunctionCast(timestampUpdated, orgTimestampUpdated)(channel);
        }

        void resetChannel(void* channel)
        {
            const auto index = queueIndex(channel);
            if (index >= 0) {
                const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                const auto* writeback = getMember<const volatile uint32_t*>(channel, 0xC0);
                if (!firstReset.captured && writeback && writeback != &getMember<uint32_t>(channel, 0x84)) {
                    firstReset.capture({mach_absolute_time(), getMember<uint32_t>(channel, 0xC),
                                        getMember<uint32_t>(channel, 0x80), getMember<uint32_t>(channel, 0x84),
                                        *writeback}, live);
                }
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            }
            FunctionCast(resetChannel, orgReset)(channel);
            if (index >= 0) {
                Failure cleared{};
                __atomic_store(&queues[index].failure, &cleared, __ATOMIC_RELEASE);
            }
        }

        void freeMappings(void* task)
        {
            auto original = FunctionCast(freeMappings, orgFreeMappings);
            if (!__atomic_load_n(&activeEngine, __ATOMIC_ACQUIRE) || !VCNBridge::singleton().engineEnabled()) {
                return original(task);
            }
            auto* accelerator = getMember<IOService*>(task, 0x10);
            if (!accelerator || accelerator->getProvider() != NRed::singleton().getIGPU()) { return original(task); }
            auto* context = getMember<void*>(task, 0x260);
            MappingRelease release{};
            release.mmVmid = context ? getMember<uint32_t>(context, 0x11C) : 0;
            release.begin = mach_absolute_time();
            TaskReleaseScope scope{current_thread(), task, release.begin};
            auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            activeReleases.enter(scope);
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            original(task);
            release.end = mach_absolute_time();
            release.pid = getMember<int32_t>(task, 0xAD0);
            irq = IOSimpleLockLockDisableInterrupt(historyLock);
            activeReleases.leave(scope);
            if (release.begin >= engineStart) { live.releases.append(release); }
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
        }

        void unmapVA(void* context, uint64_t address, uint64_t bytes)
        {
            const auto original = FunctionCast(unmapVA, orgUnmapVA);
            const auto hardware = __atomic_load_n(&activeHardware, __ATOMIC_ACQUIRE);
            if (!hardware || hardware != getMember<void*>(context, 0x10)) { return original(context, address, bytes); }
            const auto vmids = __atomic_load_n(&videoVmids, __ATOMIC_RELAXED);
            if (!vmids) { return original(context, address, bytes); }
            const auto vmid = getMember<uint32_t>(context, 0x11C);
            if (!eligibleUnmapRange(vmids, vmid, address, bytes, getMember<uint64_t>(context, 0x80),
                                    getMember<uint64_t>(context, 0x88))) {
                return original(context, address, bytes);
            }
            const auto* task = getMember<void*>(context, 0x28);
            UnmapRange range{mach_absolute_time(), 0, address, bytes, 0, vmid};
            original(context, address, bytes);
            range.end = mach_absolute_time();
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            if (range.begin >= engineStart && activeReleases.associate(range, current_thread(), task)) {
                live.unmaps.append(range);
            }
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
        }

        __attribute__((format(printf, 3, 4)))
        void appendReport(char*& cursor, uint32_t& remaining, const char* format, ...)
        {
            if (!cursor || remaining < 2) { return; }
            va_list args;
            va_start(args, format);
            const auto length = vsnprintf(cursor, remaining, format, args);
            va_end(args);
            if (length <= 0) { return; }
            const auto used = static_cast<uint32_t>(length) < remaining ? static_cast<uint32_t>(length) : remaining - 1;
            cursor += used;
            remaining -= used;
        }

        template<typename Entry, size_t Capacity, typename Print>
        void printHistory(const History<Entry, Capacity>& history, uint64_t end, uint64_t liveStart, Print emit)
        {
            for (auto serial = end > Capacity ? end - Capacity : 0; serial < end; ++serial) {
                Entry entry;
                const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                const bool available = (!liveStart || engineStart == liveStart) && history.read(serial, entry);
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
                if (available) { emit(entry); }
            }
        }

        void reportChannel(void* channel, char*& cursor, uint32_t& remaining)
        {
            FunctionCast(reportChannel, orgReport)(channel, cursor, remaining);
            const auto index = queueIndex(channel);
            if (index < 0) { return; }
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            const bool captured = firstReset.captured;
            const auto stamps = firstReset.stamps;
            const auto& timeline = captured ? firstReset.timeline : live;
            const auto liveStart = captured ? 0 : engineStart;
            const auto submissions = timeline.submissions[index].next;
            const auto releases = timeline.releases.next;
            const auto unmaps = timeline.unmaps.next;
            const auto& current = live.submissions[index];
            SubmitEvent latest{};
            const bool hasLatest = captured && current.next && (engineStart > stamps.time || current.next != submissions)
                && current.read(current.next - 1, latest);
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            if (captured) {
                appendReport(cursor, remaining, "  NRedVCN first-reset t=%llu hw=%u submitted=%x cached=%x writeback=%x\n",
                             stamps.time, stamps.channel, stamps.submitted, stamps.lastRead, stamps.writeback);
            }
            auto printSubmission = [&](const SubmitEvent& event, const char* prefix) {
                constexpr const char* paths[]{"primary", "fence", "auxiliary", "rejected", "replay"};
                appendReport(cursor, remaining, "  NRedVCN %st=%llu hw=%u stamp=%x sw=%u/%x flags=%x vmid=%u "
                             "path=%s error=%x count=%u", prefix, event.time, getMember<uint32_t>(channel, 0xC),
                             event.stamp, event.softwareChannel, event.softwareStamp, event.flags, event.vmid,
                             paths[static_cast<size_t>(event.path)], event.error, event.count);
                for (size_t slot = 0; slot < event.count && slot < MaxIBs; ++slot) {
                    appendReport(cursor, remaining, " ib%zu=%llx/%u", slot, event.addresses[slot], event.words[slot]);
                }
                appendReport(cursor, remaining, "\n");
            };
            printHistory(timeline.submissions[index], submissions, liveStart,
                         [&](const SubmitEvent& event) { printSubmission(event, ""); });
            if (hasLatest) { printSubmission(latest, "latest "); }
            if (index == 0) {
                printHistory(timeline.releases, releases, liveStart, [&](const MappingRelease& release) {
                    appendReport(cursor, remaining, "  NRedVCN freeMappings t=%llu..%llu pid=%d mmVMID=%u\n",
                                 release.begin, release.end, release.pid, release.mmVmid);
                });
                appendReport(cursor, remaining, "  NRedVCN unmapVA enabled=%u records=%llu\n",
                             unmapTraceReady ? 1U : 0U, unmaps);
                printHistory(timeline.unmaps, unmaps, liveStart, [&](const UnmapRange& range) {
                    appendReport(cursor, remaining, "  NRedVCN unmapVA t=%llu..%llu mmVMID=%u va=%llx bytes=%llx release=%llu\n",
                                 range.begin, range.end, range.mmVmid, range.address, range.bytes, range.taskRelease);
                });
            }
        }

        void detachEngine(void* engine)
        {
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            const bool active = engine == __atomic_load_n(&activeEngine, __ATOMIC_RELAXED);
            if (active) {
                __atomic_store_n(&activeHardware, nullptr, __ATOMIC_RELEASE);
                __atomic_store_n(&activeEngine, nullptr, __ATOMIC_RELEASE);
            }
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            if (active) { NRed::singleton().setProp32("NRedVCNOrderedFence", 0); }
        }

        bool initEngine(void* engine, void* hardware, uint32_t type)
        {
            const auto original = FunctionCast(initEngine, orgEngineInit);
            if (!__atomic_load_n(&routesReady, __ATOMIC_ACQUIRE) || !__atomic_load_n(&hardwareLibraryMatched, __ATOMIC_ACQUIRE)
                || !VCNBridge::singleton().engineEnabled() || !hardware || getMember<uintptr_t>(hardware, 0) != hardwareTable
                || getMember<IOPCIDevice*>(hardware, 0x10) != NRed::singleton().getIGPU()) {
                return original(engine, hardware, type);
            }
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            Failure cleared{};
            for (auto& queue : queues) {
                __atomic_store(&queue.failure, &cleared, __ATOMIC_RELEASE);
                __atomic_store_n(&queue.failures, uint64_t{0}, __ATOMIC_RELAXED);
                queue.writebacks = {};
            }
            for (auto& history : live.submissions) { history.next = 0; }
            live.releases.next = live.unmaps.next = 0;
            engineStart = mach_absolute_time();
            __atomic_store_n(&videoVmids, 0U, __ATOMIC_RELAXED);
            __atomic_store_n(&activeHardware, hardware, __ATOMIC_RELEASE);
            __atomic_store_n(&activeEngine, engine, __ATOMIC_RELEASE);
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            const auto result = original(engine, hardware, type);
            if (result) { NRed::singleton().setProp32("NRedVCNOrderedFence", 1); }
            else { detachEngine(engine); }
            return result;
        }

        void freeEngine(void* engine)
        {
            detachEngine(engine);
            FunctionCast(freeEngine, orgEngineFree)(engine);
        }

        bool bridgeValidated(const char* property)
        {
            auto* gpu = NRed::singleton().getIGPU();
            auto* value = gpu ? gpu->copyProperty(property) : nullptr;
            auto* number = OSDynamicCast(OSNumber, value);
            const bool validated = number && number->unsigned32BitValue() == 1;
            OSSafeReleaseNULL(value);
            return validated;
        }

        bool prepareProducer(KernelPatcher& patcher, size_t id, uintptr_t slide, size_t size)
        {
            const auto table = patcher.solveSymbol(id, "__ZTV32AMDRadeonX5000_AMDVega10Hardware", slide, size);
            if (!table) {
                patcher.clearError();
                return false;
            }
            historyLock = IOSimpleLockAlloc();
            if (!historyLock) { return false; }
            KernelPatcher::RouteRequest release{"__ZN27AMDRadeonX5000_AMDAccelTask18freeAllGPUMappingsEv",
                                                freeMappings, orgFreeMappings};
            if (!patcher.routeMultiple(id, &release, 1, slide, size)) {
                patcher.clearError();
                IOSimpleLockFree(historyLock);
                historyLock = nullptr;
                return false;
            }
            constexpr uint8_t unmapHead[]{0x48, 0x3B, 0xB7, 0x80, 0, 0, 0};
            constexpr char unmapSymbol[] = "__ZN29AMDRadeonX5000_AMDHWVMContext7unmapVAEyy";
            const auto address = patcher.solveSymbol(id, unmapSymbol, slide, size);
            if (address >= slide && size >= sizeof(unmapHead) && address - slide <= size - sizeof(unmapHead)
                && memcmp(reinterpret_cast<const void*>(address), unmapHead, sizeof(unmapHead)) == 0) {
                KernelPatcher::RouteRequest range{unmapSymbol, unmapVA, orgUnmapVA};
                unmapTraceReady = patcher.routeMultipleShort(id, &range, 1, slide, size);
            }
            if (!unmapTraceReady) { patcher.clearError(); }
            hardwareTable = table + 2 * sizeof(void*);
            return true;
        }

        bool prepareConsumer(KernelPatcher& patcher, size_t id, uintptr_t slide, size_t size)
        {
            constexpr const char* tables[]{"__ZTV32AMDRadeonX6000_AMDVCN2DecChannel",
                                           "__ZTV32AMDRadeonX6000_AMDVCN2EncChannel",
                                           "__ZTV35AMDRadeonX6000_AMDVCN2EncLLQChannel"};
            static_assert(arrsize(tables) == QueueCount);
            for (size_t index = 0; index < QueueCount; ++index) {
                const auto address = patcher.solveSymbol(id, tables[index], slide, size);
                if (!address) {
                    patcher.clearError();
                    return false;
                }
                channelTables[index] = address + 2 * sizeof(void*);
            }
            KernelPatcher::RouteRequest routes[]{
                {"__ZN32AMDRadeonX6000_AMDVCN2EncChannel27commitIndirectCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
                 commitChannel<false>, orgEncode},
                {"__ZN32AMDRadeonX6000_AMDVCN2DecChannel27commitIndirectCommandBufferEP30AMD_SUBMIT_COMMAND_BUFFER_INFO",
                 commitChannel<true>, orgDecode},
                {"__ZN27AMDRadeonX6000_AMDHWChannel5resetEv", resetChannel, orgReset},
                {"__ZN27AMDRadeonX6000_AMDHWChannel20writeDiagnosisReportERPcRj", reportChannel, orgReport},
                {"__ZN30AMDRadeonX6000_AMDVCN2HWEngine4initEP30AMDRadeonX6000_IAMDHWInterface20_eAMD_HW_ENGINE_TYPE",
                 initEngine, orgEngineInit},
                {"__ZN26AMDRadeonX6000_AMDHWEngine4freeEv", freeEngine, orgEngineFree},
            };
            if (!patcher.routeMultiple(id, &routes[0], arrsize(routes), slide, size)) {
                patcher.clearError();
                return false;
            }
            KernelPatcher::RouteRequest timestamp{"__ZN27AMDRadeonX6000_AMDHWChannel16timestampUpdatedEv",
                                                  timestampUpdated, orgTimestampUpdated};
            writebackTraceReady = patcher.routeMultiple(id, &timestamp, 1, slide, size);
            if (!writebackTraceReady) { patcher.clearError(); }
            sysctl_register_oid(&sysctl__debug_nred_vcn_submissions);
            __atomic_store_n(&routesReady, true, __ATOMIC_RELEASE);
            return true;
        }
    }

    void processKext(KernelPatcher& patcher, size_t id, uintptr_t slide, size_t size)
    {
        auto& bridge = VCNBridge::singleton();
        if (!bridge.isRequested()) { return; }
        if (id == kextRadeonX5000HWLibs.loadIndex) {
            __atomic_store_n(&hardwareLibraryMatched, RenoirDriverBuild::matches(slide, size, RenoirDriverBuild::X5000HWLibs_25G332),
                             __ATOMIC_RELEASE);
            return;
        }
        if (NRed::singleton().getDeviceID() != 0x1636) { return; }
        if (id == kextRadeonX5000.loadIndex && RenoirDriverBuild::matches(slide, size, RenoirDriverBuild::X5000_25G332)
                   && bridgeValidated("NRedVCNX5000ABIValidated") && !prepareProducer(patcher, id, slide, size)) {
            bridge.failMedia(21);
        } else if (id == kextRadeonX6000.loadIndex && hardwareTable
                   && RenoirDriverBuild::matches(slide, size, RenoirDriverBuild::X6000_25G332)
                   && bridgeValidated("NRedVCNX6000ABIValidated") && !prepareConsumer(patcher, id, slide, size)) {
            bridge.failMedia(22);
        }
    }
}
