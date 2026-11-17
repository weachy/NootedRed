#include "vcnvp9.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "ac_vcn_vp9_default.h"
#include "avd.h"
#include "vp9msg.h"
#include "../../../NootedRed/MediaHardware.h"

int vcnvp9_debug = 0;
int vcnvp9_gpu_armed = 0;
#define DBG(...)                                                \
  do {                                                          \
    if (vcnvp9_debug) fprintf(stderr, "[vcnvp9] " __VA_ARGS__); \
  } while (0)

#define VCN_MODE_VCN0 \
  4  // VP9 runs on both instances, but AV1 contexts only work on VCN0
#define NUM_DPB 9
#define NUM_BS 4
#define BS_SIZE 0x1800000u
#define FRAME_BYTES 4096
#define SEQ_BYTES 2048
#define BSINFO_BYTES (20 + 64 * 512 + 64)
#define VCN_BYPASS_DRM_CNTL 0x1000000u
#define VCN_BYPASS_DRM_CMD 0x80000000u

// Parameter block of the driver's AV1 path. The driver itself only reads
// output, bsinfo and the frame size out of it, the rest goes to the message
// builder, which here is ours.
typedef struct {
  void* seq;              // +0
  void* frame;            // +8   driver reads +312 width, +320 height
  uint32_t curr_pic_idx;  // +16
  uint32_t pad0;
  IOSurfaceRef output;        // +24
  uint32_t ref_frame_map[8];  // +32
  uint32_t frame_refs[7];     // +64
  uint32_t pad1;              // +92
  void* bsinfo;               // +96  driver reads +0 buffer index, +16 size
  const VP9Parser* vp9;       // +104 ours
  uint32_t mif_wrc_en;        // +112 ours: VCN3+, zero on VCN 2.2
  uint32_t dpb_tier;          // +116 ours: explicit tier 1 on VCN 2.2
  uint8_t reserved[144];
} VP9Params;

typedef struct {
  void* refcon;
  uint32_t pollMs;
  uint32_t wait;
  uint32_t status, status2;
} AVD_QueryEntry;
typedef struct {
  uint32_t count, pad;
  AVD_QueryEntry* entries;
} AVD_QueryList;

struct VcnVP9 {
  AVD_Accelerator* accel;
  void* drv;
  void* ctx;
  void* bsAddr[NUM_BS];
  int bsNext;
  uint32_t maxW, maxH;
  VP9Parser parser;
  CVPixelBufferRef dpb[NUM_DPB];
  int dpbRefs[NUM_DPB];
  uint8_t *seq, *frame, *bsinfo;
  char err[256];
  VcnVP9OutputFn out;
  void* outCtx;
  int64_t tag;
  CVPixelBufferPoolRef pool;
  uint32_t poolW, poolH;
  bool poolTen;
  uint32_t deviceID;
  uint32_t mif_wrc_en;
};

#define FN(name) ((AVD_##name##_fn)d->accel->fn[AVD_FN_##name])

// Every offset below was read from the 26.7.2 database, whose decoder code
// matches 26.7.1. The Sequoia driver was compared against it in IDA: same
// vtable slots, object sizes and offsets on everything used here, and its kext
// loads the same VCN firmware image.
static const char* const kValidatedDrivers[] = {
    "5D90FED3BCBF34759F4B8FC00026D3AB",  // macOS 15.8.2 (24H232) and 15.8.3
                                         // (24H241, same binary)
    "8D3BA61E7BC73BDFADB3A7866360B6DE",  // macOS 26.7.1 (25G241)
    "1181E2CA31B33DF090E23AE70552D753",  // macOS 26.7.2 (25G332) and 26.7.3
                                         // (25G342, same binary)
};
static bool driver_uuid(char out[33]) {
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    if (!strstr(_dyld_get_image_name(i), "AMDRadeonVADriver2")) continue;
    const struct mach_header_64* h = (const void*)_dyld_get_image_header(i);
    const struct load_command* lc = (const void*)(h + 1);
    for (uint32_t k = 0; k < h->ncmds;
         k++, lc = (const void*)((const char*)lc + lc->cmdsize))
      if (lc->cmd == LC_UUID) {
        for (int j = 0; j < 16; j++)
          snprintf(out + 2 * j, 3, "%02X",
                   ((const struct uuid_command*)lc)->uuid[j]);
        return true;
      }
  }
  return false;
}

static bool being_traced(void) {
  struct kinfo_proc info;
  size_t size = sizeof(info);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  memset(&info, 0, sizeof(info));
  if (sysctl(mib, 4, &info, &size, NULL, 0)) return false;
  return (info.kp_proc.p_flag & P_TRACED) != 0;
}

// ---------------------------------------------------------------------------------------------------
// Driver object layout (DecodeDataProcessor and friends), from the database.
#define U32(p, off) (*(uint32_t*)((uint8_t*)(p) + (off)))
#define PTR(p, off) (*(uint8_t**)((uint8_t*)(p) + (off)))
#define PROC_DECODER(proc) PTR(proc, 16)
#define PROC_VACONTEXT(proc) PTR(proc, 24)
#define PROC_FEEDBACK(proc) U32(proc, 32)
#define PROC_DEPTH(proc) U32(proc, 64)
#define DEC_DATA(dec) PTR(dec, 88)
#define DEC_WIDTH(dec) U32(dec, 124)
#define DEC_HEIGHT(dec) U32(dec, 128)
#define DATA_REALLOC(data) (*((uint8_t*)(data) + 12))
#define DATA_TUPLE(data) ((uint8_t*)(data) + 16)
#define DATA_DPB_SIZE(data) U32(data, 48)
#define DATA_DPB_TEX(data) PTR(data, 56)
#define DATA_MSG_TEX(data) PTR(data, 8 * (uint64_t)U32(data, 216) + 152)
#define DATA_MSG(data) PTR(data, 224)
#define DATA_CTX_SIZE(data) U32(data, 456)
#define DATA_CTX_TEX(data) PTR(data, 464)
#define DATA_SESSION_TEX(data) PTR(data, 480)
#define DATA_IT_TEX(data) PTR(data, 8 * (uint64_t)U32(data, 624) + 496)
#define DATA_IT(data) PTR(data, 632)
#define DATA_BS_SIZE(data) U32(data, 688)
#define TEX_CPU(tex) PTR(tex, 152)
#define TEX_HEIGHT(tex) U32(tex, 124)
#define TEX_PITCH(tex) U32(tex, 128)
#define TEX_BPE(tex) U32(tex, 132)
#define HWCONFIG(ctx) PTR(ctx, 1120)
#define BI_MSG_ON 4
#define BI_MSG 8
#define BI_DPB_ON 16
#define BI_DPB 24
#define BI_TARGET_ON 112
#define BI_TARGET 120
#define BI_TARGET_OFF 128
#define BI_IT_ON 144
#define BI_IT 152
#define BI_CTX_ON 160
#define BI_CTX 168
#define BI_SESSION_ON 176
#define BI_SESSION 184
// Virtual slots of AvdVcnAv1DecDataProcessor, in pointers.
enum {
  VT_CREATE_SESSION = 2,
  VT_DPB_SIZE = 8,
  VT_CTX_SIZE = 9,
  VT_INIT_CTX = 12,
  VT_SET_MESSAGE = 14,
  VT_SLOTS = 16
};
#define FACTORY_CREATE_PROCESSOR 4  // VAFactory::createDecodeDataProcessor
#define FACTORY_SLOTS 32
#define AV1_PROCESSOR_TYPE 6

static uint32_t al(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Mesa's alignment for VP9 on VCN 2 and later.
static uint32_t dpb_alignment(uint32_t width) { return width <= 32 ? 32 : 64; }

static int g_session_is_vp9;  // process wide on purpose: only the dry run ever
                              // sets it to 0
static void* g_vt[VT_SLOTS + 2];
static void** g_orig;
static __thread int t_creating;

// The dry run script breaks here. It checks that its submission trap is armed
// before anything can go out.
__attribute__((noinline)) void vcnvp9_dryrun_checkpoint(int phase) {
  __asm__ volatile("" ::"r"(phase) : "memory");
}
// And here, to read the create message before it is submitted.
__attribute__((noinline)) void vcnvp9_session_message(const void* msg) {
  __asm__ volatile("" ::"r"(msg) : "memory");
}
// And here, to find the decode message that hook_set_message just built.
__attribute__((noinline)) void vcnvp9_message_built(const void* data) {
  __asm__ volatile("" ::"r"(data) : "memory");
}

static int hook_create_session(uint8_t* proc, void* bi) {
  int r = ((int (*)(void*, void*))g_orig[VT_CREATE_SESSION])(proc, bi);
  uint8_t* msg = DATA_MSG(DEC_DATA(PROC_DECODER(proc)));
  if (!r && g_session_is_vp9) U32(msg, 40) = VCN_CODEC_VP9;
  vcnvp9_session_message(msg);
  return r;
}

// Reserve the full 8K rectangle for nine pictures, including aligned height.
// Renoir describes each reference plane with the tier-1 message below; simply
// growing this allocation without that descriptor does not fix tier-0 chroma.
// Its fixed layout preserves references across coded-resolution changes.
static uint32_t hook_dpb_size(uint8_t* proc) {
  uint32_t size = VCN_VP9_MAX_PIXELS * 3 / 2 * NUM_DPB;
  if (PROC_DEPTH(proc) == 10) size = size * 3 / 2;
  return size;
}
// Mesa's calc_ctx_size_vp9 for VCN 2 and later.
static uint32_t hook_ctx_size(uint8_t* proc) {
  uint32_t size =
      2304 * 5 + 32 * 2 * 128 * 68 + 9 * 64 * 2 * 128 * 68 + 8 * 2 * 2 * 8192;
  if (PROC_DEPTH(proc) == 10) size += 8 * 2 * 4096;
  return size;
}

// Mesa's vcn_init_session_buf: the first 2304 bytes of the context are the
// default probabilities, and the firmware derives the four frame contexts from
// them.
static void hook_init_ctx(uint8_t* proc) {
  uint8_t* data = DEC_DATA(PROC_DECODER(proc));
  uint8_t* ctx = TEX_CPU(DATA_CTX_TEX(data));
  memset(ctx, 0, DATA_CTX_SIZE(data));
  VcnVP9Probs* p = (VcnVP9Probs*)ctx;
  memcpy(p->coef_probs[0], default_coef_probs_4x4,
         sizeof(default_coef_probs_4x4));
  memcpy(p->coef_probs[1], default_coef_probs_8x8,
         sizeof(default_coef_probs_8x8));
  memcpy(p->coef_probs[2], default_coef_probs_16x16,
         sizeof(default_coef_probs_16x16));
  memcpy(p->coef_probs[3], default_coef_probs_32x32,
         sizeof(default_coef_probs_32x32));
  memcpy(p->y_mode_prob, default_if_y_probs, sizeof(default_if_y_probs));
  memcpy(p->uv_mode_prob, default_if_uv_probs, sizeof(default_if_uv_probs));
  memcpy(p->single_ref_prob, default_single_ref_p,
         sizeof(default_single_ref_p));
  memcpy(p->switchable_interp_prob, default_switchable_interp_prob,
         sizeof(default_switchable_interp_prob));
  memcpy(p->partition_prob, default_partition_probs,
         sizeof(default_partition_probs));
  memcpy(p->inter_mode_probs, default_inter_mode_probs,
         sizeof(default_inter_mode_probs));
  memcpy(p->mbskip_probs, default_skip_probs, sizeof(default_skip_probs));
  memcpy(p->intra_inter_prob, default_intra_inter_p,
         sizeof(default_intra_inter_p));
  memcpy(p->comp_inter_prob, default_comp_inter_p,
         sizeof(default_comp_inter_p));
  memcpy(p->comp_ref_prob, default_comp_ref_p, sizeof(default_comp_ref_p));
  memcpy(p->tx_probs_32x32, default_tx_probs_32x32,
         sizeof(default_tx_probs_32x32));
  memcpy(p->tx_probs_16x16, default_tx_probs_16x16,
         sizeof(default_tx_probs_16x16));
  memcpy(p->tx_probs_8x8, default_tx_probs_8x8, sizeof(default_tx_probs_8x8));
  memcpy(p->mv_joints, default_nmv_joints, sizeof(default_nmv_joints));
  memcpy(p->mv_comps, default_nmv_components, sizeof(default_nmv_components));
}

static void fill_vp9_message(VcnMsgVP9* m, const VP9Params* prm) {
  const VP9Parser* p = prm->vp9;
  const VP9Frame* h = &p->fh;
  memset(m, 0, sizeof(*m));
  uint32_t fl = 0;
  if (h->frame_type) fl |= VP9_HDR_FRAME_TYPE;
  if (h->error_res) fl |= VP9_HDR_ERROR_RES;
  if (h->intra_only) fl |= VP9_HDR_INTRA_ONLY;
  if (h->allow_high_precision_mv) fl |= VP9_HDR_ALLOW_HP;
  if (h->refresh_frame_context) fl |= VP9_HDR_REFRESH_CTX;
  if (h->frame_parallel) fl |= VP9_HDR_PARALLEL;
  if (h->seg_enabled) fl |= VP9_HDR_SEG_ENABLED;
  if (h->seg_update_map) fl |= VP9_HDR_SEG_UPDATE_MAP;
  if (h->seg_temporal_update) fl |= VP9_HDR_SEG_TEMPORAL;
  if (h->seg_update_data) fl |= VP9_HDR_SEG_UPDATE_DATA;
  if (h->lf_delta_enabled) fl |= VP9_HDR_LF_DELTA_ENABLED;
  if (h->lf_delta_update) fl |= VP9_HDR_LF_DELTA_UPDATE;
  if (h->use_prev_frame_mvs) fl |= VP9_HDR_USE_PREV_MVS;
  fl |= VP9_HDR_USE_UNCOMPRESSED;  // what Mesa's VA path sends: whole frame in
                                   // the bitstream buffer
  m->frame_header_flags = fl;
  m->frame_context_idx = (uint8_t)h->frame_context_idx;
  m->reset_frame_context = (uint8_t)h->reset_frame_context;
  m->curr_pic_idx = (uint8_t)prm->curr_pic_idx;
  m->interp_filter = (uint8_t)h->interp_filter;
  m->filter_level = (uint8_t)h->lf_level;
  m->sharpness_level = (uint8_t)h->lf_sharpness;
  vp9_lf_adj_levels(p, m->lf_adj_level);
  m->base_qindex = (uint8_t)h->base_q_idx;
  m->y_dc_delta_q = (int8_t)h->y_dc_delta_q;
  m->uv_ac_delta_q = (int8_t)h->uv_ac_delta_q;
  m->uv_dc_delta_q = (int8_t)h->uv_dc_delta_q;
  m->log2_tile_cols = (uint8_t)h->log2_tile_cols;
  m->log2_tile_rows = (uint8_t)h->log2_tile_rows;
  m->chroma_format = 1;
  m->bit_depth_luma_minus8 = m->bit_depth_chroma_minus8 =
      (uint8_t)(h->bit_depth - 8);
  if (h->bit_depth > 8) m->p010_mode = m->msb_mode = 1;
  m->compressed_header_size = h->compressed_header_size;
  m->uncompressed_header_size = h->uncompressed_header_size;
  for (int i = 0; i < 8; i++)
    m->ref_frame_map[i] = (uint8_t)prm->ref_frame_map[i];
  for (int i = 0; i < 3; i++) {
    m->frame_refs[i] = (uint8_t)prm->frame_refs[i];
    m->ref_frame_sign_bias[i] = (uint8_t)h->sign_bias[i];
  }
}

static void fill_segments(uint8_t* it, const VP9Parser* p) {
  memset(it, 0, VCN_VP9_PROBS_DATA_SIZE + 256);
  if (!p->fh.seg_enabled) return;
  VcnVP9Seg* s = (VcnVP9Seg*)(it + VCN_VP9_PROBS_DATA_SIZE);
  for (int i = 0; i < 8; i++) {
    const int16_t* f = p->seg_feature_data[i];
    s->feature_data[i] =
        ((uint32_t)f[0] & 0xffff) | (((uint32_t)f[1] & 0xff) << 16) |
        (((uint32_t)f[2] & 0xf) << 24) | (((uint32_t)f[3] & 0xf) << 28);
    s->feature_mask[i] = p->seg_feature_mask[i];
  }
  memcpy(s->tree_probs, p->seg_tree_probs, 7);
  memcpy(s->pred_probs, p->seg_pred_probs, 3);
  s->abs_delta = (uint8_t)p->seg_abs_delta;
}

static void fill_dynamic_dpb(VcnMsgDynamicDPB* m, bool ten_bit) {
  memset(m, 0, sizeof(*m));
  m->array_size = NUM_DPB;
  m->luma_pitch = VCN_VP9_MAX_WIDTH;
  m->luma_height = VCN_VP9_MAX_HEIGHT;
  m->luma_size = m->luma_pitch * m->luma_height;
  m->chroma_pitch = m->luma_pitch / 2;
  m->chroma_height = m->luma_height / 2;
  m->chroma_size = m->chroma_pitch * m->chroma_height * 2;
  m->reserved[0] = dpb_alignment(VCN_VP9_MAX_WIDTH);
  if (ten_bit) {
    m->luma_size = m->luma_size * 3 / 2;
    m->chroma_size = m->chroma_size * 3 / 2;
  }
}

// Replaces AvdVcnAv1DecDataProcessor::SetDecodeMessageFields. Same layout the
// driver sends for AV1 (four index entries, the decode message filled the same
// way) with VP9's codec message in AV1's place.
static int hook_set_message(uint8_t* proc, const VP9Params* prm,
                            uint8_t* target, uint8_t* bi, uint32_t a4,
                            uint32_t a5) {
  (void)a4;
  (void)a5;
  const VP9Frame* h = &prm->vp9->fh;
  uint8_t* dec = PROC_DECODER(proc);
  uint8_t* data = DEC_DATA(dec);
  uint8_t* hw = HWCONFIG(PROC_VACONTEXT(proc));
  DATA_REALLOC(data) = 0;
  PROC_DEPTH(proc) = (uint32_t)h->bit_depth;
  uint32_t tuple[6] = {(uint32_t)h->profile,   0,
                       DEC_WIDTH(dec),         DEC_HEIGHT(dec),
                       (uint32_t)h->bit_depth, 0};
  if (memcmp(tuple, DATA_TUPLE(data), sizeof(tuple))) {
    uint32_t dpb = hook_dpb_size(proc), ctx = hook_ctx_size(proc);
    if (dpb > DATA_DPB_SIZE(data) || ctx > DATA_CTX_SIZE(data)) {
      DATA_REALLOC(data) = 1;
      DATA_DPB_SIZE(data) = dpb;
      DATA_CTX_SIZE(data) = ctx;
    }
    memcpy(DATA_TUPLE(data), tuple, sizeof(tuple));
  }
  uint8_t* msg = DATA_MSG(data);
  const bool tier1 = prm->dpb_tier == 1;
  const uint32_t count = tier1 ? 5 : 4;
  const uint32_t decode_off = 24 + count * 16, codec_off = decode_off + 188,
                 codec_size = sizeof(VcnMsgVP9);
  const uint32_t drm_off = codec_off + al(codec_size, 4),
                 zero_off = drm_off + 68, dynamic_off = zero_off + 168,
                 end = dynamic_off + (tier1 ? sizeof(VcnMsgDynamicDPB) : 0);
  memset(msg, 0, end + 16);
  U32(msg, 0) = 40;
  U32(msg, 4) =
      end + 16;  // the driver's AV1 total also has these 16 extra bytes
  U32(msg, 8) = count;
  U32(msg, 12) = 1;
  U32(msg, 16) = (uint32_t)(uintptr_t)data;
  U32(msg, 20) = PROC_FEEDBACK(proc);
  const uint32_t index[5][3] = {{VCN_MESSAGE_DECODE, decode_off, 188},
                                {VCN_MESSAGE_VP9, codec_off, codec_size},
                                {VCN_MESSAGE_DRM, drm_off, 68},
                                {VCN_MESSAGE_0F, zero_off, 168},
                                {VCN_MESSAGE_DYNAMIC_DPB, dynamic_off,
                                 sizeof(VcnMsgDynamicDPB)}};
  for (uint32_t i = 0; i < count; i++)
    for (int j = 0; j < 3; j++) U32(msg, 24 + 16 * i + 4 * j) = index[i][j];

  uint8_t* m = msg + decode_off;
  uint32_t pitch = TEX_PITCH(target), bpe = TEX_BPE(target),
           texh = TEX_HEIGHT(target);
  uint32_t w = DEC_WIDTH(dec), hh = DEC_HEIGHT(dec), a = dpb_alignment(w);
  U32(m, 0) = VCN_CODEC_VP9;
  U32(m, 8) = w;
  U32(m, 12) = hh;
  U32(m, 16) = DATA_BS_SIZE(data);
  U32(m, 20) = DATA_DPB_SIZE(data);
  U32(m, 24) = (3 * pitch * texh) >> 1;
  U32(m, 36) = DATA_CTX_SIZE(data);
  U32(m, 72) = al(w, a);
  U32(m, 76) = al(hh, 64);
  U32(m, 100) = pitch / bpe;
  U32(m, 104) = (pitch / bpe) >> 1;
  U32(m, 140) = pitch * hh - 1;
  U32(m, 144) = pitch * al(texh, 16);
  U32(m, 148) = pitch * al(texh, 16) + ((pitch * hh) >> 1) - 1;
  // Mesa enables memory-interface write combining only on VCN3 and later.
  // Renoir accepts the VP9 protocol but must keep this VCN3 flag clear.
  U32(m, 160) = prm->mif_wrc_en;
  U32(m, 164) = al(w / 2, a);
  if (tier1) {
    U32(m, 4) = VCN_DECODE_USE_DYNAMIC_DPB | VCN_DECODE_USE_PAL;
    fill_dynamic_dpb((VcnMsgDynamicDPB*)(msg + dynamic_off), h->bit_depth > 8);
  }

  // SetupVcnDRMCommand with no protected content.
  if (U32(hw, 144) & 0x80000)
    U32(msg + drm_off, 56) = VCN_BYPASS_DRM_CNTL;
  else
    U32(msg + drm_off, 52) = VCN_BYPASS_DRM_CMD;

  fill_vp9_message((VcnMsgVP9*)(msg + codec_off), prm);
  fill_segments(DATA_IT(data), prm->vp9);

  U32(bi, BI_MSG_ON) = 1;
  *(uint8_t**)(bi + BI_MSG) = DATA_MSG_TEX(data);
  U32(bi, BI_DPB_ON) = 1;
  *(uint8_t**)(bi + BI_DPB) = DATA_DPB_TEX(data);
  U32(bi, BI_TARGET_ON) = 1;
  *(uint8_t**)(bi + BI_TARGET) = target;
  U32(bi, BI_TARGET_OFF) = 0;
  U32(bi, BI_IT_ON) = 1;
  *(uint8_t**)(bi + BI_IT) = DATA_IT_TEX(data);
  U32(bi, BI_CTX_ON) = 1;
  *(uint8_t**)(bi + BI_CTX) = DATA_CTX_TEX(data);
  if (*((int8_t*)hw + 149) < 0) {
    U32(bi, BI_SESSION_ON) = 1;
    *(uint8_t**)(bi + BI_SESSION) = DATA_SESSION_TEX(data);
  }
  DBG("message: hw flags %08x session_flag=%d session_tex=%p dpb=%u ctx=%u\n",
      U32(hw, 148), *((int8_t*)hw + 149) < 0, DATA_SESSION_TEX(data),
      DATA_DPB_SIZE(data), DATA_CTX_SIZE(data));
  vcnvp9_message_built(data);
  return 0;
}

static void** g_class_vt;
static void init_vt(void) {
  g_orig = g_class_vt;
  memcpy(g_vt, g_class_vt - 2, sizeof(g_vt));
  void** vt = &g_vt[2];
  vt[VT_CREATE_SESSION] = (void*)hook_create_session;
  vt[VT_DPB_SIZE] = (void*)hook_dpb_size;
  vt[VT_CTX_SIZE] = (void*)hook_ctx_size;
  vt[VT_INIT_CTX] = (void*)hook_init_ctx;
  vt[VT_SET_MESSAGE] = (void*)hook_set_message;
}
// Wraps the factory's createDecodeDataProcessor: the driver creates the
// processor and submits the create message inside one call, so this is the only
// moment to swap the vtable pointer on that one object. Other threads may pass
// through here while the factory is wrapped, and they get the plain object.
static void* (*g_orig_create_processor)(void*, int, void*);
static void* hook_create_processor(void* factory, int type, void* decoder) {
  void** proc = g_orig_create_processor(factory, type, decoder);
  if (proc && type == AV1_PROCESSOR_TYPE && t_creating) {
    void** vt = *(void***)proc;
    if (!g_class_vt) g_class_vt = vt;
    if (vt != g_class_vt)
      abort();  // something else where AV1's class should be: never continue
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_vt);
    *proc = (void*)&g_vt[2];
  }
  return proc;
}
// ---------------------------------------------------------------------------------------------------

static uint32_t pci_property(io_service_t service, CFStringRef key) {
  CFTypeRef value = IORegistryEntrySearchCFProperty(
      service, kIOServicePlane, key, NULL,
      kIORegistryIterateParents | kIORegistryIterateRecursively);
  uint32_t result = 0;
  if (value && CFGetTypeID(value) == CFDataGetTypeID() &&
      CFDataGetLength(value) == sizeof(result))
    memcpy(&result, CFDataGetBytePtr(value), sizeof(result));
  if (value) CFRelease(value);
  return result;
}

static io_service_t find_accelerator(uint32_t* deviceID) {
  io_iterator_t it;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOAccelerator"), &it))
    return 0;
  io_service_t s, found = 0;
  while ((s = IOIteratorNext(it))) {
    CFTypeRef codec =
        IORegistryEntryCreateCFProperty(s, CFSTR("IOGVACodec"), NULL, 0);
    if (codec && pci_property(s, CFSTR("vendor-id")) == 0x1002) {
      CFRelease(codec);
      *deviceID = pci_property(s, CFSTR("device-id"));
      found = s;
      break;
    }
    if (codec) CFRelease(codec);
    IOObjectRelease(s);
  }
  IOObjectRelease(it);
  return found;
}

static CFNumberRef cfnum(long long v) {
  return CFNumberCreate(NULL, kCFNumberLongLongType, &v);
}
CFDictionaryRef vcnvp9_copy_pixel_buffer_attributes(uint32_t w, uint32_t h,
                                                    bool ten) {
  CFMutableDictionaryRef a =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  CFMutableDictionaryRef io =
      CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
  CFNumberRef n;
#define PUT(k, v) (n = cfnum(v), CFDictionarySetValue(a, k, n), CFRelease(n))
  PUT(kCVPixelBufferPixelFormatTypeKey,
      ten ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
          : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  PUT(kCVPixelBufferWidthKey, w);
  PUT(kCVPixelBufferHeightKey, h);
  PUT(kCVPixelBufferBytesPerRowAlignmentKey, 256);
  PUT(kCVPixelBufferPlaneAlignmentKey, 4096);
  PUT(kCVPixelBufferExtendedPixelsBottomKey, al(h, 64) - h);
#undef PUT
  CFDictionarySetValue(a, kCVPixelBufferIOSurfacePropertiesKey, io);
  CFDictionarySetValue(a, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
  CFRelease(io);
  return a;
}

static CVPixelBufferRef new_target(VcnVP9* d, uint32_t w, uint32_t h,
                                   bool ten) {
  if (d->pool && (d->poolW != w || d->poolH != h || d->poolTen != ten)) {
    CVPixelBufferPoolRelease(d->pool);
    d->pool = NULL;
  }
  if (!d->pool) {
    CFDictionaryRef a = vcnvp9_copy_pixel_buffer_attributes(w, h, ten);
    CVReturn r = CVPixelBufferPoolCreate(NULL, NULL, a, &d->pool);
    CFRelease(a);
    if (r) {
      d->pool = NULL;
      return NULL;
    }
    d->poolW = w;
    d->poolH = h;
    d->poolTen = ten;
  }
  CVPixelBufferRef pb = NULL;
  if (CVPixelBufferPoolCreatePixelBuffer(NULL, d->pool, &pb)) return NULL;
  return pb;
}

VcnVP9* vcnvp9_create(uint32_t maxWidth, uint32_t maxHeight, char* err,
                      size_t errLen) {
  if (!vcnvp9_bounds_supported(maxWidth, maxHeight)) {
    if (err && errLen)
      snprintf(err, errLen, "invalid VP9 session maximum %ux%u", maxWidth,
               maxHeight);
    return NULL;
  }
#define FAIL(...)                                          \
  do {                                                     \
    if (err && errLen) snprintf(err, errLen, __VA_ARGS__); \
    vcnvp9_destroy(d);                                     \
    return NULL;                                           \
  } while (0)
  VcnVP9* d = calloc(1, sizeof(*d));
  if (!d) {
    if (err && errLen) snprintf(err, errLen, "out of memory");
    return NULL;
  }
  d->maxW = maxWidth;
  d->maxH = maxHeight;
  vp9_parser_init(&d->parser);
  d->seq = calloc(1, SEQ_BYTES);
  d->frame = calloc(1, FRAME_BYTES);
  d->bsinfo = calloc(1, BSINFO_BYTES);
  if (!d->seq || !d->frame || !d->bsinfo) FAIL("out of memory");
  // No path to the GPU unless asked for: either under the dry run's debugger,
  // which stops the submission, or with the explicit switch. VCNVP9_SESSION=av1
  // pairs an AV1 session with VP9 messages, so it must never run without the
  // debugger.
  const char* sess = vcnvp9_gpu_armed ? "vp9" : getenv("VCNVP9_SESSION");
  const char* arm = getenv("VCNVP9_ALLOW_GPU");
  bool traced = being_traced(),
       armed =
           vcnvp9_gpu_armed || (arm && !strcmp(arm, "yes-i-know-it-can-hang"));
  if (!sess || (strcmp(sess, "vp9") && strcmp(sess, "av1")))
    FAIL("set VCNVP9_SESSION=vp9 or av1");
  if (!traced && !armed)
    FAIL("GPU submission disabled (set VCNVP9_ALLOW_GPU or use the dry run)");
  if (!strcmp(sess, "av1") && !traced)
    FAIL("VCNVP9_SESSION=av1 is only for the dry run");
  g_session_is_vp9 = !strcmp(sess, "vp9");
  void* lib = dlopen(AVD_DRIVER_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!lib) FAIL("dlopen driver: %s", dlerror());
  char uuid[33] = "unknown";
  bool validated = false;
  driver_uuid(uuid);
  for (size_t i = 0; i < sizeof(kValidatedDrivers) / sizeof(*kValidatedDrivers);
       i++)
    if (!strcmp(uuid, kValidatedDrivers[i])) validated = true;
  if (!validated)
    FAIL("AMDRadeonVADriver2 build %s not validated for VP9", uuid);
  AVD_CreateAVDAccelerator_fn create =
      (AVD_CreateAVDAccelerator_fn)dlsym(lib, "AVD_CreateAVDAccelerator");
  if (!create) FAIL("no AVD_CreateAVDAccelerator");
  io_service_t svc = find_accelerator(&d->deviceID);
  if (!svc) FAIL("no AMD accelerator");
  // Classify the real PCI GPU by media IP. Same-IP APUs use the same protocol,
  // while different generations retain their own layout and binary guards.
  // The AV1-shaped CPU-side carrier does not imply AV1 hardware support.
  if (!nredMediaVP9Supported(d->deviceID)) {
    IOObjectRelease(svc);
    FAIL("GPU %04x has no supported VP9 backend", d->deviceID);
  }
  if (nredMediaUsesVCN2Bridge(d->deviceID) &&
      strcmp(uuid, "1181E2CA31B33DF090E23AE70552D753")) {
    IOObjectRelease(svc);
    FAIL("VCN 2.2 requires the checked 25G332 driver");
  }
  d->mif_wrc_en = nredMediaVP9WriteCombine(d->deviceID);
  uint32_t services[8] = {svc};
  AVD_AcceleratorCreate ac = {
      .index = 0, .services = services, .validMask = 0xffffffff};
  uint8_t info[256] = {0};
  int r = create(&d->accel, &ac, info);
  IOObjectRelease(svc);
  if (r || !d->accel) FAIL("CreateAVDAccelerator %d", r);
  AVD_DriverCreate dc = {.initFlags = 0, .accel = d->accel};
  r = FN(CreateDriver)(&d->drv, &dc);
  if (r) FAIL("CreateDriver %d", r);
  AVD_BitstreamPool pool = {.type = 0,
                            .size = BS_SIZE,
                            .format = 0x72617762,
                            .one = 1,
                            .count = NUM_BS,
                            .addressesOut = d->bsAddr,
                            .eight = 8,
                            .five = 5};
  r = FN(AllocateBitstreamBufferPool)(d->drv, &pool);
  if (r || !d->bsAddr[0]) FAIL("AllocateBitstreamBufferPool %d", r);
  AVD_ContextList list = {0};
  void* ctxOut[2] = {0};
  AVD_FunctionInfo* fis[1] = {&list.inlineInfo};
  list.count = 1;
  list.functions = fis;
  list.contextsOut = ctxOut;
  list.inlineInfo = (AVD_FunctionInfo){.kind = 1,
                                       .function = AVD_FUNC_AV1,
                                       .mode = VCN_MODE_VCN0,
                                       .pixelRate = maxWidth * maxHeight * 60,
                                       .width = maxWidth,
                                       .height = maxHeight,
                                       .inFormat = 0x72617762,
                                       .inFormatCount = 1,
                                       .outFormat = 0x4e563132,
                                       .outFormatCount = 1};
  AVD_DriverContextInit ci = {.list = &list};
  // Swap the vtable pointer on this driver's factory, only for the length of
  // CreateContexts.
  static pthread_mutex_t create_lock = PTHREAD_MUTEX_INITIALIZER;
  uint8_t* va = PTR(d->drv, 8);
  void*** factory = (void***)PTR(va, 75072);
  if (!factory || !*factory) FAIL("no factory");
  vcnvp9_dryrun_checkpoint(1);
  pthread_mutex_lock(&create_lock);
  void** origFactoryVt = *factory;
  static void* factoryVt[FACTORY_SLOTS + 2];
  memcpy(factoryVt, origFactoryVt - 2, sizeof(factoryVt));
  g_orig_create_processor =
      (void* (*)(void*, int, void*))origFactoryVt[FACTORY_CREATE_PROCESSOR];
  factoryVt[2 + FACTORY_CREATE_PROCESSOR] = (void*)hook_create_processor;
  *factory = &factoryVt[2];
  t_creating = 1;
  r = FN(CreateContexts)(d->drv, &ci);
  t_creating = 0;
  *factory = origFactoryVt;
  pthread_mutex_unlock(&create_lock);
  if (r || !ctxOut[0]) FAIL("CreateContexts %d", r);
  d->ctx = ctxOut[0];
  DBG("created: driver %s session=%s drv=%p ctx=%p\n", uuid, sess, d->drv,
      d->ctx);
  return d;
}

void vcnvp9_destroy(VcnVP9* d) {
  if (!d) return;
  if (d->ctx && d->drv && d->accel) {
    void* ctxs[1] = {d->ctx};
    AVD_DriverContextDelete del = {.count = 1, .contexts = ctxs};
    FN(DeleteContexts)(d->drv, &del);
  }
  if (d->drv && d->accel) {
    FN(FreeBitstreamBufferPool)(d->drv, 0);
    FN(DeleteDriver)(d->drv);
  }
  for (int i = 0; i < NUM_DPB; i++)
    if (d->dpb[i]) CVPixelBufferRelease(d->dpb[i]);
  if (d->pool) CVPixelBufferPoolRelease(d->pool);
  free(d->seq);
  free(d->frame);
  free(d->bsinfo);
  free(d);
}

const char* vcnvp9_last_error(VcnVP9* d) { return d ? d->err : ""; }

static int alloc_dpb_slot(VcnVP9* d) {
  for (int i = 0; i < NUM_DPB; i++)
    if (d->dpbRefs[i] == 0) return i;
  return -1;
}
static void recount_refs(VcnVP9* d) {
  memset(d->dpbRefs, 0, sizeof(d->dpbRefs));
  for (int i = 0; i < 8; i++) {
    const VP9Ref* r = &d->parser.ref[i];
    if (r->valid && r->dpb_slot >= 0) d->dpbRefs[r->dpb_slot]++;
  }
}

static int submit(VcnVP9* d, VP9Params* prm) {
  vcnvp9_dryrun_checkpoint(2);
  AVD_CmdInfo cmd = {.kind = 1,
                     .function = AVD_FUNC_AV1,
                     .mode = VCN_MODE_VCN0,
                     .context = d->ctx,
                     .params = prm};
  AVD_CmdInfo* cmds[1] = {&cmd};
  AVD_CmdList list = {.count = 1, .cmds = cmds};
  void* refcons[1] = {0};
  AVD_RefConList rl = {.count = 1, .refcons = refcons};
  int r = FN(ScheduleAVDCommands)(d->drv, &list, &rl);
  if (r) {
    snprintf(d->err, sizeof(d->err), "ScheduleAVDCommands %d", r);
    return -1;
  }
  AVD_QueryEntry qe = {.refcon = refcons[0], .pollMs = 1, .wait = 1};
  AVD_QueryList ql = {.count = 1, .entries = &qe};
  r = FN(QueryAVDCommandStatus)(d->drv, &ql);
  if (r) {
    snprintf(d->err, sizeof(d->err), "QueryAVDCommandStatus %d", r);
    return -1;
  }
  return 0;
}

static int on_frame(void* vctx, VP9Parser* p, VP9ParseResult kind,
                    const uint8_t* data, size_t size) {
  VcnVP9* d = vctx;
  const VP9Frame* h = &p->fh;
  if (kind == VP9_PARSE_SHOW_EXISTING) {
    const VP9Ref* r = &p->ref[h->frame_to_show];
    if (!r->valid || r->dpb_slot < 0 || !d->dpb[r->dpb_slot]) {
      snprintf(d->err, sizeof(d->err), "show_existing of a missing reference");
      return -1;
    }
    if (d->out)
      d->out(d->outCtx, d->dpb[r->dpb_slot], p, r->width, r->height, d->tag);
    return 0;
  }
  if (!vp9_frame_supported(p)) {
    snprintf(d->err, sizeof(d->err), "profile %d depth %d not supported",
             h->profile, h->bit_depth);
    return -1;
  }
  if (h->frame_type && !h->intra_only)
    for (int i = 0; i < 3; i++)
      if (!p->ref[h->ref_frame_idx[i]].valid) {
        snprintf(d->err, sizeof(d->err), "missing reference");
        return -1;
      }
  if (!vcnvp9_dimensions_supported(h->width, h->height) ||
      (nredMediaVP9DPBTier(d->deviceID) == 0 &&
       (uint64_t)h->width * h->height > VCN_VP9_TIER0_MAX_PIXELS) ||
      h->width > d->maxW || h->height > d->maxH) {
    snprintf(d->err, sizeof(d->err), "%ux%u is over the session maximum",
             h->width, h->height);
    return -1;
  }
  if (size > BS_SIZE - 256) {
    snprintf(d->err, sizeof(d->err), "frame too big");
    return -1;
  }
  int slot = alloc_dpb_slot(d);
  if (slot < 0) {
    snprintf(d->err, sizeof(d->err), "no free DPB slot");
    return -1;
  }
  CVPixelBufferRef target =
      new_target(d, h->width, h->height, h->bit_depth > 8);
  if (!target) {
    snprintf(d->err, sizeof(d->err), "no pixel buffer for %ux%u", h->width,
             h->height);
    return -1;
  }
  if (d->dpb[slot]) CVPixelBufferRelease(d->dpb[slot]);
  d->dpb[slot] = target;
  int bi = d->bsNext;
  d->bsNext = (d->bsNext + 1) % NUM_BS;
  memcpy(d->bsAddr[bi], data, size);
  memset((uint8_t*)d->bsAddr[bi] + size, 0, 256);
  memset(d->bsinfo, 0, BSINFO_BYTES);
  *(uint32_t*)(d->bsinfo + 0) = (uint32_t)bi;
  *(uint32_t*)(d->bsinfo + 16) = al((uint32_t)size, 128);
  memset(d->frame, 0, FRAME_BYTES);
  // What the driver's own code reads from the AV1 frame block.
  *(uint32_t*)(d->frame + 308) = h->width - 1;
  *(uint32_t*)(d->frame + 312) = h->width;
  *(uint32_t*)(d->frame + 316) = h->height - 1;
  *(uint32_t*)(d->frame + 320) = h->height;
  VP9Params prm;
  memset(&prm, 0, sizeof(prm));
  prm.seq = d->seq;
  prm.frame = d->frame;
  prm.curr_pic_idx = (uint32_t)slot;
  prm.output = CVPixelBufferGetIOSurface(target);
  prm.bsinfo = d->bsinfo;
  prm.vp9 = p;
  prm.mif_wrc_en = d->mif_wrc_en;
  prm.dpb_tier = nredMediaVP9DPBTier(d->deviceID);
  for (int i = 0; i < 8; i++)
    prm.ref_frame_map[i] = p->ref[i].valid && p->ref[i].dpb_slot >= 0
                               ? (uint32_t)p->ref[i].dpb_slot
                               : 0x7f;
  bool intra = h->frame_type == 0 || h->intra_only;
  for (int i = 0; i < 7; i++) prm.frame_refs[i] = 0x7f;
  if (!intra)
    for (int i = 0; i < 3; i++)
      prm.frame_refs[i] = (uint32_t)p->ref[h->ref_frame_idx[i]].dpb_slot;
  DBG("frame type=%d intra=%d show=%d %ux%u depth=%d q=%d bytes=%zu slot=%d "
      "refresh=%02x\n",
      h->frame_type, h->intra_only, h->show_frame, h->width, h->height,
      h->bit_depth, h->base_q_idx, size, slot, h->refresh_frame_flags);
  if (submit(d, &prm)) return -1;
  vp9_frame_done(p, slot);
  recount_refs(d);
  if (h->show_frame && d->out)
    d->out(d->outCtx, target, p, h->width, h->height, d->tag);
  return 0;
}

int vcnvp9_decode(VcnVP9* d, const uint8_t* chunk, size_t size, int64_t tag,
                  VcnVP9OutputFn fn, void* ctx) {
  d->out = fn;
  d->outCtx = ctx;
  d->tag = tag;
  d->err[0] = 0;
  int r = vp9_parse_chunk(&d->parser, chunk, size, on_frame, d);
  if (r && !d->err[0])
    snprintf(d->err, sizeof(d->err), "%s",
             d->parser.err[0] ? d->parser.err : "bitstream error");
  return r;
}

// Used by the dry run script: copy memory to a file from inside the process.
__attribute__((noinline, used)) int vcnvp9_dump(const void* p, long n,
                                                const char* path) {
  // Dry-run only: refuse odd sizes and path characters that break C exprs.
  if (!p || !path || n <= 0 || n > 64 * 1024 * 1024) return -1;
  for (const unsigned char* c = (const unsigned char*)path; *c; c++) {
    if (*c < 0x20 || *c == 0x7f) return -1;
    if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
          (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '/' ||
          *c == '-'))
      return -1;
  }
  if (strstr(path, "..")) return -1;
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  long w = (long)fwrite(p, 1, (size_t)n, f);
  fclose(f);
  return (int)w;
}
