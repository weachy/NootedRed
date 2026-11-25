import Foundation
import Metal

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

setvbuf(stdout, nil, _IOLBF, 0)
let arguments = Array(CommandLine.arguments.dropFirst())
let usage = "Usage: x5000_uma_workingset [MiB ...] | --hold MiB seconds | --latency [MiB ...]"
var holdSeconds: Double?
let measurePageLatency = arguments.first == "--latency"
let sizes: [Int]
if arguments.first == "--hold" {
    guard arguments.count == 3, let mib = Int(arguments[1]), mib > 0,
          let seconds = Double(arguments[2]), seconds.isFinite, seconds > 0 else { fail(usage) }
    holdSeconds = seconds
    sizes = [mib]
} else {
    let sizeArgs = measurePageLatency ? Array(arguments.dropFirst()) : arguments
    let defaults = measurePageLatency ? [64] : [256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096]
    sizes = sizeArgs.isEmpty ? defaults : sizeArgs.map {
        guard let mib = Int($0), mib > 0 else { fail(usage) }
        return mib
    }
}

guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fail("Metal setup failed") }
let library = try device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void fill(texture2d<half, access::write> target [[texture(0)]], constant uint &seed [[buffer(0)]],
                 uint2 pos [[thread_position_in_grid]]) {
    target.write(half4(half(pos.x & 255u), half(pos.y & 255u), half(seed), half((pos.x + pos.y + seed) & 255u)), pos);
}
kernel void checksum(texture2d<half, access::read> source [[texture(0)]], device uint *output [[buffer(0)]],
                     constant uint &pass [[buffer(1)]], uint2 pos [[thread_position_in_grid]]) {
    float sum = 0;
    for (uint row = pos.y; row < source.get_height(); row += 64) {
        sum += dot(float4(source.read(uint2(pos.x, row))), float4(1, 3, 5, 7));
    }
    output[pos.y * source.get_width() + pos.x] = uint(sum) ^ pass;
}
kernel void chase(device const uint *chain [[buffer(0)]], device uint *result [[buffer(1)]],
                  constant uint2 &walk [[buffer(2)]]) {
    uint position = walk.x;
    for (uint step = 0; step < walk.y; ++step) { position = chain[position]; }
    result[0] = position;
}
""", options: nil)
let fill = try device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let checksum = try device.makeComputePipelineState(function: library.makeFunction(name: "checksum")!)
let width = 2048, lanesPerColumn = 64, textureMiB = 32

struct Payload {
    let texture: MTLTexture
    let output: MTLBuffer
    let seed: Int
    let channelSums: [Int]
}

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

func submit(_ encode: (MTLComputeCommandEncoder) -> Void) -> MTLCommandBuffer {
    guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
        fail("Compute encoder creation failed")
    }
    encode(encoder)
    encoder.endEncoding()
    command.commit()
    return command
}

func complete(_ commands: [MTLCommandBuffer]) {
    for command in commands {
        command.waitUntilCompleted()
        if command.status != .completed { fail("GPU command failed: \(String(describing: command.error))") }
    }
}

func makePayloads(mib: Int) -> [Payload] {
    var payloads: [Payload] = [], commands: [MTLCommandBuffer] = []
    var remaining = mib
    while remaining > 0 {
        let chunk = min(remaining, textureMiB)
        let height = chunk * 64
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float,
                                      width: width, height: height, mipmapped: false)
        descriptor.storageMode = .private
        descriptor.usage = [.shaderRead, .shaderWrite]
        guard let texture = device.makeTexture(descriptor: descriptor),
              let output = device.makeBuffer(length: width * lanesPerColumn * MemoryLayout<UInt32>.size,
                                             options: .storageModeShared) else {
            fail("Allocation failed after \(mib - remaining) MiB of \(mib) MiB")
        }
        let seed = payloads.count & 255
        let channelSums = (0..<256).map { base in
            (0..<chunk).reduce(0) { sum, step in sum + ((base + step * lanesPerColumn) & 255) }
        }
        payloads.append(Payload(texture: texture, output: output, seed: seed, channelSums: channelSums))
        commands.append(submit { encoder in
            var value = UInt32(seed)
            encoder.setComputePipelineState(fill)
            encoder.setTexture(texture, index: 0)
            encoder.setBytes(&value, length: MemoryLayout.size(ofValue: value), index: 0)
            encoder.dispatchThreads(MTLSize(width: width, height: height, depth: 1),
                                    threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        })
        if commands.count == 8 {
            complete(commands)
            commands.removeAll(keepingCapacity: true)
        }
        remaining -= chunk
    }
    complete(commands)
    return payloads
}

func readPass(_ payloads: [Payload], pass: UInt32) -> Double {
    let start = now()
    let commands = payloads.map { payload in
        submit { encoder in
            var stamp = pass
            encoder.setComputePipelineState(checksum)
            encoder.setTexture(payload.texture, index: 0)
            encoder.setBuffer(payload.output, offset: 0, index: 0)
            encoder.setBytes(&stamp, length: MemoryLayout.size(ofValue: stamp), index: 1)
            encoder.dispatchThreads(MTLSize(width: width, height: lanesPerColumn, depth: 1),
                                    threadsPerThreadgroup: MTLSize(width: 64, height: 4, depth: 1))
        }
    }
    complete(commands)
    let elapsed = now() - start
    for (index, payload) in payloads.enumerated() {
        let values = payload.output.contents().bindMemory(to: UInt32.self, capacity: width * lanesPerColumn)
        let rows = payload.texture.height / lanesPerColumn
        for row in 0..<lanesPerColumn {
            let greenBlue = 3 * payload.channelSums[row] + 5 * rows * payload.seed
            for column in 0..<width {
                let alpha = payload.channelSums[(column + row + payload.seed) & 255]
                let expected = UInt32(rows * (column & 255) + greenBlue + 7 * alpha) ^ pass
                if values[row * width + column] != expected {
                    fail("Checksum mismatch in texture \(index), lane (\(column), \(row)), pass \(pass)")
                }
            }
        }
    }
    return elapsed
}

func measure(mib: Int) {
    let start = now()
    let payloads = makePayloads(mib: mib)
    let setup = now() - start
    var times: [Double] = []
    if let seconds = holdSeconds {
        print("HOLD \(mib) MiB ready")
        let end = now() + seconds * 1000
        repeat {
            times.append(autoreleasepool { readPass(payloads, pass: UInt32(truncatingIfNeeded: times.count)) })
            let remaining = end - now()
            if remaining > 0 { Thread.sleep(forTimeInterval: min(1, remaining / 1000)) }
        } while now() < end
    } else {
        for pass in 0..<6 {
            let elapsed = autoreleasepool { readPass(payloads, pass: UInt32(pass)) }
            if pass > 0 { times.append(elapsed) }
        }
    }
    let sorted = times.sorted()
    let median = sorted[(sorted.count - 1) / 2]
    let p95 = sorted[Int(ceil(Double(sorted.count) * 0.95)) - 1]
    print(String(format: "%@ %d MiB textures=%d setup=%.0f ms passes=%d pass_p50=%.1f ms pass_p95=%.1f ms pass_max=%.1f ms payload=%.2f GiB/s checksums_per_pass=%d",
                 holdSeconds == nil ? "WORKINGSET" : "HOLD", mib, payloads.count, setup, times.count, median, p95,
                 sorted.last!, Double(mib) / 1024 / (median / 1000), width * lanesPerColumn * payloads.count))
}

func measureLatency(mib: Int, strideBytes: Int, pipeline: MTLComputePipelineState) {
    let wordSize = MemoryLayout<UInt32>.stride
    guard mib <= min(device.maxBufferLength, Int(UInt32.max) * wordSize) >> 20 else { fail("Buffer too large: \(mib) MiB") }
    let bytes = mib << 20, nodes = bytes / strideBytes, wordStride = strideBytes / wordSize
    guard let chain = device.makeBuffer(length: bytes, options: .storageModePrivate),
          let output = device.makeBuffer(length: wordSize, options: .storageModeShared) else { fail("Chase allocation failed") }
    var order = Array(0..<nodes)
    var state = UInt64(bytes ^ strideBytes)
    for index in stride(from: nodes - 1, to: 0, by: -1) {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        order.swapAt(index, Int((state >> 32) % UInt64(index + 1)))
    }
    autoreleasepool {
        guard let upload = device.makeBuffer(length: bytes, options: .storageModeShared),
              let command = queue.makeCommandBuffer(), let blit = command.makeBlitCommandEncoder() else { fail("Chase upload failed") }
        let links = upload.contents().bindMemory(to: UInt32.self, capacity: bytes / wordSize)
        links.initialize(repeating: 0, count: bytes / wordSize)
        for index in order.indices {
            links[order[index] * wordStride] = UInt32(order[(index + 1) % nodes] * wordStride)
        }
        blit.copy(from: upload, sourceOffset: 0, to: chain, destinationOffset: 0, size: bytes)
        blit.endEncoding()
        command.commit()
        complete([command])
    }
    let steps = min(65536, max(16384, nodes)) + 37
    let result = output.contents().bindMemory(to: UInt32.self, capacity: 1)
    var times: [Double] = []
    for pass in 0..<23 {
        let start = (pass * 127) % nodes
        result.pointee = UInt32.max
        let elapsed = autoreleasepool { () -> Double in
            let command = submit { encoder in
                var walk = SIMD2<UInt32>(UInt32(order[start] * wordStride), UInt32(steps))
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(chain, offset: 0, index: 0)
                encoder.setBuffer(output, offset: 0, index: 1)
                encoder.setBytes(&walk, length: MemoryLayout.size(ofValue: walk), index: 2)
                let one = MTLSize(width: 1, height: 1, depth: 1)
                encoder.dispatchThreads(one, threadsPerThreadgroup: one)
            }
            complete([command])
            return (command.gpuEndTime - command.gpuStartTime) * 1e9 / Double(steps)
        }
        guard result.pointee == UInt32(order[(start + steps) % nodes] * wordStride) else { fail("Chase readback mismatch at pass \(pass)") }
        guard elapsed.isFinite && elapsed > 0 else { fail("GPU timing unavailable") }
        if pass >= 3 { times.append(elapsed) }
    }
    times.sort()
    print(String(format: "LATENCY %d MiB stride=%d B nodes=%d steps=%d n=%d gpu_p50=%.1f ns/load gpu_p95=%.1f ns/load",
                 mib, strideBytes, nodes, steps, times.count, times[(times.count - 1) / 2],
                 times[Int(ceil(Double(times.count) * 0.95)) - 1]))
}

print("DEVICE \(device.name) recommendedWorkingSet=\(device.recommendedMaxWorkingSetSize >> 20) MiB")
if measurePageLatency {
    let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "chase")!)
    for mib in sizes {
        for strideBytes in [4096, 65536] { autoreleasepool { measureLatency(mib: mib, strideBytes: strideBytes, pipeline: pipeline) } }
    }
} else {
    for mib in sizes { autoreleasepool { measure(mib: mib) } }
}
