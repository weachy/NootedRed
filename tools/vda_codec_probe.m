#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

// Public VideoToolbox APIs only. This is a correctness probe, not a throughput
// benchmark: CPU pattern generation and pixel readback/hashing are included.
typedef struct {
    FILE *hashes;
    _Atomic int frames, errors, dropped;
} DecodeState;

static void decoded(void *opaque, void *frame, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    DecodeState *s = opaque;
    if (status || !image || (flags & kVTDecodeInfo_FrameDropped)) {
        if (status) s->errors++; else s->dropped++;
        printf("decode_callback status=%d flags=%u\n", (int)status, flags);
        return;
    }
    CVPixelBufferRef pb = image;
    OSType format = CVPixelBufferGetPixelFormatType(pb);
    if (format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
        format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
        format != kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange) {
        s->errors++; printf("unexpected pixel format=%08x\n", (unsigned)format); return;
    }
    CVReturn st = CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (st) { s->errors++; return; }
    CC_MD5_CTX hash; CC_MD5_Init(&hash);
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    int bytes = format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? 2 : 1;
    if (CVPixelBufferGetPlaneCount(pb) != 2 || w % 2 || h % 2) {
        s->errors++; CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); return;
    }
    for (int plane = 0; plane < 2; plane++) {
        unsigned char *data = CVPixelBufferGetBaseAddressOfPlane(pb, plane);
        size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pb, plane);
        if (!data || stride < w * bytes) {
            s->errors++; CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); return;
        }
        for (size_t y = 0; y < h / (plane ? 2 : 1); y++)
            CC_MD5_Update(&hash, data + y * stride, (CC_LONG)(w * bytes));
    }
    unsigned char digest[16]; CC_MD5_Final(digest, &hash);
    if (!CMTIME_IS_NUMERIC(pts) || pts.timescale <= 0) {
        s->errors++; CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); return;
    }
    flockfile(s->hashes);
    fprintf(s->hashes, "%lld/%d ", (long long)pts.value, pts.timescale);
    for (int i = 0; i < 16; i++) fprintf(s->hashes, "%02x", digest[i]);
    fprintf(s->hashes, " %zu %zu %s\n", w, h, bytes == 2 ? "p010le" : "nv12");
    funlockfile(s->hashes);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    s->frames++;
}

static int decodeSamples(NSArray *samples, const char *prefix, int tenbit) {
    if (!samples.count) return 2;
    NSString *path = [NSString stringWithFormat:@"%s.hw.md5", prefix];
    DecodeState state = {.hashes = fopen(path.UTF8String, "w")};
    if (!state.hashes) return 2;
    CMSampleBufferRef first = (__bridge CMSampleBufferRef)samples[0];
    NSDictionary *spec = @{(__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder:@YES};
    NSMutableDictionary *attrs = [@{
        (__bridge id)kCVPixelBufferPixelFormatTypeKey:@(tenbit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
                                                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge id)kCVPixelBufferIOSurfacePropertiesKey:@{}
    } mutableCopy];
    if (tenbit == 2) [attrs removeObjectForKey:(__bridge id)kCVPixelBufferPixelFormatTypeKey];
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(first);
    CMVideoDimensions dims = format ? CMVideoFormatDescriptionGetDimensions(format) : (CMVideoDimensions){0,0};
    printf("first_format=%08x dimensions=%dx%d data=%zu samples=%ld\n",
        format ? (unsigned)CMFormatDescriptionGetMediaSubType(format) : 0,dims.width,dims.height,
        CMSampleBufferGetTotalSampleSize(first),(long)CMSampleBufferGetNumSamples(first));
    VTDecompressionSessionRef session = NULL;
    VTDecompressionOutputCallbackRecord callback = {decoded, &state};
    OSStatus st = VTDecompressionSessionCreate(NULL, CMSampleBufferGetFormatDescription(first),
        (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs, &callback, &session);
    printf("decode_create=%d\n", (int)st);
    if (st) { fclose(state.hashes); return 2; }
    CFTypeRef hw = NULL;
    st = VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
    bool isHW = !st && hw == kCFBooleanTrue;
    printf("decode_hardware=%d query_status=%d\n", isHW, (int)st);
    if (hw) CFRelease(hw);
    double started = CFAbsoluteTimeGetCurrent();
    for (id object in samples) {
        VTDecodeInfoFlags info = 0;
        st = VTDecompressionSessionDecodeFrame(session, (__bridge CMSampleBufferRef)object, 0, NULL, &info);
        if (st) { state.errors++; printf("decode_submit_error=%d\n", (int)st); }
    }
    st = VTDecompressionSessionFinishDelayedFrames(session);
    OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(session);
    printf("decode_frames=%d errors=%d dropped=%d submitted=%lu finish=%d wait=%d seconds_including_hash=%.3f\n",
        state.frames,state.errors,state.dropped,(unsigned long)samples.count,(int)st,(int)wait,
        CFAbsoluteTimeGetCurrent()-started);
    VTDecompressionSessionInvalidate(session); CFRelease(session); fclose(state.hashes);
    return !isHW || state.errors || state.dropped || state.frames != samples.count || st || wait ? 2 : 0;
}

typedef struct { NSMutableArray *samples; _Atomic int errors; } EncodeState;
static void encoded(void *opaque, void *frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    EncodeState *s = opaque;
    if (status || !sample || (flags & kVTEncodeInfo_FrameDropped)) {
        s->errors++; printf("encode_callback=%d flags=%u\n",(int)status,flags); return;
    }
    @synchronized(s->samples) { [s->samples addObject:(__bridge id)sample]; }
}

static int saveAnnexB(NSArray *samples, CMVideoCodecType codec, const char *path) {
    FILE *file = fopen(path, "wb"); if (!file) return 2;
    const unsigned char start[] = {0,0,0,1};
    CMVideoFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription((__bridge CMSampleBufferRef)samples[0]);
    size_t sets = 0; int nalLength = 0; OSStatus st;
    const uint8_t *bytes; size_t size;
    if (codec == kCMVideoCodecType_H264)
        st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &bytes, &size, &sets, &nalLength);
    else st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, &bytes, &size, &sets, &nalLength);
    if (st || nalLength < 1 || nalLength > 4) { fclose(file); return 2; }
    for (size_t i = 0; i < sets; i++) {
        if (codec == kCMVideoCodecType_H264)
            st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt,i,&bytes,&size,NULL,NULL);
        else st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt,i,&bytes,&size,NULL,NULL);
        if (st) { fclose(file); return 2; }
        fwrite(start,1,4,file); fwrite(bytes,1,size,file);
    }
    for (id object in samples) {
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer((__bridge CMSampleBufferRef)object);
        size_t total = CMBlockBufferGetDataLength(block);
        NSMutableData *data = [NSMutableData dataWithLength:total];
        if (CMBlockBufferCopyDataBytes(block,0,total,data.mutableBytes)) { fclose(file); return 2; }
        unsigned char *p = data.mutableBytes; size_t at = 0;
        while (at + nalLength <= total) {
            uint32_t n = 0;
            for (int k=0;k<nalLength;k++) n=(n<<8)|p[at++];
            if (n > total-at) { fclose(file); return 2; }
            fwrite(start,1,4,file); fwrite(p+at,1,n,file); at+=n;
        }
        if (at != total) { fclose(file); return 2; }
    }
    int error = ferror(file); if (fclose(file)) error = 1; return error ? 2 : 0;
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc >= 4 && !strcmp(argv[1],"decode")) {
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[2])] options:nil];
        AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        NSError *error = nil;
        AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!reader || !track) { NSLog(@"asset setup %@",error); return 2; }
        AVAssetReaderTrackOutput *output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:nil];
        if (![reader canAddOutput:output]) { NSLog(@"asset setup %@",error); return 2; }
        [reader addOutput:output];
        if (![reader startReading]) { NSLog(@"asset read %@",reader.error); return 2; }
        NSMutableArray *samples = [NSMutableArray array]; CMSampleBufferRef sample;
        while ((sample = [output copyNextSampleBuffer])) {
            if (CMSampleBufferGetFormatDescription(sample) && CMSampleBufferGetNumSamples(sample)>0 &&
                CMSampleBufferGetTotalSampleSize(sample)>0)
                [samples addObject:(__bridge id)sample];
            else printf("skip_marker format=%p samples=%ld bytes=%zu\n",CMSampleBufferGetFormatDescription(sample),
                (long)CMSampleBufferGetNumSamples(sample),CMSampleBufferGetTotalSampleSize(sample));
            CFRelease(sample);
            if (samples.count >= 120) break;
        }
        printf("asset_samples=%lu read_status=%ld\n",(unsigned long)samples.count,(long)reader.status);
        return decodeSamples(samples,argv[3],argc>4 ? (!strcmp(argv[4],"auto") ? 2 : !strcmp(argv[4],"10")) : 0);
    }
    if (argc != 7 || (strcmp(argv[1], "encode") && strcmp(argv[1], "encode-only")) ||
        (strcmp(argv[2], "h264") && strcmp(argv[2], "hevc") && strcmp(argv[2], "hevc10"))) {
        fprintf(stderr,"usage: probe encode|encode-only h264|hevc|hevc10 W H N prefix | decode file prefix [10]\n");
        return 1;
    }
    bool tenbit = !strcmp(argv[2], "hevc10");
    CMVideoCodecType codec = !strcmp(argv[2],"h264") ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
    int w=atoi(argv[3]), h=atoi(argv[4]), n=atoi(argv[5]); const char *prefix=argv[6];
    if (w<16 || h<16 || w%2 || h%2 || n<1 || n>120) return 1;
    // Square HEVC is a separate revision-9 test. Require its explicit gate so
    // running an older generic regression cannot submit the known bad command.
    bool squareHEVC = codec == kCMVideoCodecType_HEVC && w == 4096 && h == 4096 &&
        getenv("NRED_PROBE_SQUARE_HEVC") && !strcmp(getenv("NRED_PROBE_SQUARE_HEVC"), "1");
    if (w > 4096 || (h > (codec == kCMVideoCodecType_H264 ? 4096 : 2304) && !squareHEVC)) {
        fprintf(stderr, "outside verified bridge encode bounds; no session created\n");
        return 1;
    }
    EncodeState state = {.samples=[NSMutableArray array]};
    NSDictionary *spec=@{(__bridge id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES};
    OSType sourceFormat = tenbit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange :
                                  kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    NSDictionary *attrs=@{(__bridge id)kCVPixelBufferPixelFormatTypeKey:@(sourceFormat),
        (__bridge id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    VTCompressionSessionRef session=NULL;
    OSStatus st=VTCompressionSessionCreate(NULL,w,h,codec,(__bridge CFDictionaryRef)spec,
        (__bridge CFDictionaryRef)attrs,NULL,encoded,&state,&session);
    printf("encode_create=%d size=%dx%d\n",(int)st,w,h); if(st) return 2;
    NSMutableDictionary *settings=[@{
        (__bridge id)kVTCompressionPropertyKey_ProfileLevel:(__bridge id)(
            codec == kCMVideoCodecType_H264 ? kVTProfileLevel_H264_High_AutoLevel :
            tenbit ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel),
        (__bridge id)kVTCompressionPropertyKey_RealTime:@YES,
        (__bridge id)kVTCompressionPropertyKey_AllowFrameReordering:@NO,
        (__bridge id)kVTCompressionPropertyKey_AverageBitRate:@(w>=3000?20000000:6000000),
        (__bridge id)kVTCompressionPropertyKey_ExpectedFrameRate:@60
    } mutableCopy];
    NSString *extra = [[[NSProcessInfo processInfo] environment] objectForKey:@"NRED_PROBE_SETTINGS"];
    if (extra) {
        NSError *error = nil;
        id overrides = [NSJSONSerialization JSONObjectWithData:[extra dataUsingEncoding:NSUTF8StringEncoding]
                                                       options:0 error:&error];
        if (![overrides isKindOfClass:[NSDictionary class]]) {
            NSLog(@"invalid probe settings: %@", error);
            VTCompressionSessionInvalidate(session); CFRelease(session); return 1;
        }
        [settings addEntriesFromDictionary:overrides];
    }
    int settingErrors = 0;
    for (id key in settings) {
        st=VTSessionSetProperty(session,(__bridge CFStringRef)key,(__bridge CFTypeRef)settings[key]);
        printf("property=%s status=%d\n",[key UTF8String],(int)st);
        if (st) settingErrors++;
    }
    if (settingErrors) {
        printf("required_property_errors=%d\n", settingErrors);
        VTCompressionSessionInvalidate(session); CFRelease(session); return 2;
    }
    st=VTCompressionSessionPrepareToEncodeFrames(session);
    printf("encode_prepare=%d\n",(int)st); if(st) return 2;
    CFTypeRef hw=NULL;
    st=VTSessionCopyProperty(session,kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,NULL,&hw);
    bool isHW=!st && hw==kCFBooleanTrue;
    printf("encode_hardware=%d query_status=%d\n",isHW,(int)st); if(hw) CFRelease(hw);
    CFTypeRef encoder=NULL;
    st=VTSessionCopyProperty(session,kVTCompressionPropertyKey_EncoderID,NULL,&encoder);
    if(!st && encoder) { CFShow(encoder); CFRelease(encoder); }
    CVPixelBufferPoolRef pool=VTCompressionSessionGetPixelBufferPool(session);
    if(!pool) { printf("no_encoder_pool\n"); VTCompressionSessionInvalidate(session); CFRelease(session); return 2; }
    double started=CFAbsoluteTimeGetCurrent();
    for(int frame=0;frame<n;frame++) {
        CVPixelBufferRef pb=NULL;
        CVReturn rv=CVPixelBufferPoolCreatePixelBuffer(NULL,pool,&pb);
        if(rv) {state.errors++;break;}
        if (CVPixelBufferLockBaseAddress(pb,0)) {state.errors++; CVPixelBufferRelease(pb); break;}
        if (CVPixelBufferGetPixelFormatType(pb) != sourceFormat || CVPixelBufferGetPlaneCount(pb) != 2 ||
            CVPixelBufferGetWidth(pb) != w || CVPixelBufferGetHeight(pb) != h ||
            CVPixelBufferGetIOSurface(pb) == NULL) {
            state.errors++; CVPixelBufferUnlockBaseAddress(pb,0); CVPixelBufferRelease(pb); break;
        }
        unsigned char *y=CVPixelBufferGetBaseAddressOfPlane(pb,0),*uv=CVPixelBufferGetBaseAddressOfPlane(pb,1);
        size_t ys=CVPixelBufferGetBytesPerRowOfPlane(pb,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(pb,1);
        if(!frame) printf("source_pitch_y=%zu uv=%zu\n",ys,uvs);
        if (!y || !uv || ys < w * (tenbit ? 2 : 1) || uvs < w * (tenbit ? 2 : 1)) {
            state.errors++; CVPixelBufferUnlockBaseAddress(pb,0); CVPixelBufferRelease(pb); break;
        }
        for(int j=0;j<h;j++) for(int i=0;i<w;i++) {
            unsigned value=16+(i/8+j/8+frame*3)%220;
            // Exercise all ten source bits; an 8-bit pattern shifted left by
            // two cannot validate the P010 input path's low-bit handling.
            if (tenbit) ((uint16_t*)(y+j*ys))[i]=(value*4+(i+j+frame)%4)<<6; else y[j*ys+i]=value;
        }
        for(int j=0;j<h/2;j++) for(int i=0;i<w;i+=2) {
            unsigned u=64+(j+frame)%128, v=64+(i/2+frame)%128;
            if (tenbit) {
                ((uint16_t*)(uv+j*uvs))[i]=(u*4+(i/2+j+frame)%4)<<6;
                ((uint16_t*)(uv+j*uvs))[i+1]=(v*4+(i/2+2*j+frame+1)%4)<<6;
            } else {uv[j*uvs+i]=u;uv[j*uvs+i+1]=v;}
        }
        CVPixelBufferUnlockBaseAddress(pb,0);
        VTEncodeInfoFlags flags=0;
        st=VTCompressionSessionEncodeFrame(session,pb,CMTimeMake(frame,60),CMTimeMake(1,60),NULL,NULL,&flags);
        CVPixelBufferRelease(pb);
        if(st) {state.errors++;printf("encode_submit_error=%d\n",(int)st);break;}
    }
    st=VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);
    printf("encode_frames=%lu errors=%d complete=%d seconds_including_fill=%.3f\n",
        (unsigned long)state.samples.count,state.errors,(int)st,CFAbsoluteTimeGetCurrent()-started);
    VTCompressionSessionInvalidate(session);CFRelease(session);
    if(!isHW || st || state.errors || state.samples.count!=n) return 2;
    NSString *path=[NSString stringWithFormat:@"%s.%s",prefix,codec==kCMVideoCodecType_H264?"h264":"hevc"];
    if(saveAnnexB(state.samples,codec,path.UTF8String)) return 2;
    // Square AVC encoding can be verified by an independent CPU decoder even
    // while square hardware decoding is unsupported. Do not conflate them.
    if (!strcmp(argv[1], "encode-only")) return 0;
    return decodeSamples(state.samples,prefix,tenbit);
} }
