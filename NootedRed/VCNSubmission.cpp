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
#include <libkern/c++/OSNumber.h>
#include <sys/sysctl.h>

namespace VCNSubmission
{
    namespace
    {
        constexpr size_t queueCount = 3;
        constexpr size_t historySize = 8;
        IOSimpleLock* historyLock{};
        bool hardwareLibraryMatched{};
        bool routesReady{};
        void* activeEngine{};
        uintptr_t hardwareTable{};
        uintptr_t channelTables[queueCount]{};
        mach_vm_address_t orgEncode{}, orgDecode{}, orgReset{}, orgReport{}, orgFreeMappings{};
        mach_vm_address_t orgEngineInit{}, orgEngineFree{};

        struct SubmitEvent
        {
            uint64_t time;
            uint32_t stamp, softwareStamp, softwareChannel, flags, vmid, count, error;
            Path path;
            uint32_t words[MaxIBs];
            uint64_t addresses[MaxIBs];
        };

        struct MappingRelease
        {
            uint64_t begin, end;
            int32_t pid;
            uint32_t mmVmid;
        };

        template<typename Entry>
        struct History
        {
            Entry entries[historySize]{};
            uint64_t next{};

            void append(const Entry& entry)
            {
                const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                entries[next++ % historySize] = entry;
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            }

            template<typename Print>
            void print(Print emit) const
            {
                auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
                const uint64_t end = next;
                IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
                for (auto serial = end > historySize ? end - historySize : 0; serial < end; ++serial) {
                    Entry entry;
                    irq = IOSimpleLockLockDisableInterrupt(historyLock);
                    const bool available = serial < next && next - serial <= historySize;
                    if (available) { entry = entries[serial % historySize]; }
                    IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
                    if (available) { emit(entry); }
                }
            }
        };

        struct QueueState
        {
            History<SubmitEvent> history;
            Failure failure{};
            uint64_t failures{};
        };
        QueueState queues[queueCount]{};
        History<MappingRelease> releases{};

        int readSubmissions(struct sysctl_oid* oid, void*, int, struct sysctl_req* request)
        {
            uint64_t records[queueCount], failures[queueCount];
            const auto irq = IOSimpleLockLockDisableInterrupt(historyLock);
            for (size_t index = 0; index < queueCount; ++index) {
                records[index] = queues[index].history.next;
                failures[index] = __atomic_load_n(&queues[index].failures, __ATOMIC_RELAXED);
            }
            IOSimpleLockUnlockEnableInterrupt(historyLock, irq);
            char text[192];
            snprintf(text, sizeof(text), "VCN0Dec=%llu/%llu VCN0Enc=%llu/%llu VCN0EncLLQ=%llu/%llu",
                     records[0], failures[0], records[1], failures[1], records[2], failures[2]);
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
            for (size_t index = 0; index < queueCount; ++index) {
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

            static uint32_t invalid() { return static_cast<uint32_t>(kIOReturnBadArgument); }
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
                queues[queue].history.append(event);
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
            Port port{channel, static_cast<uint32_t>(index)};
            return commit(*job, DecodeChannel, port);
        }

        void resetChannel(void* channel)
        {
            const auto index = queueIndex(channel);
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
            release.pid = getMember<int32_t>(task, 0xAD0);
            release.mmVmid = context ? getMember<uint32_t>(context, 0x11C) : 0;
            release.begin = mach_absolute_time();
            original(task);
            release.end = mach_absolute_time();
            releases.append(release);
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

        void reportChannel(void* channel, char*& cursor, uint32_t& remaining)
        {
            FunctionCast(reportChannel, orgReport)(channel, cursor, remaining);
            const auto index = queueIndex(channel);
            if (index < 0) { return; }
            constexpr const char* paths[]{"primary", "fence", "auxiliary", "rejected"};
            queues[index].history.print([&](const SubmitEvent& event) {
                appendReport(cursor, remaining, "  NRedVCN t=%llu hw=%u stamp=%x sw=%u/%x flags=%x vmid=%u "
                             "path=%s error=%x count=%u", event.time, getMember<uint32_t>(channel, 0xC),
                             event.stamp, event.softwareChannel, event.softwareStamp, event.flags, event.vmid,
                             paths[static_cast<size_t>(event.path)], event.error, event.count);
                for (size_t slot = 0; slot < event.count && slot < MaxIBs; ++slot) {
                    appendReport(cursor, remaining, " ib%zu=%llx/%u", slot, event.addresses[slot], event.words[slot]);
                }
                appendReport(cursor, remaining, "\n");
            });
            if (index == 0) {
                releases.print([&](const MappingRelease& release) {
                    appendReport(cursor, remaining, "  NRedVCN freeMappings t=%llu..%llu pid=%d mmVMID=%u\n",
                                 release.begin, release.end, release.pid, release.mmVmid);
                });
            }
        }

        void detachEngine(void* engine)
        {
            if (__atomic_compare_exchange_n(&activeEngine, &engine, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                NRed::singleton().setProp32("NRedVCNOrderedFence", 0);
            }
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
                queue.history.next = 0;
            }
            releases.next = 0;
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
            KernelPatcher::RouteRequest route{"__ZN27AMDRadeonX5000_AMDAccelTask18freeAllGPUMappingsEv",
                                              freeMappings, orgFreeMappings};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) {
                patcher.clearError();
                IOSimpleLockFree(historyLock);
                historyLock = nullptr;
                return false;
            }
            hardwareTable = table + 2 * sizeof(void*);
            return true;
        }

        bool prepareConsumer(KernelPatcher& patcher, size_t id, uintptr_t slide, size_t size)
        {
            constexpr const char* tables[]{"__ZTV32AMDRadeonX6000_AMDVCN2DecChannel",
                                           "__ZTV32AMDRadeonX6000_AMDVCN2EncChannel",
                                           "__ZTV35AMDRadeonX6000_AMDVCN2EncLLQChannel"};
            static_assert(arrsize(tables) == queueCount);
            for (size_t index = 0; index < queueCount; ++index) {
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
