// VCN decode message layouts for VP9, same layout as Mesa's ac_vcn_dec.h.

#ifndef VCNVP9_VP9MSG_H_
#define VCNVP9_VP9MSG_H_

#include <stdint.h>

#define VCN_CODEC_VP9 0x11u
#define VCN_CODEC_AV1 0x13u
#define VCN_MESSAGE_DECODE 2u
#define VCN_MESSAGE_DRM 3u
#define VCN_MESSAGE_VP9 0xEu
#define VCN_MESSAGE_0F 15u  // the driver always sends this one zeroed
#define VCN_MESSAGE_DYNAMIC_DPB 16u
#define VCN_DECODE_USE_DYNAMIC_DPB 0x1u
#define VCN_DECODE_USE_PAL 0x8u

#define VP9_HDR_FRAME_TYPE (1u << 1)
#define VP9_HDR_ERROR_RES (1u << 2)
#define VP9_HDR_INTRA_ONLY (1u << 3)
#define VP9_HDR_ALLOW_HP (1u << 4)
#define VP9_HDR_REFRESH_CTX (1u << 5)
#define VP9_HDR_PARALLEL (1u << 6)
#define VP9_HDR_SEG_ENABLED (1u << 7)
#define VP9_HDR_SEG_UPDATE_MAP (1u << 8)
#define VP9_HDR_SEG_TEMPORAL (1u << 9)
#define VP9_HDR_SEG_UPDATE_DATA (1u << 10)
#define VP9_HDR_LF_DELTA_ENABLED (1u << 11)
#define VP9_HDR_LF_DELTA_UPDATE (1u << 12)
#define VP9_HDR_USE_PREV_MVS (1u << 13)
#define VP9_HDR_USE_UNCOMPRESSED (1u << 14)

// VCN1/2 VP9 tier-1 reference-array descriptor, matching
// rvcn_dec_message_dynamic_dpb_t. Ten-bit internal reference storage uses
// 3/2 the eight-bit byte sizes, independently of the P010 output surface.
typedef struct {
  uint32_t flags;
  uint32_t luma_pitch, luma_height, luma_size;
  uint32_t chroma_pitch, chroma_height, chroma_size;
  uint8_t array_size, current_slice, ref_slices[16], reserved[2];
  uint32_t current_offset, offsets[16];
} VcnMsgDynamicDPB;

_Static_assert(sizeof(VcnMsgDynamicDPB) == 116, "tier-1 DPB wire layout");

typedef struct {
  uint32_t frame_header_flags;
  uint8_t frame_context_idx, reset_frame_context;
  uint8_t curr_pic_idx, interp_filter;
  uint8_t filter_level, sharpness_level;
  uint8_t lf_adj_level[8][4][2];
  uint8_t base_qindex;
  int8_t y_dc_delta_q, uv_ac_delta_q, uv_dc_delta_q;
  uint8_t log2_tile_cols, log2_tile_rows;
  uint8_t tx_mode, reference_mode, chroma_format;
  uint8_t ref_frame_map[8];
  uint8_t frame_refs[3];
  uint8_t ref_frame_sign_bias[3];
  uint8_t frame_to_show;
  uint8_t bit_depth_luma_minus8, bit_depth_chroma_minus8;
  uint8_t p010_mode, msb_mode, luma_10to8, chroma_10to8;
  uint32_t vp9_frame_size;
  uint32_t compressed_header_size;
  uint32_t uncompressed_header_size;
  uint8_t reserved[2];
} VcnMsgVP9;

typedef struct {
  uint16_t classes_mask[2];
  uint16_t bits_mask[2];
  uint8_t joints_mask;
  uint8_t sign_mask[2];
  uint8_t class0_mask[2];
  uint8_t class0_fp_mask[2];
  uint8_t fp_mask[2];
  uint8_t class0_hp_mask[2];
  uint8_t hp_mask[2];
  uint8_t reserve[11];
} VcnVP9NmvMask;

typedef struct {
  uint8_t sign;
  uint8_t classes[10];
  uint8_t class0[1];
  uint8_t bits[10];
  uint8_t class0_fp[2][3];
  uint8_t fp[3];
  uint8_t class0_hp;
  uint8_t hp;
} VcnVP9NmvComp;

typedef struct {
  VcnVP9NmvMask nmvc_mask;
  uint8_t coef_probs[4][2][2][6][6][3];
  uint8_t y_mode_prob[4][9];
  uint8_t uv_mode_prob[10][9];
  uint8_t single_ref_prob[5][2];
  uint8_t switchable_interp_prob[4][2];
  uint8_t partition_prob[16][3];
  uint8_t inter_mode_probs[7][3];
  uint8_t mbskip_probs[3];
  uint8_t intra_inter_prob[4];
  uint8_t comp_inter_prob[5];
  uint8_t comp_ref_prob[5];
  uint8_t tx_probs_32x32[2][3];
  uint8_t tx_probs_16x16[2][2];
  uint8_t tx_probs_8x8[2][1];
  uint8_t mv_joints[3];
  VcnVP9NmvComp mv_comps[2];
} VcnVP9Probs;

// The prob table buffer: Mesa only fills the segment part per frame, the
// probabilities live in the context.
#define VCN_VP9_PROBS_DATA_SIZE 2304u
typedef struct {
  uint32_t feature_data[8];
  uint8_t tree_probs[7];
  uint8_t pred_probs[3];
  uint8_t abs_delta;
  uint8_t feature_mask[8];
} VcnVP9Seg;

_Static_assert(sizeof(VcnVP9Probs) <= VCN_VP9_PROBS_DATA_SIZE,
               "probability table layout");
_Static_assert(sizeof(VcnVP9NmvComp) == 33,
               "default_nmv_components entry size");

#endif  // VCNVP9_VP9MSG_H_
