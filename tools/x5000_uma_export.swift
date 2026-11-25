// Editing-style export: L 4K layers stay resident as Metal textures; every output frame composites them through Core
// Image (blur, color controls, per-layer transform, blend) on the GPU into an IOSurface-backed pixel buffer, which the
// hardware HEVC encoder writes to a file. Then the file is decoded once (hardware) to measure playback decode.
//   x5000_uma_export <layers> <frames> <output.mov> <label>
import AVFoundation
import CoreImage
import Foundation
import Metal
import VideoToolbox

setvbuf(stdout, nil, _IOLBF, 0)
let a = CommandLine.arguments
let layers = a.count > 1 ? Int(a[1])! : 6, frames = a.count > 2 ? Int(a[2])! : 240
let path = a.count > 3 ? a[3] : "/tmp/x5000_uma_export.mov", label = a.count > 4 ? a[4] : "export"
let w = 3840, h = 2160
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { fatalError("no Metal") }
let context = CIContext(mtlDevice: device, options: [.cacheIntermediates: false, .workingFormat: CIFormat.RGBAh])
func now() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

// Source layers: distinct procedural content rendered once into private RGBA16Float textures.
let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: w, height: h, mipmapped: false)
d.storageMode = .private; d.usage = [.shaderRead, .shaderWrite]
var sources: [CIImage] = []
let rect = CGRect(x: 0, y: 0, width: w, height: h)
for i in 0..<layers {
    guard let t = device.makeTexture(descriptor: d) else { print("EXPORT \(label) ALLOC_FAILED"); exit(1) }
    let noise = CIFilter(name: "CIRandomGenerator")!.outputImage!.transformed(by: CGAffineTransform(scaleX: CGFloat(2 + i), y: CGFloat(2 + i)))
    let tint = CIImage(color: CIColor(red: CGFloat(i % 3) / 2, green: CGFloat((i + 1) % 3) / 2, blue: CGFloat((i + 2) % 3) / 2, alpha: 1))
    let img = noise.applyingFilter("CIMultiplyCompositing", parameters: [kCIInputBackgroundImageKey: tint]).cropped(to: rect)
    let cb = queue.makeCommandBuffer()!
    context.render(img, to: t, commandBuffer: cb, bounds: rect, colorSpace: CGColorSpaceCreateDeviceRGB())
    cb.commit(); cb.waitUntilCompleted()
    sources.append(CIImage(mtlTexture: t, options: [.colorSpace: CGColorSpaceCreateDeviceRGB()])!)
}

try? FileManager.default.removeItem(atPath: path)
let writer = try! AVAssetWriter(outputURL: URL(fileURLWithPath: path), fileType: .mov)
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.hevc, AVVideoWidthKey: w, AVVideoHeightKey: h,
    AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: 60_000_000, AVVideoExpectedSourceFrameRateKey: 30,
                                      kVTCompressionPropertyKey_RealTime as String: false]])
input.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: w,
    kCVPixelBufferHeightKey as String: h, kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    kCVPixelBufferMetalCompatibilityKey as String: true])
writer.add(input)
guard writer.startWriting() else { print("EXPORT \(label) WRITER_FAILED \(String(describing: writer.error))"); exit(1) }
writer.startSession(atSourceTime: .zero)
print("EXPORT \(label) layers=\(layers) frames=\(frames) source_mib=\((layers * w * h * 8) >> 20)")

var frameTimes: [Double] = []
let start = now()
for f in 0..<frames {
    while !input.isReadyForMoreMediaData { usleep(500) }
    let t0 = now()
    var out = CIImage(color: .black).cropped(to: rect)
    for (i, layer) in sources.enumerated() {
        let phase = CGFloat(f + i * 17)
        let moved = layer.transformed(by: CGAffineTransform(translationX: sin(phase / 30) * 40, y: cos(phase / 40) * 30))
        let graded = moved.applyingFilter("CIColorControls", parameters: [kCIInputSaturationKey: 1.1, kCIInputContrastKey: 1.05])
            .applyingGaussianBlur(sigma: i % 2 == 0 ? 3 : 1).cropped(to: rect)
        out = graded.applyingFilter("CIDissolveTransition", parameters: [kCIInputTargetImageKey: out, kCIInputTimeKey: 0.5])
    }
    var buffer: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, adaptor.pixelBufferPool!, &buffer)
    guard let pb = buffer else { print("EXPORT \(label) POOL_FAILED"); exit(1) }
    context.render(out, to: pb, bounds: rect, colorSpace: CGColorSpaceCreateDeviceRGB())
    if !adaptor.append(pb, withPresentationTime: CMTime(value: CMTimeValue(f), timescale: 30)) {
        print("EXPORT \(label) APPEND_FAILED \(String(describing: writer.error))"); exit(1)
    }
    frameTimes.append(now() - t0)
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
let total = now() - start
guard writer.status == .completed else { print("EXPORT \(label) FINISH_FAILED \(String(describing: writer.error))"); exit(1) }
let s = frameTimes.sorted()
print(String(format: "EXPORT %@ fps=%.2f total=%.1f s frame_p50=%.1f ms frame_p95=%.1f ms frame_max=%.1f ms", label,
             Double(frames) / (total / 1000), total / 1000, s[s.count / 2], s[s.count * 95 / 100], s.last!))

// Hardware decode of the exported file.
let asset = AVURLAsset(url: URL(fileURLWithPath: path))
let reader = try! AVAssetReader(asset: asset)
let semaphore = DispatchSemaphore(value: 0)
var track: AVAssetTrack?
asset.loadTracks(withMediaType: .video) { tracks, _ in track = tracks?.first; semaphore.signal() }
semaphore.wait()
let output = AVAssetReaderTrackOutput(track: track!, outputSettings: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange])
reader.add(output)
reader.startReading()
let d0 = now()
var decoded = 0
while let sample = output.copyNextSampleBuffer() { if CMSampleBufferGetImageBuffer(sample) != nil { decoded += 1 } }
let decodeMs = now() - d0
print(String(format: "DECODE %@ frames=%d fps=%.1f status=%d", label, decoded, Double(decoded) / (decodeMs / 1000), reader.status.rawValue))
