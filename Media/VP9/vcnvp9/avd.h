// Reverse-engineered interface of AMDRadeonVADriver2 ("ATIAVDVADriver"), the
// AMD backend that AppleGVA drives on Intel Macs. Layouts recovered from
// Hex-Rays output and live lldb traces of VTDecoderXPCService decoding HEVC.
// Only the fields we need are named.

#ifndef VCNVP9_AVD_H_
#define VCNVP9_AVD_H_

#include <IOKit/IOKitLib.h>
#include <stdint.h>

#define AVD_DRIVER_PATH                                                  \
  "/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/" \
  "AMDRadeonVADriver2"

// AVD function ids (AVD_FunctionInfo.function / AVD_CmdInfo.function)
#define AVD_FUNC_H264 0x8ull
#define AVD_FUNC_HEVC 0x800ull
#define AVD_FUNC_AV1 0x1000000ull

// Argument of AVD_CreateAVDAccelerator.
typedef struct {
  uint32_t index;  // which entry of services[] to use
  uint32_t pad0;
  uint32_t* services;  // io_service_t array (accelerator service ports)
  uint32_t validMask;  // bit i set => services[i] valid
  uint32_t pad1;
  uint8_t reserved[0x30];
} AVD_AcceleratorCreate;

typedef struct {
  void* inner;
  void* fn[16];
} AVD_Accelerator;

enum {
  AVD_FN_CreateDriver,
  AVD_FN_DeleteDriver,
  AVD_FN_CreateContexts,
  AVD_FN_DeleteContexts,
  AVD_FN_GetBitstreamBufferInfo,
  AVD_FN_AllocateBitstreamBufferPool,
  AVD_FN_FreeBitstreamBufferPool,
  AVD_FN_GetPixelBufferInfo,
  AVD_FN_AllocatePixelBufferPool,
  AVD_FN_FreePixelBufferPool,
  AVD_FN_ScheduleAVDCommands,
  AVD_FN_QueryAVDCommandStatus,
  AVD_FN_SetContextParameter,
  AVD_FN_GetContextParameter,
  AVD_FN_SetDriverParameter,
  AVD_FN_GetDriverParameter,
};

typedef struct {
  uint32_t initFlags;  // passed to VAContext::Init (AppleGVA: 0)
  uint32_t pad;
  AVD_Accelerator* accel;
  uint8_t reserved[0x20];
} AVD_DriverCreate;

typedef struct {
  uint32_t kind;  // 1 = decode
  uint32_t pad0;
  uint64_t function;  // AVD_FUNC_*
  uint32_t mode;      // AppleGVA HEVC: 5
  uint32_t pad1;
  uint32_t pixelRate;  // width*height*fps, must be non-zero
  uint32_t pad2;
  uint32_t width;
  uint32_t height;
  uint32_t inFormat;        // 'rawb' (0x72617762)
  uint32_t inFormatCount;   // 1
  uint32_t outFormat;       // 'NV12' (0x4e563132) or 'P010'
  uint32_t outFormatCount;  // 1
  void* next;
  uint8_t reserved[0x40];
} AVD_FunctionInfo;

typedef struct {
  uint32_t count;
  uint32_t pad;
  AVD_FunctionInfo** functions;
  void** contextsOut;
  AVD_FunctionInfo inlineInfo;
} AVD_ContextList;

typedef struct {
  AVD_ContextList* list;
  void* capsOut;
  uint8_t reserved[0x30];
} AVD_DriverContextInit;

typedef struct {
  uint32_t count;
  uint32_t pad;
  void** contexts;
} AVD_DriverContextDelete;

typedef struct {
  uint32_t type;  // 0
  uint32_t pad0;
  uint32_t size;        // bytes per buffer
  uint32_t format;      // 'rawb'
  uint32_t one;         // 1
  uint32_t count;       // 1..32
  void** addressesOut;  // receives CPU address of each buffer
  void* reserved0;
  uint32_t eight, five;
  uint8_t reserved[0x20];
} AVD_BitstreamPool;

typedef struct {
  uint32_t kind;  // 1 = decode
  uint32_t pad0;
  uint64_t function;  // AVD_FUNC_*
  uint32_t mode;      // 5
  uint32_t pad1;
  void* context;  // from CreateContexts
  void* params;   // codec specific
  void* reserved[6];
} AVD_CmdInfo;

typedef struct {
  uint32_t count;
  uint32_t pad;
  AVD_CmdInfo** cmds;
} AVD_CmdList;

typedef struct {
  uint32_t count;
  uint32_t pad;
  void** refcons;  // out
} AVD_RefConList;

typedef int (*AVD_CreateAVDAccelerator_fn)(AVD_Accelerator** out,
                                           AVD_AcceleratorCreate* in,
                                           void* info96);
typedef int (*AVD_CreateDriver_fn)(void** driver, AVD_DriverCreate* dc);
typedef int (*AVD_CreateContexts_fn)(void* driver, AVD_DriverContextInit* init);
typedef int (*AVD_DeleteContexts_fn)(void* driver,
                                     AVD_DriverContextDelete* del);
typedef int (*AVD_AllocateBitstreamBufferPool_fn)(void* driver,
                                                  AVD_BitstreamPool* pool);
typedef int (*AVD_FreeBitstreamBufferPool_fn)(void* driver, uint32_t type);
typedef int (*AVD_ScheduleAVDCommands_fn)(void* driver, AVD_CmdList* cmds,
                                          AVD_RefConList* refcons);
typedef int (*AVD_QueryAVDCommandStatus_fn)(void* driver, void* query);
typedef int (*AVD_DeleteDriver_fn)(void* driver);

#endif  // VCNVP9_AVD_H_
