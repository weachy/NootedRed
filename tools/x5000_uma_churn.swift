import Darwin
import Foundation
import Metal

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

let arguments = CommandLine.arguments.dropFirst()
let compare = arguments.contains("--compare")
let asyncMode = arguments.contains("--async")
let sizes = arguments.filter { $0 != "--compare" && $0 != "--async" }.map { argument -> Int in
    guard let value = Int(argument), value > 0 else { fail("Usage: x5000_uma_churn [--compare] [--async] [MiB ...]") }
    return value
}
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("Metal setup failed") }
let library = try device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void fill(texture2d<half, access::write> output [[texture(0)]], constant uint &tag [[buffer(0)]],
                 uint2 pos [[thread_position_in_grid]]) {
    output.write(as_type<half4>(ushort4(tag, 0x3c00u | (pos.x & 255u), 0x3c00u | (pos.y & 255u), 0x3c00u)), pos);
}
""", options: nil)
let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let counterNames = ["pool_allocations", "task_commits", "pte_batched_entries", "pool_fence_reuse_hits",
                    "texture_cache_stores", "texture_cache_hits", "texture_cache_mapping_hits", "texture_cache_evictions",
                    "fragment_windows", "fragment_contiguous_windows", "fragment_mapped_windows", "fragment_4k_pages",
                    "fragment_incomplete_maps", "fragment_pool_swaps", "fragment_va_retries"]

func sysctlValue<Value: FixedWidthInteger>(_ name: String, as: Value.Type) -> Value? {
    var value: Value = 0
    var size = MemoryLayout.size(ofValue: value)
    if sysctlbyname("debug.nred_uma." + name, &value, &size, nil, 0) != 0 {
        if errno == ENOENT { return nil }
        fail("Reading \(name) failed: \(String(cString: strerror(errno)))")
    }
    guard size == MemoryLayout.size(ofValue: value) else { fail("Unexpected size for \(name)") }
    return value
}

func counter(_ name: String) -> UInt64? { sysctlValue(name, as: UInt64.self) }

func deltas(since before: [UInt64?]) -> String {
    let fields = zip(counterNames, before).compactMap { name, old -> String? in
        guard let old, let new = counter(name), new >= old else { return nil }
        return "\(name)=\(new - old)"
    }
    return fields.isEmpty ? "uma_counters=unavailable" : fields.joined(separator: " ")
}

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }
func percentile(_ values: [Double], _ fraction: Double) -> Double {
    values.sorted()[Int(ceil(Double(values.count) * fraction)) - 1]
}

func readback() -> MTLBuffer {
    guard let buffer = device.makeBuffer(length: 256, options: .storageModeShared) else { fail("Readback allocation failed") }
    return buffer
}

struct Sample {
    let tag: UInt32
    let origin: MTLOrigin
    let buffer: MTLBuffer
    let command: MTLCommandBuffer

    func verify() {
        command.waitUntilCompleted()
        guard command.status == .completed else { fail("Texture write failed: \(String(describing: command.error))") }
        let pixel = buffer.contents().bindMemory(to: UInt16.self, capacity: 4)
        guard pixel[0] == UInt16(tag), pixel[1] == UInt16(0x3c00 | (origin.x & 255)),
              pixel[2] == UInt16(0x3c00 | (origin.y & 255)), pixel[3] == 0x3c00 else {
            fail("Texture readback mismatch at (\(origin.x), \(origin.y)) for tag \(tag)")
        }
    }
}

func submit(_ texture: MTLTexture, iteration: Int, to buffer: MTLBuffer, on queue: MTLCommandQueue) -> Sample {
    guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
        fail("Compute encoder creation failed")
    }
    var tag = UInt32(0x3c00 + iteration)
    let origin = MTLOrigin(x: (texture.width / 2 + 17 * iteration) % texture.width,
                           y: (texture.height / 2 + 31 * iteration) % texture.height, z: 0)
    encoder.setComputePipelineState(pipeline)
    encoder.setTexture(texture, index: 0)
    encoder.setBytes(&tag, length: MemoryLayout.size(ofValue: tag), index: 0)
    encoder.dispatchThreads(MTLSize(width: texture.width, height: texture.height, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
    encoder.endEncoding()
    guard let blit = command.makeBlitCommandEncoder() else { fail("Blit encoder creation failed") }
    blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: origin,
              sourceSize: MTLSize(width: 1, height: 1, depth: 1), to: buffer, destinationOffset: 0,
              destinationBytesPerRow: 256, destinationBytesPerImage: 256)
    blit.endEncoding()
    command.commit()
    return Sample(tag: tag, origin: origin, buffer: buffer, command: command)
}

enum Source: String { case fresh, heap, persistent }

func evaluate(_ mode: Source, _ descriptor: MTLTextureDescriptor, mib: Int) {
    let initialLive = counter("bytes_live")
    var growth = "unavailable"
    var heap: MTLHeap?
    var persistent: MTLTexture?
    switch mode {
    case .fresh: break
    case .persistent:
        persistent = device.makeTexture(descriptor: descriptor)
        if persistent == nil { fail("Persistent texture allocation failed at \(mib) MiB") }
    case .heap:
        let required = device.heapTextureSizeAndAlign(descriptor: descriptor)
        let config = MTLHeapDescriptor()
        config.size = required.size
        config.storageMode = .private
        config.hazardTrackingMode = .tracked
        heap = device.makeHeap(descriptor: config)
        if heap == nil { fail("Heap allocation failed at \(mib) MiB") }
    }
    let buffer = readback()
    var before: [UInt64?] = []
    var wall: [Double] = [], gpu: [Double] = []
    var firstWall = 0.0
    for iteration in 0..<29 {
        if iteration == 5 { before = counterNames.map(counter) }
        let start = now()
        let gpuTime = autoreleasepool { () -> Double in
            let allocation: MTLTexture?
            switch mode {
            case .fresh: allocation = device.makeTexture(descriptor: descriptor)
            case .heap: allocation = heap?.makeTexture(descriptor: descriptor)
            case .persistent: allocation = persistent
            }
            guard let texture = allocation else { fail("\(mode.rawValue) texture allocation failed at \(mib) MiB") }
            let sample = submit(texture, iteration: iteration, to: buffer, on: queue)
            sample.verify()
            if iteration == 2, let initialLive, let live = counter("bytes_live") {
                growth = String(format: "%.1f", (Double(live) - Double(initialLive)) / 1048576)
            }
            if mode == .heap { texture.makeAliasable() }
            return 1000 * (sample.command.gpuEndTime - sample.command.gpuStartTime)
        }
        let wallTime = now() - start
        if iteration == 0 { firstWall = wallTime }
        if iteration >= 5 {
            wall.append(wallTime)
            gpu.append(gpuTime)
        }
    }
    print(String(format: "CHURN %@ %d MiB n=%d wall_p50=%.2f ms wall_p95=%.2f ms gpu_p50=%.2f ms gpu_p95=%.2f ms first_wall=%.2f ms",
                 mode.rawValue, mib, wall.count, percentile(wall, 0.50), percentile(wall, 0.95),
                 percentile(gpu, 0.50), percentile(gpu, 0.95), firstWall) + " live_growth_MiB=\(growth) " + deltas(since: before))
}

func runAsync(_ descriptor: MTLTextureDescriptor, mib: Int) {
    guard let second = device.makeCommandQueue() else { fail("Second queue allocation failed") }
    let queues = [queue, second], buffers = (0..<3).map { _ in readback() }
    var pending = Array<Sample?>(repeating: nil, count: buffers.count)
    let before = counterNames.map(counter)
    for iteration in 0..<48 {
        let slot = iteration % buffers.count
        pending[slot]?.verify()
        pending[slot] = nil
        pending[slot] = autoreleasepool {
            guard let texture = device.makeTexture(descriptor: descriptor) else { fail("Async texture allocation failed") }
            return submit(texture, iteration: iteration, to: buffers[slot], on: queues[iteration % queues.count])
        }
    }
    for sample in pending { sample?.verify() }
    print("ASYNC fresh \(mib) MiB queues=2 max_in_flight=3 verified=48 " + deltas(since: before))
}

let modes = ["active", "pool_fence_reuse", "texture_cache", "fragment_stats", "fragment_64k"].compactMap { name in
    sysctlValue(name, as: Int32.self).map { "\(name)=\($0)" }
}
print("DEVICE \(device.name) format=rgba16Float " + (modes.isEmpty ? "uma_modes=unavailable" : modes.joined(separator: " ")))
for mib in sizes.isEmpty ? [64] : sizes {
    let side = Int((Double(mib) * 131072).squareRoot())
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: side, height: side, mipmapped: false)
    descriptor.storageMode = .private
    descriptor.usage = .shaderWrite
    descriptor.hazardTrackingMode = .tracked
    if compare || !asyncMode {
        let modes: [Source] = compare ? [.fresh, .heap, .persistent, .persistent, .heap, .fresh] : [.fresh]
        for mode in modes { autoreleasepool { evaluate(mode, descriptor, mib: mib) } }
    }
    if asyncMode { autoreleasepool { runAsync(descriptor, mib: mib) } }
}
