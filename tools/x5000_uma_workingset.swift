// Working-set sweep: N MiB of private RGBA16Float textures (32 MiB each) stay resident; every pass reads all of them,
// one command buffer per texture as an app that draws layer by layer would. Reports the wall time per pass (it
// includes the driver's paging and residency work), the effective read rate and failed command buffers.
//   x5000_uma_workingset [MiB ...]
import Foundation
import Metal

setvbuf(stdout, nil, _IOLBF, 0)
// x5000_uma_workingset --hold MiB seconds: keep MiB of textures resident and read them all once a second, as a background
// app with a large GPU working set would.
let holdIndex = CommandLine.arguments.firstIndex(of: "--hold")
let sizes = holdIndex != nil ? [] : CommandLine.arguments.dropFirst().compactMap { Int($0) }
let sweep = sizes.isEmpty ? [256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096] : sizes
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal") }
print("DEVICE \(device.name) recommendedWorkingSet=\(device.recommendedMaxWorkingSetSize >> 20) MiB")
let library = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void fill(texture2d<half, access::write> t [[texture(0)]], constant uint& seed [[buffer(0)]],
                 uint2 g [[thread_position_in_grid]]) {
    t.write(half4(half((g.x ^ seed) & 255) / 256.0h, half(g.y & 255) / 256.0h, half(seed & 255) / 256.0h, 1.0h), g);
}
kernel void readAll(texture2d<half, access::read> t [[texture(0)]], device atomic_uint* out [[buffer(0)]],
                    uint2 g [[thread_position_in_grid]]) {
    half4 acc = 0;
    for (uint y = g.y; y < t.get_height(); y += 64) { acc += t.read(uint2(g.x, y)); }
    if (g.x == 0 && g.y == 0) { atomic_fetch_add_explicit(out, uint(acc.z * 256.0h), memory_order_relaxed); }
}
""", options: nil)
let fill = try! device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let readAll = try! device.makeComputePipelineState(function: library.makeFunction(name: "readAll")!)
let side = 2048
let textureMiB = (side * side * 8) >> 20
let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: side, height: side, mipmapped: false)
descriptor.storageMode = .private
descriptor.usage = [.shaderRead, .shaderWrite]
let out = device.makeBuffer(length: 4096, options: .storageModeShared)!

func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

func readPass(_ textures: [MTLTexture]) -> Bool {
    var last: MTLCommandBuffer?
    for texture in textures {
        guard let cb = queue.makeCommandBuffer(), let e = cb.makeComputeCommandEncoder() else { return false }
        e.setComputePipelineState(readAll); e.setTexture(texture, index: 0); e.setBuffer(out, offset: 0, index: 0)
        e.dispatchThreads(MTLSize(width: side, height: 64, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 4, depth: 1))
        e.endEncoding(); cb.commit(); last = cb
    }
    last?.waitUntilCompleted()
    return last?.status == .completed
}

if let holdIndex, holdIndex + 2 < CommandLine.arguments.count, let mib = Int(CommandLine.arguments[holdIndex + 1]),
   let seconds = Double(CommandLine.arguments[holdIndex + 2]) {
    var textures: [MTLTexture] = []
    for index in 0..<(mib / textureMiB) {
        guard let texture = device.makeTexture(descriptor: descriptor), let cb = queue.makeCommandBuffer(),
              let e = cb.makeComputeCommandEncoder() else { print("HOLD ALLOC_FAILED at \(textures.count)"); exit(1) }
        var seed = UInt32(index)
        e.setComputePipelineState(fill); e.setTexture(texture, index: 0); e.setBytes(&seed, length: 4, index: 0)
        e.dispatchThreads(MTLSize(width: side, height: side, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        e.endEncoding(); cb.commit(); textures.append(texture)
    }
    print("HOLD \(mib) MiB ready")
    let end = now() + seconds * 1000
    var passes: [Double] = []
    while now() < end {
        let start = now()
        if !readPass(textures) { print("HOLD GPU_ERROR"); exit(1) }
        passes.append(now() - start)
        usleep(1_000_000)
    }
    let sorted = passes.sorted()
    print(String(format: "HOLD %d MiB passes=%d pass_median=%.1f ms pass_max=%.1f ms", mib, passes.count,
                 sorted[sorted.count / 2], sorted.last!))
    exit(0)
}

for mib in sweep {
    let count = mib / textureMiB
    var textures: [MTLTexture] = []
    var failed = false
    let t0 = now()
    for index in 0..<count {
        guard let texture = device.makeTexture(descriptor: descriptor), let cb = queue.makeCommandBuffer(),
              let e = cb.makeComputeCommandEncoder() else { failed = true; break }
        var seed = UInt32(index)
        e.setComputePipelineState(fill); e.setTexture(texture, index: 0); e.setBytes(&seed, length: 4, index: 0)
        e.dispatchThreads(MTLSize(width: side, height: side, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        e.endEncoding(); cb.commit()
        textures.append(texture)
        if index % 8 == 7 { cb.waitUntilCompleted(); if cb.status != .completed { failed = true; break } }
    }
    let setup = now() - t0
    if failed { print("WORKINGSET \(mib) MiB FAILED at \(textures.count) textures"); continue }
    var passes: [Double] = []
    var errors = 0
    for pass in 0..<6 {
        let start = now()
        var last: MTLCommandBuffer?
        for texture in textures {
            guard let cb = queue.makeCommandBuffer(), let e = cb.makeComputeCommandEncoder() else { errors += 1; continue }
            e.setComputePipelineState(readAll); e.setTexture(texture, index: 0); e.setBuffer(out, offset: 0, index: 0)
            e.dispatchThreads(MTLSize(width: side, height: 64, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 4, depth: 1))
            e.endEncoding(); cb.commit(); last = cb
        }
        last?.waitUntilCompleted()
        if last?.status != .completed { errors += 1 }
        if pass > 0 { passes.append(now() - start) }
    }
    let sorted = passes.sorted()
    let median = sorted[sorted.count / 2]
    print(String(format: "WORKINGSET %d MiB textures=%d setup=%.0f ms pass_median=%.1f ms pass_max=%.1f ms rate=%.1f GB/s errors=%d",
                 mib, count, setup, median, sorted.last!, Double(mib) / 1024 / (median / 1000), errors))
    textures.removeAll()
}
