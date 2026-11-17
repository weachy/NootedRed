// Drives a vp09 decoder through the public VideoToolbox API, the way Safari
// does.
//   vtvp9test [-load bundle] list
//   vtvp9test [-load bundle] file [reference] [-n] [-a] [-pace fps] [-loop N]
//   [-frames N] [-id decoder] [-sw]
// -load registers the bundle in this process only, nothing is installed on the
// system. -n reads no pixels, -a decodes asynchronously, -pace feeds at a fixed
// rate and reports lateness, -loop feeds the file N times in one session (a
// seek back to a keyframe), -sw doesn't ask for hardware.
#include <CommonCrypto/CommonDigest.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../vcnvp9/container.h"
#include "../vcnvp9/vp9parse.h"

static double now_ms(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static void copy_kv(const void* k, const void* v, void* dict) {
  CFDictionarySetValue(dict, k, v);
}

static int load_bundle(const char* path) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      NULL, (const UInt8*)path, (CFIndex)strlen(path), true);
  CFBundleRef b = CFBundleCreate(NULL, url);
  CFRelease(url);
  if (!b || !CFBundleLoadExecutable(b)) {
    fprintf(stderr, "cannot load %s\n", path);
    return -1;
  }
  CFArrayRef impls =
      CFBundleGetValueForInfoDictionaryKey(b, CFSTR("CMClassImplementations"));
  OSStatus (*reg)(FourCharCode, CFDictionaryRef, void*) =
      dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoderWithInfo");
  for (CFIndex i = 0; impls && reg && i < CFArrayGetCount(impls); i++) {
    CFDictionaryRef impl = CFArrayGetValueAtIndex(impls, i);
    void* factory = CFBundleGetFunctionPointerForName(
        b, CFDictionaryGetValue(impl, CFSTR("CMFactoryFunction")));
    CFMutableDictionaryRef info = CFDictionaryCreateMutableCopy(NULL, 0, impl);
    // Dynamic registration ignores CMMatchingInfo: lift the matching keys to
    // the top level.
    CFDictionaryRef match = CFDictionaryGetValue(impl, CFSTR("CMMatchingInfo"));
    if (match) CFDictionaryApplyFunction(match, copy_kv, info);
    CFDictionarySetValue(info, CFSTR("IsHardwareAccelerated"), kCFBooleanTrue);
    CFDictionarySetValue(
        info, CFSTR("CodecName"),
        CFDictionaryGetValue(impl, CFSTR("CMClassImplementationName")));
    OSStatus st = reg(kCMVideoCodecType_VP9, info, factory);
    CFRelease(info);
    printf("registered %p from %s -> %d\n", factory, path, (int)st);
  }
  return 0;
}

static int list(void) {
  OSStatus (*copyList)(CFDictionaryRef, CFArrayRef*) =
      dlsym(RTLD_DEFAULT, "VTCopyVideoDecoderList");
  CFArrayRef decs = NULL;
  if (copyList) copyList(NULL, &decs);
  printf("VTCopyVideoDecoderList: %ld decoders\n",
         decs ? (long)CFArrayGetCount(decs) : 0L);
  for (CFIndex i = 0; decs && i < CFArrayGetCount(decs); i++) {
    CFStringRef desc = CFCopyDescription(CFArrayGetValueAtIndex(decs, i));
    if (CFStringFind(desc, CFSTR("vp09"), 0).location != kCFNotFound ||
        CFStringFind(desc, CFSTR("VP9"), 0).location != kCFNotFound)
      CFShow(CFArrayGetValueAtIndex(decs, i));
    CFRelease(desc);
  }
  printf("VTIsHardwareDecodeSupported(vp09) = %d\n",
         VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9));
  if (decs) CFRelease(decs);
  return 0;
}

typedef struct {
  FILE* ref;
  int frames, mismatches, errors, dropped, nohash;
  double *fed, *lat;
} Ctx;

static bool next_ref(Ctx* c, char out[33]) {
  char line[512];
  while (c->ref && fgets(line, sizeof(line), c->ref)) {
    if (line[0] == '#') continue;
    char* h = strrchr(line, ',');
    if (!h) continue;
    return sscanf(h + 1, " %32s", out) == 1;
  }
  return false;
}

static void out_cb(void* refcon, void* frameRefcon, OSStatus st,
                   VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts,
                   CMTime dur) {
  Ctx* c = refcon;
  if (c->lat) {
    long i = (long)frameRefcon;
    c->lat[i] = now_ms() - c->fed[i];
  }
  if (st) {
    c->errors++;
    printf("frame error %d flags %x\n", (int)st, flags);
    return;
  }
  if (!img) {
    c->dropped++;
    return;
  }
  c->frames++;
  if (c->nohash) return;
  CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  int ten = CVPixelBufferGetPixelFormatType(img) ==
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
  int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
  uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0),
          *uv = CVPixelBufferGetBaseAddressOfPlane(img, 1);
  size_t ys = CVPixelBufferGetBytesPerRowOfPlane(img, 0),
         uvs = CVPixelBufferGetBytesPerRowOfPlane(img, 1);
  CC_MD5_CTX m;
  CC_MD5_Init(&m);
  int cw = (w + 1) / 2, ch = (h + 1) / 2;
  uint16_t* row = malloc((size_t)w * 2 + 16);
  if (!row) {
    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    return;
  }
  for (int r = 0; r < h; r++) {
    if (ten) {
      uint16_t* src = (uint16_t*)(y + r * ys);
      for (int i = 0; i < w; i++) row[i] = src[i] >> 6;
      CC_MD5_Update(&m, row, (CC_LONG)w * 2);
    } else
      CC_MD5_Update(&m, y + r * ys, (CC_LONG)w);
  }
  for (int pl = 0; pl < 2; pl++)
    for (int r = 0; r < ch; r++) {
      if (ten) {
        uint16_t* src = (uint16_t*)(uv + r * uvs);
        for (int i = 0; i < cw; i++) row[i] = src[2 * i + pl] >> 6;
        CC_MD5_Update(&m, row, (CC_LONG)cw * 2);
      } else {
        uint8_t* b = (uint8_t*)row;
        for (int i = 0; i < cw; i++) b[i] = uv[r * uvs + 2 * i + pl];
        CC_MD5_Update(&m, b, (CC_LONG)cw);
      }
    }
  free(row);
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  uint8_t dg[16];
  char hex[33], expect[33] = "";
  CC_MD5_Final(dg, &m);
  for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
  if (c->ref) {
    bool have = next_ref(c, expect);
    if (!have || strcmp(expect, hex)) {
      c->mismatches++;
      printf("frame %d %dx%d %s MISMATCH expected %s\n", c->frames, w, h, hex,
             have ? expect : "(reference ended)");
    }
  }
}

// One frame's size and depth without a decoder. The first frame of a stream is
// enough.
typedef struct {
  int w, h, depth, profile, found;
} FirstFrame;
static int first_frame(void* ctx, VP9Parser* p, VP9ParseResult kind,
                       const uint8_t* d, size_t n) {
  FirstFrame* f = ctx;
  if (kind != VP9_PARSE_SHOW_EXISTING && !f->found) {
    f->w = (int)p->fh.width;
    f->h = (int)p->fh.height;
    f->depth = p->fh.bit_depth;
    f->profile = p->fh.profile;
    f->found = 1;
  }
  if (kind != VP9_PARSE_SHOW_EXISTING) vp9_frame_done(p, 0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: vtvp9test [-load bundle] list | file [reference] [-n] [-a] "
            "[-pace fps] [-loop N] [-frames N] [-id decoder] [-sw]\n");
    return 1;
  }
  // Without this VideoToolbox refuses every vp09 decoder in this process, the
  // real ones too: it is what sets AllowAlternateDecoderSelection on the
  // decoder specification.
  VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
  if (argc > 2 && !strcmp(argv[1], "-load")) {
    if (load_bundle(argv[2])) return 1;
    argv += 2;
    argc -= 2;
  }
  if (!strcmp(argv[1], "list")) return list();
  Ctx c = {0};
  int async = 0, loops = 1, limit = 0, require_hw = 1;
  double pace = 0;
  const char *decoderID = NULL, *refPath = NULL;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "-n"))
      c.nohash = 1;
    else if (!strcmp(argv[i], "-a"))
      async = 1;
    else if (!strcmp(argv[i], "-sw"))
      require_hw = 0;
    else if (!strcmp(argv[i], "-pace") && i + 1 < argc) {
      const char* s = argv[++i];
      char* end = NULL;
      double v = strtod(s, &end);
      if (!end || end == s || *end || v < 0) {
        fprintf(stderr, "invalid -pace\n");
        return 1;
      }
      pace = v;
    } else if (!strcmp(argv[i], "-loop") && i + 1 < argc) {
      const char* s = argv[++i];
      char* end = NULL;
      long v = strtol(s, &end, 10);
      if (!end || end == s || *end || v < 1 || v > INT_MAX) {
        fprintf(stderr, "invalid -loop\n");
        return 1;
      }
      loops = (int)v;
    } else if (!strcmp(argv[i], "-frames") && i + 1 < argc) {
      const char* s = argv[++i];
      char* end = NULL;
      long v = strtol(s, &end, 10);
      if (!end || end == s || *end || v < 0 || v > INT_MAX) {
        fprintf(stderr, "invalid -frames\n");
        return 1;
      }
      limit = (int)v;
    } else if (!strcmp(argv[i], "-id") && i + 1 < argc)
      decoderID = argv[++i];
    else
      refPath = argv[i];
  }
  Container f;
  if (!container_open(&f, argv[1])) {
    perror(argv[1]);
    return 1;
  }
  size_t cap = 4096, cnt = 0;
  const uint8_t** chunks = malloc(cap * sizeof(*chunks));
  size_t* sizes = malloc(cap * sizeof(*sizes));
  if (!chunks || !sizes) {
    free(chunks);
    free(sizes);
    container_close(&f);
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  const uint8_t* data;
  size_t size;
  while ((!limit || (int)cnt < limit) && container_next(&f, &data, &size)) {
    if (cnt == cap) {
      if (cap > SIZE_MAX / 2) {
        free(chunks);
        free(sizes);
        container_close(&f);
        fprintf(stderr, "too many frames\n");
        return 1;
      }
      size_t ncap = cap * 2;
      const uint8_t** nchunks = realloc(chunks, ncap * sizeof(*chunks));
      if (!nchunks) {
        free(chunks);
        free(sizes);
        container_close(&f);
        fprintf(stderr, "out of memory\n");
        return 1;
      }
      chunks = nchunks;
      size_t* nsizes = realloc(sizes, ncap * sizeof(*sizes));
      if (!nsizes) {
        free(chunks);
        free(sizes);
        container_close(&f);
        fprintf(stderr, "out of memory\n");
        return 1;
      }
      sizes = nsizes;
      cap = ncap;
    }
    chunks[cnt] = data;
    sizes[cnt++] = size;
  }
  if (!cnt) {
    fprintf(stderr, "no frames in %s\n", argv[1]);
    return 1;
  }
  VP9Parser ps;
  vp9_parser_init(&ps);
  FirstFrame ff = {0};
  vp9_parse_chunk(&ps, chunks[0], sizes[0], first_frame, &ff);
  if (!ff.found) {
    fprintf(stderr, "no frame header in the first chunk\n");
    return 1;
  }

  // Level 6.2 covers the 8K diagnostic clips (including 8192x4352).
  uint8_t level = (ff.w > 4096 || ff.h > 2304) ? 62 : 51;
  uint8_t vpcC[12] = {
      1, 0, 0, 0, (uint8_t)ff.profile, level, (uint8_t)(ff.depth << 4 | 1 << 1), 1,
      1, 1, 0, 0};
  CFDataRef vc = CFDataCreate(NULL, vpcC, sizeof(vpcC));
  const void *ak[] = {CFSTR("vpcC")}, *av[] = {vc};
  CFDictionaryRef atoms =
      CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  const void *ek[] =
      {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms},
             *ev[] = {atoms};
  CFDictionaryRef ext =
      CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  CMVideoFormatDescriptionRef fmt = NULL;
  OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9,
                                               ff.w, ff.h, ext, &fmt);
  if (st) {
    fprintf(stderr, "format description %d\n", (int)st);
    return 1;
  }

  CFMutableDictionaryRef spec =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  if (require_hw)
    CFDictionarySetValue(
        spec,
        kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
        kCFBooleanTrue);
  if (decoderID) {
    CFStringRef id =
        CFStringCreateWithCString(NULL, decoderID, kCFStringEncodingUTF8);
    CFDictionarySetValue(spec, CFSTR("DecoderID"), id);
    CFRelease(id);
  }
  VTDecompressionOutputCallbackRecord cb = {out_cb, &c};
  VTDecompressionSessionRef sess = NULL;
  st = VTDecompressionSessionCreate(NULL, fmt, spec, NULL, &cb, &sess);
  printf("VTDecompressionSessionCreate = %d\n", (int)st);
  if (st) return 1;
  CFBooleanRef hw = NULL;
  VTSessionCopyProperty(
      sess, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
      NULL, &hw);
  printf("UsingHardwareAcceleratedVideoDecoder = %s\n",
         hw == kCFBooleanTrue ? "true" : "false");

  size_t total = cnt * (size_t)loops;
  if (pace > 0) {
    c.fed = calloc(total, sizeof(double));
    c.lat = calloc(total, sizeof(double));
  }
  double t0 = now_ms();
  size_t fed = 0;
  for (int l = 0; l < loops; l++) {
    if (refPath) {
      if (async) VTDecompressionSessionWaitForAsynchronousFrames(sess);
      if (c.ref) fclose(c.ref);
      c.ref = fopen(refPath, "r");
    }
    for (size_t i = 0; i < cnt; i++, fed++) {
      if (pace > 0) {
        double due = t0 + (double)fed * 1000.0 / pace;
        while (now_ms() < due) usleep(200);
        c.fed[fed] = due;
      }
      CMBlockBufferRef bb = NULL;
      CMBlockBufferCreateWithMemoryBlock(NULL, (void*)chunks[i], sizes[i],
                                         kCFAllocatorNull, NULL, 0, sizes[i], 0,
                                         &bb);
      CMSampleBufferRef sb = NULL;
      CMSampleTimingInfo ti = {CMTimeMake(1, 60), CMTimeMake((int64_t)fed, 60),
                               kCMTimeInvalid};
      size_t ss = sizes[i];
      CMSampleBufferCreateReady(NULL, bb, fmt, 1, 1, &ti, 1, &ss, &sb);
      VTDecodeInfoFlags info = 0;
      st = VTDecompressionSessionDecodeFrame(
          sess, sb, async ? kVTDecodeFrame_EnableAsynchronousDecompression : 0,
          (void*)(long)fed, &info);
      if (st) {
        printf("DecodeFrame %zu -> %d\n", fed, (int)st);
        c.errors++;
      }
      CFRelease(sb);
      CFRelease(bb);
    }
  }
  VTDecompressionSessionWaitForAsynchronousFrames(sess);
  double t = now_ms() - t0;
  printf(
      "%s: %d frames, %d hidden only, %d mismatches, %d errors, %.1f ms (%.1f "
      "fps%s)\n",
      argv[1], c.frames, c.dropped, c.mismatches, c.errors, t,
      c.frames * 1000.0 / t,
      c.nohash ? (async ? ", async" : "") : ", incl. CPU hashing");
  if (pace > 0) {
    int late33 = 0, late50 = 0;
    double mx = 0, sum = 0;
    for (size_t i = 0; i < total; i++) {
      double l = c.lat[i];
      sum += l;
      if (l > mx) mx = l;
      if (l > 33.3) late33++;
      if (l > 50) late50++;
    }
    printf(
        "paced %.0f fps: latency avg %.1f ms, max %.1f ms, over 33 ms %d, over "
        "50 ms %d (of %zu)\n",
        pace, sum / (double)total, mx, late33, late50, total);
  }
  VTDecompressionSessionInvalidate(sess);
  CFRelease(sess);
  return c.mismatches || c.errors || c.frames + c.dropped != (int)total ? 2 : 0;
}
