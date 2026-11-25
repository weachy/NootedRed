// Private texture churn: create an RGBA16Float texture, write it once from a compute kernel, release it; 30 times.
//   x5000_uma_churn [MiB]   texture size, 64 by default. Prints the median and the total time.
// This is the "create, first use, release" figure in docs/X5000-UnifiedMemory.md.
import Foundation
import Metal
let device = MTLCreateSystemDefaultDevice()!, queue = device.makeCommandQueue()!
let lib = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void writeTex(texture2d<half, access::write> tex [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x < tex.get_width() && gid.y < tex.get_height()) { tex.write(half4(half(gid.x & 255) * 0.001h), gid); }
}
""", options: nil)
let p = try! device.makeComputePipelineState(function: lib.makeFunction(name: "writeTex")!)
let mib = Int(CommandLine.arguments.dropFirst().first ?? "64")!, n = 30
let side = Int(sqrt(Double((mib << 20) / 8)))
var ms: [Double] = []
for _ in 0..<n {
    let t0 = DispatchTime.now().uptimeNanoseconds
    do {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: side, height: side, mipmapped: false)
        d.storageMode = .private; d.usage = [.shaderWrite]
        let tex = device.makeTexture(descriptor: d)!
        let cb = queue.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
        e.setComputePipelineState(p); e.setTexture(tex, index: 0)
        e.dispatchThreads(MTLSize(width: side, height: side, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
        e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    }
    ms.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6)
}
let s = ms.sorted()
print(String(format: "CHURN %d MiB x%d  p50 %.2f ms  total %.0f ms", mib, n, s[n / 2], ms.reduce(0, +)))
