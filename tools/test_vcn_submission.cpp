#include "VCNSubmission.hpp"
#include <cassert>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

using namespace VCNSubmission;
using Frame = std::vector<uint32_t>;
constexpr uint64_t fenceAddress = 0xF123456000ULL;
constexpr uint64_t ibAddress = 0x40000C000ULL;

Frame referenceFrame(bool decode)
{
    Frame frame;
    if (decode) {
        frame = {0, 0, 0x503, 0x80000014, 0x1E1, 0, 0x5A6, 0, 0x5A7, 0, 0x504, 0, 0x505, 0, 0x503,
                 0x8000001E, 0x1E2, 0, 0x1FD, 0, 0x505, 0xF1, 0x504, 0x23456000, 0x503, 0x80000000,
                 0x1BF, 0x80000002, 0x503, 0x80000016};
        while (frame.size() < Decode.words) { frame.insert(frame.end(), {0x53F, 0}); }
    } else {
        frame = {2, 0, 0, 0, 0, 3, 0x23456000, 0xF1, 0, 4, 1};
        frame.resize(Encode.words);
    }
    return frame;
}

Frame readFrame(const std::string& path, size_t words)
{
    Frame frame(words);
    std::ifstream file(path, std::ios::binary);
    assert(file.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(words * 4)));
    assert(file.peek() == std::ifstream::traits_type::eof());
    return frame;
}

struct Queue
{
    const Frame source;
    const Layout layout;
    std::deque<Frame> pending;
    Failure firstFailure{};
    uint32_t completed{}, submitError{}, buildErrorAt{}, builds{}, attempts{}, replays{};
    Path path{};

    explicit Queue(bool decode) : source(referenceFrame(decode)), layout(decode ? Decode : Encode) {}
    static uint32_t invalid() { return 1; }
    Failure failure() const { return firstFailure; }
    uint32_t replay(const Submission& job) { ++replays; return job.stamp; }
    void record(const Submission&, Path selected, uint32_t) { path = selected; }
    uint32_t reject(const Submission& job, uint32_t status)
    {
        firstFailure = {status, job.stamp - 1U};
        return firstFailure.precedingStamp;
    }
    uint32_t submit(uint32_t* frame)
    {
        ++attempts;
        if (!submitError) { pending.emplace_back(frame, frame + layout.words); }
        return submitError;
    }
    uint32_t updateIB(bool decode, const IndirectBuffer& ib, uint32_t vmid, uint32_t* packet)
    {
        if (++builds == buildErrorAt) { return 2; }
        packet[1] = vmid;
        const auto low = static_cast<uint32_t>(ib.gpuAddress);
        const auto high = static_cast<uint32_t>(ib.gpuAddress >> 32);
        if (decode) {
            packet[3] = high;
            packet[5] = low;
            packet[7] = packet[9] = 0x1234;
            packet[13] = ib.dwords;
        } else {
            packet[2] = low;
            packet[3] = high;
            packet[4] = ib.dwords;
        }
        return 0;
    }
    uint32_t queueFrame(const Submission& job, Path selected, bool decode)
    {
        return submitFrame(job, selected, decode, source.data(), source.size(), layout.nop, fenceAddress, *this);
    }
    void retire(size_t packets = 1)
    {
        assert(!pending.empty());
        completed = pending.front().at(layout.start + packets * layout.ib + layout.stamp);
        pending.pop_front();
    }
};

void testQueue(bool decode, const std::string& name, const char* directory)
{
    Queue queue(decode);
    const auto layout = queue.layout;
    if (directory) { assert(queue.source == readFrame(std::string(directory) + "/" + name + ".bin", layout.words)); }
    assert(validTemplate(decode, queue.source.data(), layout.words, layout.nop, fenceAddress));
    assert(!validTemplate(decode, queue.source.data(), layout.words - 1, layout.nop, fenceAddress));
    assert(!validTemplate(decode, queue.source.data(), layout.words, layout.nop, fenceAddress + 4));
    auto damaged = queue.source;
    damaged[layout.start + layout.ib] ^= 1;
    assert(!validTemplate(decode, damaged.data(), damaged.size(), layout.nop, fenceAddress));

    Submission job{};
    job.count = 1;
    job.ib[0].gpuAddress = ibAddress;
    job.ib[0].dwords = 19;
    job.stamp = UINT32_MAX - 1;
    assert(commit(job, decode, queue) == job.stamp && queue.path == Path::Primary);
    job.ib[0].dwords = 0;
    ++job.stamp;
    assert(commit(job, decode, queue) == job.stamp && queue.path == Path::Fence);
    auto expected = queue.source;
    for (size_t index = 0; index < layout.ib; ++index) {
        expected[layout.start + index] = queue.source[layout.words - layout.nop + index % layout.nop];
    }
    expected[layout.start + layout.ib + layout.stamp] = job.stamp;
    assert(queue.pending.back() == expected);
    job.flags = 0;
    job.count = 0;
    ++job.stamp;
    assert(commit(job, decode, queue) == 0 && queue.path == Path::Fence);
    assert(queue.completed == 0 && queue.pending.size() == 3);
    queue.retire();
    assert(queue.completed == UINT32_MAX - 1);
    queue.retire();
    assert(queue.completed == UINT32_MAX);
    queue.retire();
    assert(queue.completed == 0);
    job.count = MaxIBs;
    job.flags = 2;
    for (auto& ib : job.ib) { ib.gpuAddress = ibAddress; }
    const auto enqueued = queue.attempts;
    assert(commit(job, decode, queue) == 0 && queue.path == Path::Replay && queue.replays == 1);
    assert(queue.builds == 1 && queue.attempts == enqueued && queue.pending.empty());
    queue.firstFailure = {3, 123};
    assert(commit(job, decode, queue) == 0 && queue.replays == 2 && queue.firstFailure.precedingStamp == 123);
    assert(queue.attempts == enqueued && queue.pending.empty());
    queue.firstFailure = {};

    job.flags = 0;
    job.vmid = 8;
    job.stamp = 0x97E;
    job.count = MaxIBs;
    job.ib[3].dwords = 19;
    job.ib[3].gpuAddress = ibAddress;
    job.ib[0] = job.ib[3];
    job.ib[3].gpuAddress += 0x1000;
    assert(commit(job, decode, queue) == job.stamp && queue.path == Path::Primary);
    const auto primary = queue.pending.front();
    if (directory) {
        assert(primary == readFrame(std::string(directory) + "/" + name + "-single.bin", layout.words));
    }
    queue.retire();
    job.ib[0].dwords = 0;
    job.ib[3].gpuAddress = ibAddress;
    assert(commit(job, decode, queue) == job.stamp && queue.path == Path::Auxiliary);
    assert(queue.pending.front() == primary);
    queue.retire();
    job.ib[1].gpuAddress = 0x512345000;
    job.ib[1].dwords = 12;
    job.ib[2].gpuAddress = 0x601234000;
    job.ib[2].dwords = 44;
    ++job.stamp;
    assert(commit(job, decode, queue) == job.stamp && queue.pending.size() == 1);
    const auto& packed = queue.pending.front();
    for (size_t index = 1; index < job.count; ++index) {
        const size_t start = layout.start + (index - 1) * layout.ib;
        assert(packed[start + 1] == job.vmid);
        assert(packed[start + (decode ? 5 : 2)] == static_cast<uint32_t>(job.ib[index].gpuAddress));
        assert(packed[start + layout.ib - 1] == job.ib[index].dwords);
    }
    const auto oldTail = layout.start + layout.ib;
    const auto newTail = layout.start + 3 * layout.ib;
    for (size_t index = 0; index < layout.tail; ++index) {
        assert(packed[newTail + index] == (index == layout.stamp ? job.stamp : queue.source[oldTail + index]));
    }
    queue.retire(3);
    assert(queue.completed == job.stamp);

    queue.buildErrorAt = queue.builds + 2;
    auto attempts = queue.attempts;
    assert(commit(job, decode, queue) == job.stamp - 1 && queue.firstFailure.status == 2);
    assert(queue.attempts == attempts && queue.pending.empty());
    const auto precedingStamp = job.stamp - 1;
    job = {};
    job.count = 1;
    job.ib[0].gpuAddress = ibAddress;
    job.ib[0].dwords = 19;
    assert(commit(job, decode, queue) == precedingStamp && queue.pending.empty() && queue.attempts == attempts);
    queue.firstFailure = {};
    queue.buildErrorAt = queue.builds + 1;
    assert(commit(job, decode, queue) == UINT32_MAX && queue.firstFailure.status == 2 && queue.attempts == attempts);
    queue.firstFailure = {};
    queue.submitError = 3;
    assert(commit(job, decode, queue) == UINT32_MAX && queue.firstFailure.status == 3 && queue.pending.empty());
    queue.submitError = 0;
    job.ib[0].dwords = 0;
    ++job.stamp;
    assert(commit(job, decode, queue) == UINT32_MAX && queue.path == Path::Rejected && queue.pending.empty());
    queue.firstFailure = {};
    job.stamp = 0;
    assert(commit(job, decode, queue) == 0 && queue.pending.size() == 1);
    queue.retire();
    job.stamp = 1;
    job.ib[0].dwords = 19;
    job.ib[0].gpuAddress = ibAddress;
    assert(commit(job, decode, queue) == 1 && queue.path == Path::Primary);
    queue.retire();
    assert(queue.completed == 1 && queue.source == referenceFrame(decode));
    job.flags = 2;
    assert(selectPath(job) == Path::Replay);
    job.flags = 0;
    job.count = MaxIBs + 1;
    assert(selectPath(job) == Path::Rejected);
    job.count = 2;
    job.ib[0].dwords = 0;
    job.ib[1] = {};
    job.ib[1].dwords = 4;
    assert(selectPath(job) == Path::Rejected);
    job.ib[1].gpuAddress = UINT64_MAX - 3;
    assert(selectPath(job) == Path::Rejected);
    job = {};
    job.count = 1;
    job.vmid = decode ? 11 : 8;
    job.stamp = decode ? 0x79 : 0x156;
    job.ib[0].gpuAddress = decode ? 0x400010000 : 0x400010280;
    job.ib[0].dwords = decode ? 176 : 13;
    assert(commit(job, decode, queue) == job.stamp && queue.path == Path::Primary);
    if (directory) {
        assert(queue.pending.front() == readFrame(std::string(directory) + "/" + name + "-handoff.bin", layout.words));
    }
    queue.retire();
    assert(queue.completed == job.stamp);
    std::printf("%s queue checks passed\n", name.c_str());
}

void testFirstReset()
{
    Timeline live{};
    FirstReset first{};
    first.capture({1, 12, 0x79, 0x79, 0x79}, live);
    assert(!first.captured);
    SubmitEvent decode{}, encode{};
    decode.stamp = 0x79;
    decode.vmid = 11;
    encode.stamp = encode.softwareStamp = 0x156;
    encode.vmid = 8;
    live.submissions[0].append(decode);
    live.submissions[2].append(encode);
    live.unmaps.append({2, 3, 0x400000000, 0x10000, 0, 11});
    live.releases.append({4, 5, 6175, 0});
    first.capture({6, 14, 0x156, 0x155, 0x155}, live);
    assert(first.captured);
    for (uint32_t replay = 0; replay < 32; ++replay) {
        ++encode.stamp;
        encode.path = Path::Fence;
        encode.vmid = 0;
        live.submissions[2].append(encode);
        live.unmaps.append({7, 8, 0, 0x1000, 0, 8});
        live.releases.append({9, 10, 0, 8});
        first.capture({11, 14, encode.stamp, encode.stamp - 1, encode.stamp - 1}, live);
    }
    assert(first.stamps.time == 6 && first.stamps.channel == 14 && first.stamps.submitted == 0x156);
    assert(first.stamps.lastRead == 0x155 && first.stamps.writeback == 0x155);
    assert(first.timeline.submissions[0].next == 1 && first.timeline.submissions[2].next == 1);
    SubmitEvent event{};
    assert(first.timeline.submissions[2].read(0, event));
    assert(event.stamp == 0x156 && event.vmid == 8 && event.path == Path::Primary);
    assert(first.timeline.unmaps.next == 1 && first.timeline.releases.next == 1);
    UnmapRange range{};
    MappingRelease release{};
    assert(first.timeline.unmaps.read(0, range) && range.bytes == 0x10000 && range.mmVmid == 11);
    assert(first.timeline.releases.read(0, release) && release.pid == 6175);
    assert(!live.submissions[2].read(24, event));
    assert(live.submissions[2].read(25, event) && event.stamp == 0x16F);
    assert(live.submissions[2].read(32, event) && event.stamp == 0x176);
    assert(!live.submissions[2].read(33, event));
    live = {};
    first.capture({12, 12, 1, 0, 0}, live);
    assert(first.stamps.time == 6 && first.timeline.submissions[2].next == 1);
    FirstReset wrapped{};
    wrapped.capture({13, 12, 0, UINT32_MAX, UINT32_MAX}, live);
    assert(wrapped.captured && wrapped.stamps.channel == 12 && wrapped.stamps.submitted == 0);
    std::puts("First reset survives replay, history wrap and engine reinitialization");
}

void testUnmapTrace()
{
    int decoderThread{}, encoderThread{}, decoderTask{}, encoderTask{};
    TaskReleaseScope outer{&decoderThread, &decoderTask, 4}, overlapping{&encoderThread, &decoderTask, 5},
        otherTask{&decoderThread, &encoderTask, 6}, inner{&decoderThread, &decoderTask, 7};
    ActiveTaskReleases releases{};
    const auto check = [&](const void* thread, const void* task, uint64_t expected) {
        for (auto vmid : {0U, 11U}) {
            UnmapRange range{0, 0, 0x4000, 0x1000, UINT64_MAX, vmid};
            assert(releases.associate(range, thread, task) == (vmid || expected));
            assert(range.taskRelease == expected);
        }
    };
    releases.enter(outer);
    releases.enter(overlapping);
    releases.enter(otherTask);
    releases.enter(inner);
    check(&decoderThread, &decoderTask, 7);
    check(&decoderThread, &encoderTask, 6);
    check(&encoderThread, &decoderTask, 5);
    check(&encoderThread, &encoderTask, 0);
    check(&decoderThread, nullptr, 0);
    releases.leave(overlapping);
    releases.leave(inner);
    check(&decoderThread, &decoderTask, 4);
    check(&encoderThread, &decoderTask, 0);
    releases.leave(outer);
    check(&decoderThread, &decoderTask, 0);
    check(&decoderThread, &encoderTask, 6);
    releases.leave(otherTask);
    check(&decoderThread, &encoderTask, 0);
    const auto vmids = 1U << 11;
    assert(eligibleUnmapRange(vmids, 11, 0x4000, 0x1000, 0x4000, 0x5000));
    assert(eligibleUnmapRange(vmids, 0, 0x4000, 0x1000, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(0, 0, 0x4000, 0x1000, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(vmids, 8, 0x4000, 0x1000, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(UINT32_MAX, 32, 0x4000, 0x1000, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(vmids, 11, 0x4000, 0, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(vmids, 11, 0x3FFF, 1, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(vmids, 11, 0x5000, 1, 0x4000, 0x5000));
    assert(!eligibleUnmapRange(vmids, 11, UINT64_MAX - 1, 3, 0x4000, UINT64_MAX));
    std::puts("MM-hub unmap ranges and thread/task ownership checked");
}

int main(int argc, char** argv)
{
    assert(argc <= 2);
    Writebacks writebacks{};
    writebacks.observe(1, 1);
    assert(!writebacks.updates);
    writebacks.observe(UINT32_MAX, 0);
    writebacks.observe(UINT32_MAX, 0);
    writebacks.observe(0, 0);
    writebacks.observe(0, 3);
    assert(writebacks.updates == 2 && writebacks.lastStamp == 3);
    testFirstReset();
    testUnmapTrace();
    testQueue(true, "Dec", argv[1]);
    testQueue(false, "Enc", argv[1]);
    testQueue(false, "EncLLQ", argv[1]);

    Queue encLLQ(false), dec(true);
    Submission job{};
    job.stamp = 12;
    encLLQ.submitError = 3;
    assert(commit(job, false, encLLQ) == 11 && encLLQ.pending.empty());
    assert(commit(job, true, dec) == 12 && !dec.firstFailure.status);
    dec.retire();
    assert(dec.completed == 12);
    ++job.stamp;
    assert(commit(job, false, encLLQ) == 11 && encLLQ.attempts == 1);
}
