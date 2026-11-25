// Editing-style render: L source layers of 3840x2160 RGBA16Float stay resident; every output frame blurs and
// color-grades each layer and blends it into the frame, which goes into a cache of C rendered frames (the editor's
// render cache). Reports frames per second and the slow-frame share.
//   x5000_uma_timeline <layers> <cache> <frames> <label>
import Foundation
import Metal
import MetalPerformanceShaders

setvbuf(stdout, nil, _IOLBF, 0)
let a = CommandLine.arguments
let layers = a.count > 1 ? Int(a[1])! : 8, cache = a.count > 2 ? Int(a[2])! : 12
let frames = a.count > 3 ? Int(a[3])! : 240, label = a.count > 4 ? a[4] : "timeline"
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal") }
let library = try! device.makeLibrary(source: """
#include <metal_stdlib>
using namespace metal;
kernel void fill(texture2d<half, access::write> t [[texture(0)]], constant uint& seed [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    t.write(half4(half((g.x + seed * 37) & 255) / 256.0h, half((g.y ^ seed) & 255) / 256.0h, half(seed & 15) / 16.0h, 1.0h), g);
}
kernel void grade(texture2d<half, access::read> src [[texture(0)]], texture2d<half, access::read_write> dst [[texture(1)]],
                  constant float& weight [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    half4 c = src.read(g);
    c.rgb = clamp(c.rgb * half3(1.05h, 0.98h, 1.02h) + half3(0.01h), 0.0h, 1.0h);
    half4 d = dst.read(g);
    dst.write(mix(d, c, half(weight)), g);
}
""", options: nil)
let fill = try! device.makeComputePipelineState(function: library.makeFunction(name: "fill")!)
let grade = try! device.makeComputePipelineState(function: library.makeFunction(name: "grade")!)
let w = 3840, h = 2160
let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: w, height: h, mipmapped: false)
d.storageMode = .private; d.usage = [.shaderRead, .shaderWrite]
let grid = MTLSize(width: w, height: h, depth: 1), group = MTLSize(width: 16, height: 16, depth: 1)
func make() -> MTLTexture { guard let t = device.makeTexture(descriptor: d) else { print("TIMELINE \(label) ALLOC_FAILED"); exit(1) }; return t }
var sources: [MTLTexture] = []
for i in 0..<layers {
    let t = make(); let cb = queue.makeCommandBuffer()!, e = cb.makeComputeCommandEncoder()!
    var seed = UInt32(i); e.setComputePipelineState(fill); e.setTexture(t, index: 0); e.setBytes(&seed, length: 4, index: 0)
    e.dispatchThreads(grid, threadsPerThreadgroup: group); e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
    sources.append(t)
}
let scratch = make()
let output = (0..<cache).map { _ in make() }
let blur = MPSImageGaussianBlur(device: device, sigma: 2.0)
let mib = ((layers + cache + 1) * w * h * 8) >> 20
print("TIMELINE \(label) layers=\(layers) cache=\(cache) working_set=\(mib) MiB")
func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }
var times: [Double] = []
var errors = 0
let start = now()
for f in 0..<frames {
    let t0 = now()
    let target = output[f % cache]
    let cb = queue.makeCommandBuffer()!
    for (i, layer) in sources.enumerated() {
        blur.encode(commandBuffer: cb, sourceTexture: layer, destinationTexture: scratch)
        let e = cb.makeComputeCommandEncoder()!
        var weight: Float = i == 0 ? 1.0 : 0.5
        e.setComputePipelineState(grade); e.setTexture(scratch, index: 0); e.setTexture(target, index: 1)
        e.setBytes(&weight, length: 4, index: 0); e.dispatchThreads(grid, threadsPerThreadgroup: group); e.endEncoding()
    }
    cb.commit(); cb.waitUntilCompleted()
    if cb.status != .completed { errors += 1 }
    times.append(now() - t0)
}
let total = now() - start
let s = times.sorted()
let slow = times.filter { $0 > 2 * s[s.count / 2] }.count
print(String(format: "TIMELINE %@ fps=%.2f frame_p50=%.1f ms frame_p95=%.1f ms frame_max=%.1f ms slow_frames=%d/%d errors=%d",
             label, Double(frames) / (total / 1000), s[s.count / 2], s[s.count * 95 / 100], s.last!, slow, frames, errors))
