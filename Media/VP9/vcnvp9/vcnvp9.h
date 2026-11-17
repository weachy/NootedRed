// VP9 on the AMD VCN through AMDRadeonVADriver2, which has no VP9 code of its
// own. It reuses the driver's AV1 context (session, buffers, submission) and
// replaces the message building on that one processor object. Nothing in the
// driver's memory is patched.

#ifndef VCNVP9_VCNVP9_H_
#define VCNVP9_VCNVP9_H_

#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vp9parse.h"
#include "vp9limits.h"

typedef struct VcnVP9 VcnVP9;
typedef void (*VcnVP9OutputFn)(void* ctx, CVPixelBufferRef pb,
                               const VP9Parser* p, uint32_t width,
                               uint32_t height, int64_t tag);

extern int vcnvp9_debug;
// The plugin sets this. The command line tool arms itself through the
// environment instead.
extern int vcnvp9_gpu_armed;

CFDictionaryRef vcnvp9_copy_pixel_buffer_attributes(uint32_t w, uint32_t h,
                                                    bool tenBit);

VcnVP9* vcnvp9_create(uint32_t maxWidth, uint32_t maxHeight, char* err,
                      size_t errLen);
void vcnvp9_destroy(VcnVP9* d);
int vcnvp9_decode(VcnVP9* d, const uint8_t* chunk, size_t size, int64_t tag,
                  VcnVP9OutputFn fn, void* ctx);
const char* vcnvp9_last_error(VcnVP9* d);

#endif  // VCNVP9_VCNVP9_H_
