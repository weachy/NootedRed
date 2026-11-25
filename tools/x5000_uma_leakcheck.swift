// Cross-process reuse check for pooled GPU memory under -NRedUnifiedMemory.
//   x5000_uma_leakcheck write N [MiB]  N private RGBA16Float textures (64 MiB by default) filled with a marker,
//                                      released, then exit.
//   x5000_uma_leakcheck read N [MiB]   N new private textures of the same size, copied to a shared buffer and scanned.
//   x5000_uma_leakcheck both N [MiB]   write then read in one process. This is the control: same-task reuse may show
//                                      the marker, which shows that the read side can see reused memory.
// Use 16 MiB with the native pool buckets, where 64 MiB textures do not come from the pool.
// Run "both 4" first and expect SAME_TASK_REUSE_VISIBLE above 0, then "write 8" and "read 8" in separate processes
// and expect "READ 8 with_pattern=0". Exit 0 = no marker found (or write done), 3 = marker found, 1 = setup error.
import Metal
import Foundation

let magic: UInt64 = 0x5A5AC0DEF00D1234
guard CommandLine.arguments.count >= 3, let count = Int(CommandLine.arguments[2]),
      let mebibytes = CommandLine.arguments.count >= 4 ? Int(CommandLine.arguments[3]) : 64, (1...256).contains(mebibytes)
else {
    print("usage: x5000_uma_leakcheck write|read|both N [MiB]"); exit(1)
}
let side = Int(Double(mebibytes << 20 / 8).squareRoot())    // 64 MiB: 2896 * 2896 * 8 B
let bytesPerRow = side * 8
let length = bytesPerRow * side
let mode = CommandLine.arguments[1]
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { print("no device"); exit(1) }
let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: side, height: side, mipmapped: false)
descriptor.storageMode = .private
descriptor.usage = [.shaderRead, .shaderWrite]

func write(_ n: Int) {
    guard let source = device.makeBuffer(length: length, options: .storageModeShared) else { print("no buffer"); exit(1) }
    let words = source.contents().bindMemory(to: UInt64.self, capacity: length / 8)
    for i in 0..<(length / 8) { words[i] = magic }
    autoreleasepool {
        var textures: [MTLTexture] = []
        for _ in 0..<n {
            guard let texture = device.makeTexture(descriptor: descriptor), let cb = queue.makeCommandBuffer(),
                  let blit = cb.makeBlitCommandEncoder() else { print("alloc failed"); exit(1) }
            blit.copy(from: source, sourceOffset: 0, sourceBytesPerRow: bytesPerRow, sourceBytesPerImage: length,
                      sourceSize: MTLSize(width: side, height: side, depth: 1), to: texture, destinationSlice: 0,
                      destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            blit.endEncoding(); cb.commit(); cb.waitUntilCompleted()
            guard cb.status == .completed, cb.error == nil else { print("GPU_ERROR \(String(describing: cb.error))"); exit(1) }
            textures.append(texture)
        }
        textures.removeAll()
    }
    print("WROTE \(n)")
}

func read(_ n: Int) -> Int {
    guard let target = device.makeBuffer(length: length, options: .storageModeShared) else { print("no buffer"); exit(1) }
    var found = 0
    var textures: [MTLTexture] = []
    for index in 0..<n {
        guard let texture = device.makeTexture(descriptor: descriptor), let cb = queue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { print("alloc failed"); exit(1) }
        memset(target.contents(), 0, length)
        blit.copy(from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: side, height: side, depth: 1), to: target, destinationOffset: 0,
                  destinationBytesPerRow: bytesPerRow, destinationBytesPerImage: length)
        blit.endEncoding(); cb.commit(); cb.waitUntilCompleted()
        guard cb.status == .completed, cb.error == nil else { print("GPU_ERROR \(String(describing: cb.error))"); exit(1) }
        let words = target.contents().bindMemory(to: UInt64.self, capacity: length / 8)
        var hits = 0
        for i in stride(from: 0, to: length / 8, by: 512) where words[i] == magic { hits += 1 }
        if hits > 0 { found += 1; print("PATTERN texture=\(index) sampled_hits=\(hits)") }
        textures.append(texture)
    }
    print("READ \(n) with_pattern=\(found)")
    return found
}

switch mode {
case "write": write(count); exit(0)
case "read": exit(read(count) > 0 ? 3 : 0)
case "both": write(count); let f = read(count); print("SAME_TASK_REUSE_VISIBLE \(f)"); exit(0)
default: print("bad mode"); exit(1)
}
