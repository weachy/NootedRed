#pragma once

#include <stddef.h>
#include <stdint.h>

class KernelPatcher;

namespace VCNSubmission
{
    constexpr size_t MaxIBs = 4;

    struct IndirectBuffer
    {
        uint32_t kind;
        uint32_t dwords;
        uint64_t cpuAddress;
        uint64_t gpuAddress;
        uint64_t reserved[2];
    };

    struct Submission
    {
        uint32_t flags;
        uint32_t vmid;
        uint64_t userQueue;
        uint32_t priority;
        uint32_t count;
        uint32_t stamp;
        uint32_t softwareChannel;
        uint32_t softwareStamp;
        uint8_t  reserved[0x24];
        IndirectBuffer ib[MaxIBs];
    };

    static_assert(sizeof(IndirectBuffer) == 0x28 && sizeof(Submission) == 0xE8);
    static_assert(offsetof(Submission, count) == 0x14 && offsetof(Submission, stamp) == 0x18);
    static_assert(offsetof(Submission, ib) + offsetof(IndirectBuffer, dwords) == 0x4C);
    static_assert(offsetof(Submission, ib) + offsetof(IndirectBuffer, gpuAddress) == 0x58);

    enum class Path : uint8_t { Primary, Fence, Auxiliary, Rejected, Replay };

    struct alignas(8) Failure
    {
        uint32_t status, precedingStamp;
    };

    template<typename Entry, size_t Capacity>
    struct History
    {
        Entry entries[Capacity]{};
        uint64_t next{};

        void append(const Entry& entry) { entries[next++ % Capacity] = entry; }

        bool read(uint64_t serial, Entry& entry) const
        {
            if (serial >= next || next - serial > Capacity) { return false; }
            entry = entries[serial % Capacity];
            return true;
        }
    };

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

    struct UnmapRange
    {
        uint64_t begin, end, address, bytes, taskRelease;
        uint32_t mmVmid;
    };

    struct TaskReleaseScope
    {
        const void* thread;
        const void* task;
        uint64_t begin;
        TaskReleaseScope* next{};
    };

    struct ActiveTaskReleases
    {
        TaskReleaseScope* head{};

        void enter(TaskReleaseScope& scope)
        {
            scope.next = head;
            head = &scope;
        }

        void leave(TaskReleaseScope& scope)
        {
            for (auto** link = &head; *link; link = &(*link)->next) {
                if (*link == &scope) {
                    *link = scope.next;
                    return;
                }
            }
        }

        bool associate(UnmapRange& range, const void* thread, const void* task) const
        {
            for (auto* scope = head; scope; scope = scope->next) {
                if (scope->thread == thread && scope->task == task) {
                    range.taskRelease = scope->begin;
                    return true;
                }
            }
            range.taskRelease = 0;
            return range.mmVmid != 0;
        }
    };

    inline bool eligibleUnmapRange(uint32_t vmids, uint32_t vmid, uint64_t address, uint64_t bytes, uint64_t base,
                                   uint64_t end)
    {
        if (!vmids || vmid >= 32 || !bytes || address < base || address >= end || bytes > end - address) { return false; }
        return vmid == 0 || (vmids & (1U << vmid)) != 0;
    }

    constexpr size_t QueueCount = 3;

    struct Writebacks
    {
        uint64_t updates{};
        uint32_t lastStamp{};

        void observe(uint32_t cached, uint32_t stamp)
        {
            if (stamp != cached && (!updates || stamp != lastStamp)) {
                ++updates;
                lastStamp = stamp;
            }
        }
    };

    struct Timeline
    {
        History<SubmitEvent, 8> submissions[QueueCount];
        History<MappingRelease, 8> releases;
        History<UnmapRange, 16> unmaps;
    };

    struct ResetStamps
    {
        uint64_t time;
        uint32_t channel, submitted, lastRead, writeback;
    };

    struct FirstReset
    {
        bool captured{};
        ResetStamps stamps{};
        Timeline timeline{};

        void capture(const ResetStamps& sample, const Timeline& live)
        {
            if (captured || (sample.submitted == sample.lastRead && sample.submitted == sample.writeback)) { return; }
            stamps = sample;
            timeline = live;
            captured = true;
        }
    };

    inline bool validIB(const IndirectBuffer& ib)
    {
        return !ib.kind && ib.dwords && ib.dwords <= UINT32_MAX / 4 && ib.gpuAddress && !(ib.gpuAddress & 3)
            && ib.gpuAddress <= UINT64_MAX - (uint64_t{ib.dwords} * 4 - 1);
    }

    inline Path selectPath(const Submission& job)
    {
        if (job.flags & 2U) { return Path::Replay; }
        if (job.count > MaxIBs) { return Path::Rejected; }
        if (job.count && job.ib[0].dwords) {
            return validIB(job.ib[0]) ? Path::Primary : Path::Rejected;
        }
        auto path = Path::Fence;
        for (size_t index = 1; index < job.count; ++index) {
            const auto& ib = job.ib[index];
            if (!ib.dwords) { continue; }
            if (!validIB(ib)) { return Path::Rejected; }
            path = Path::Auxiliary;
        }
        return path;
    }

    struct Layout
    {
        size_t words, start, ib, tail, stamp, nop;
    };

    constexpr Layout Encode{32, 0, 5, 6, 3, 1};
    constexpr Layout Decode{64, 4, 14, 12, 1, 2};
    static_assert(Encode.start + (MaxIBs - 1) * Encode.ib + Encode.tail <= Encode.words);
    static_assert(Decode.start + (MaxIBs - 1) * Decode.ib + Decode.tail <= Decode.words);

    inline bool validTemplate(bool decode, const uint32_t* frame, size_t words, size_t nop, uint64_t fenceAddress)
    {
        const auto& layout = decode ? Decode : Encode;
        if (!frame || words != layout.words || nop != layout.nop || !fenceAddress || (fenceAddress & 3)) {
            return false;
        }
        const auto* fence = frame + layout.start + layout.ib;
        const auto low = static_cast<uint32_t>(fenceAddress);
        const auto high = static_cast<uint32_t>(fenceAddress >> 32);
        if (decode) {
            return frame[2] == 0x503 && frame[3] == 0x80000014 && frame[4] == 0x1E1 && frame[6] == 0x5A6
                && frame[8] == 0x5A7 && frame[10] == 0x504 && frame[12] == 0x505 && frame[14] == 0x503
                && frame[15] == 0x8000001E && frame[16] == 0x1E2 && fence[0] == 0x1FD && fence[2] == 0x505
                && fence[3] == high && fence[4] == 0x504 && fence[5] == low && fence[6] == 0x503
                && fence[7] == 0x80000000 && fence[8] == 0x1BF && fence[9] == 0x80000002
                && fence[10] == 0x503 && fence[11] == 0x80000016 && frame[words - 2] == 0x53F && !frame[words - 1];
        }
        return (frame[0] == 2 || frame[0] == 5) && fence[0] == 3 && fence[1] == low && fence[2] == high
            && fence[4] == 4 && fence[5] == 1 && !frame[words - 1];
    }

    template<typename Port>
    uint32_t submitFrame(const Submission& job, Path path, bool decode, const uint32_t* source, size_t words,
                         size_t nop, uint64_t fenceAddress, Port& port)
    {
        const auto& layout = decode ? Decode : Encode;
        if (job.count > MaxIBs || !validTemplate(decode, source, words, nop, fenceAddress)) { return port.invalid(); }
        uint32_t frame[Decode.words];
        __builtin_memcpy(frame, source, words * sizeof(*frame));
        const auto* ibTemplate = source + layout.start;
        auto cursor = layout.start;
        const size_t end = path == Path::Primary ? 1 : job.count;
        for (size_t index = path == Path::Primary ? 0 : 1; index < end; ++index) {
            const auto& ib = job.ib[index];
            if (!ib.dwords) { continue; }
            if (cursor != layout.start) { __builtin_memcpy(frame + cursor, ibTemplate, layout.ib * sizeof(*frame)); }
            const auto status = port.updateIB(decode, ib, job.vmid, frame + cursor);
            if (status) { return status; }
            cursor += layout.ib;
        }
        if (cursor == layout.start) {
            for (size_t offset = 0; offset < layout.ib; offset += nop) {
                __builtin_memcpy(frame + cursor + offset, source + words - nop, nop * sizeof(*frame));
            }
            cursor += layout.ib;
        }
        if (cursor != layout.start + layout.ib) {
            __builtin_memcpy(frame + cursor, ibTemplate + layout.ib, layout.tail * sizeof(*frame));
        }
        frame[cursor + layout.stamp] = job.stamp;
        return port.submit(frame);
    }

    template<typename Port>
    uint32_t commit(const Submission& job, bool decode, Port& port)
    {
        const auto path = selectPath(job);
        if (path == Path::Replay) {
            port.record(job, path, 0);
            return port.replay(job);
        }
        const auto failure = port.failure();
        if (failure.status) {
            port.record(job, Path::Rejected, failure.status);
            return failure.precedingStamp;
        }
        port.record(job, path, 0);
        const auto status = path == Path::Rejected ? port.invalid() : port.queueFrame(job, path, decode);
        return status ? port.reject(job, status) : job.stamp;
    }

    void processKext(KernelPatcher& patcher, size_t id, uintptr_t slide, size_t size);
}
