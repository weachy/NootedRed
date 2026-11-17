// VP9 uncompressed header parser and superframe splitter. Only what a VCN
// decode message needs: the firmware reads the compressed header and the tiles
// itself and keeps the probability contexts.

#ifndef VCNVP9_VP9PARSE_H_
#define VCNVP9_VP9PARSE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool valid;
  int dpb_slot;
  uint32_t width, height;
  int bit_depth;
} VP9Ref;

typedef struct {
  int profile;
  int show_existing_frame, frame_to_show;
  int frame_type;  // 0 = key, 1 = non key, as coded
  int show_frame, error_res, intra_only, reset_frame_context;
  int bit_depth, subsampling_x, subsampling_y;
  uint32_t refresh_frame_flags;
  int ref_frame_idx[3];
  int sign_bias[3];
  uint32_t width, height;
  int allow_high_precision_mv;
  int interp_filter;  // firmware numbering: 0 regular, 1 smooth, 2 sharp, 3
                      // bilinear, 4 switchable
  int refresh_frame_context, frame_parallel;
  int frame_context_idx;
  int lf_level, lf_sharpness, lf_delta_enabled, lf_delta_update;
  int base_q_idx, y_dc_delta_q, uv_dc_delta_q, uv_ac_delta_q;
  int seg_enabled, seg_update_map, seg_temporal_update, seg_update_data;
  int log2_tile_cols, log2_tile_rows;
  uint32_t compressed_header_size, uncompressed_header_size;
  int use_prev_frame_mvs;
} VP9Frame;

typedef struct {
  VP9Frame fh;
  // state that outlives a frame
  VP9Ref ref[8];
  int bit_depth, subsampling_x, subsampling_y;
  int8_t lf_ref_deltas[4], lf_mode_deltas[2];
  int seg_abs_delta;
  uint8_t seg_feature_mask[8];
  int16_t seg_feature_data[8][4];
  uint8_t seg_tree_probs[7], seg_pred_probs[3];
  uint32_t last_width, last_height;
  int last_show_frame, last_intra_only, last_frame_type, have_last;
  char err[128];
} VP9Parser;

typedef enum { VP9_PARSE_DECODE, VP9_PARSE_SHOW_EXISTING } VP9ParseResult;

// Called once per coded frame, with that frame's bytes (superframe index
// removed).
typedef int (*VP9FrameFn)(void* ctx, VP9Parser* p, VP9ParseResult kind,
                          const uint8_t* data, size_t size);

void vp9_parser_init(VP9Parser* p);
// Parses one frame's uncompressed header into p->fh. Returns 0 or -1 with
// p->err set.
int vp9_parse_frame_header(VP9Parser* p, const uint8_t* data, size_t size);
// Applies refresh_frame_flags and the last-frame state after a frame was
// decoded into dpb_slot.
void vp9_frame_done(VP9Parser* p, int dpb_slot);
// Splits a container chunk (a superframe or a single frame) and runs fn on each
// coded frame.
int vp9_parse_chunk(VP9Parser* p, const uint8_t* data, size_t size,
                    VP9FrameFn fn, void* ctx);
// True for what the VCN can decode: profile 0 or 2, 8 or 10 bit, 4:2:0.
bool vp9_frame_supported(const VP9Parser* p);
// Firmware message value: per segment, per reference, per mode loop filter
// level (libvpx rule).
void vp9_lf_adj_levels(const VP9Parser* p, uint8_t out[8][4][2]);

#endif  // VCNVP9_VP9PARSE_H_
