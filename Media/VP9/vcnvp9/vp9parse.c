#include "vp9parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const uint8_t* d;
  size_t bits, pos;
  bool over;
} BR;

static uint32_t f(BR* b, int n) {
  uint32_t v = 0;
  if (n < 0 || n > 32) {
    b->over = true;
    return 0;
  }
  for (int i = 0; i < n; i++) {
    if (b->pos >= b->bits) {
      b->over = true;
      return 0;
    }
    v = (v << 1) | ((b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
    b->pos++;
  }
  return v;
}
static int su(BR* b, int n) {
  if (n < 0 || n > 31) {
    b->over = true;
    return 0;
  }
  int v = (int)f(b, n);
  return f(b, 1) ? -v : v;
}

static int fail(VP9Parser* p, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(p->err, sizeof(p->err), fmt, a);
  va_end(a);
  return -1;
}

void vp9_parser_init(VP9Parser* p) {
  memset(p, 0, sizeof(*p));
  for (int i = 0; i < 8; i++) p->ref[i].dpb_slot = -1;
  p->bit_depth = 8;
  p->subsampling_x = p->subsampling_y = 1;
  memset(p->seg_tree_probs, 255, sizeof(p->seg_tree_probs));
  memset(p->seg_pred_probs, 255, sizeof(p->seg_pred_probs));
}

static int color_config(VP9Parser* p, BR* b, int profile) {
  int depth = 8;
  if (profile >= 2) depth = f(b, 1) ? 12 : 10;
  int cs = (int)f(b, 3);
  int ssx, ssy;
  if (cs != 7) {
    f(b, 1);  // color_range
    if (profile == 1 || profile == 3) {
      ssx = (int)f(b, 1);
      ssy = (int)f(b, 1);
      f(b, 1);
    } else {
      ssx = ssy = 1;
    }
  } else {
    if (profile != 1 && profile != 3)
      return fail(p, "RGB needs profile 1 or 3");
    ssx = ssy = 0;
    f(b, 1);
  }
  p->bit_depth = depth;
  p->subsampling_x = ssx;
  p->subsampling_y = ssy;
  return 0;
}

static void frame_size(BR* b, VP9Frame* h) {
  h->width = f(b, 16) + 1;
  h->height = f(b, 16) + 1;
}
static void render_size(BR* b) {
  if (f(b, 1)) f(b, 32);
}

// setup_past_independence: only the parts this side tracks, the firmware resets
// its own probabilities.
static void past_independence(VP9Parser* p) {
  memset(p->seg_feature_mask, 0, sizeof(p->seg_feature_mask));
  memset(p->seg_feature_data, 0, sizeof(p->seg_feature_data));
  p->seg_abs_delta = 0;
  static const int8_t refd[4] = {1, 0, -1, -1};
  memcpy(p->lf_ref_deltas, refd, 4);
  p->lf_mode_deltas[0] = p->lf_mode_deltas[1] = 0;
}

static uint8_t read_prob(BR* b) { return f(b, 1) ? (uint8_t)f(b, 8) : 255; }

static void segmentation_params(VP9Parser* p, BR* b, VP9Frame* h) {
  static const int bits[4] = {8, 6, 2, 0};
  static const int sign[4] = {1, 1, 0, 0};
  h->seg_update_map = h->seg_temporal_update = h->seg_update_data = 0;
  h->seg_enabled = (int)f(b, 1);
  if (!h->seg_enabled) return;
  h->seg_update_map = (int)f(b, 1);
  if (h->seg_update_map) {
    for (int i = 0; i < 7; i++) p->seg_tree_probs[i] = read_prob(b);
    h->seg_temporal_update = (int)f(b, 1);
    for (int i = 0; i < 3; i++)
      p->seg_pred_probs[i] = h->seg_temporal_update ? read_prob(b) : 255;
  }
  h->seg_update_data = (int)f(b, 1);
  if (h->seg_update_data) {
    p->seg_abs_delta = (int)f(b, 1);
    memset(p->seg_feature_mask, 0, sizeof(p->seg_feature_mask));
    memset(p->seg_feature_data, 0, sizeof(p->seg_feature_data));
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 4; j++) {
        if (!f(b, 1)) continue;
        p->seg_feature_mask[i] |= (uint8_t)(1 << j);
        int v = bits[j] ? (int)f(b, bits[j]) : 0;
        if (sign[j] && f(b, 1)) v = -v;
        p->seg_feature_data[i][j] = (int16_t)v;
      }
  }
}

static void tile_info(BR* b, VP9Frame* h) {
  uint32_t sb64_cols = (((h->width + 7) >> 3) + 7) >> 3;
  int min_log2 = 0, max_log2 = 1;
  while ((64u << min_log2) < sb64_cols) min_log2++;
  while ((sb64_cols >> max_log2) >= 4) max_log2++;
  max_log2--;
  h->log2_tile_cols = min_log2;
  while (h->log2_tile_cols < max_log2 && f(b, 1)) h->log2_tile_cols++;
  h->log2_tile_rows = (int)f(b, 1);
  if (h->log2_tile_rows) h->log2_tile_rows += (int)f(b, 1);
}

int vp9_parse_frame_header(VP9Parser* p, const uint8_t* data, size_t size) {
  if (!p) return -1;
  if ((!data && size) || size > SIZE_MAX / 8)
    return fail(p, "invalid input size");
  BR br = {data, size * 8, 0, false}, *b = &br;
  VP9Frame* h = &p->fh;
  memset(h, 0, sizeof(*h));
  p->err[0] = 0;
  if (f(b, 2) != 2) return fail(p, "bad frame marker");
  int lo = (int)f(b, 1), hi = (int)f(b, 1);
  h->profile = (hi << 1) | lo;
  if (h->profile == 3 && f(b, 1)) return fail(p, "reserved bit set");
  h->show_existing_frame = (int)f(b, 1);
  if (h->show_existing_frame) {
    h->frame_to_show = (int)f(b, 3);
    return b->over ? fail(p, "truncated header") : 0;
  }
  h->frame_type = (int)f(b, 1);
  h->show_frame = (int)f(b, 1);
  h->error_res = (int)f(b, 1);
  bool is_intra;
  if (h->frame_type == 0) {
    if (f(b, 24) != 0x498342) return fail(p, "bad sync code");
    if (color_config(p, b, h->profile)) return -1;
    frame_size(b, h);
    render_size(b);
    h->refresh_frame_flags = 0xff;
    is_intra = true;
  } else {
    h->intra_only = h->show_frame ? 0 : (int)f(b, 1);
    h->reset_frame_context = h->error_res ? 0 : (int)f(b, 2);
    is_intra = h->intra_only;
    if (h->intra_only) {
      if (f(b, 24) != 0x498342) return fail(p, "bad sync code");
      if (h->profile > 0) {
        if (color_config(p, b, h->profile)) return -1;
      } else {
        p->bit_depth = 8;
        p->subsampling_x = p->subsampling_y = 1;
      }
      h->refresh_frame_flags = f(b, 8);
      frame_size(b, h);
      render_size(b);
    } else {
      h->refresh_frame_flags = f(b, 8);
      for (int i = 0; i < 3; i++) {
        h->ref_frame_idx[i] = (int)f(b, 3);
        h->sign_bias[i] = (int)f(b, 1);
      }
      bool found = false;
      for (int i = 0; i < 3 && !found; i++)
        if (f(b, 1)) {
          const VP9Ref* r = &p->ref[h->ref_frame_idx[i]];
          if (!r->valid) return fail(p, "frame size from a missing reference");
          h->width = r->width;
          h->height = r->height;
          found = true;
        }
      if (!found) frame_size(b, h);
      render_size(b);
      h->allow_high_precision_mv = (int)f(b, 1);
      if (f(b, 1)) {
        h->interp_filter = 4;
      } else {
        static const int lit[4] = {1, 0, 2, 3};
        h->interp_filter = lit[f(b, 2)];
      }
    }
  }
  h->bit_depth = p->bit_depth;
  h->subsampling_x = p->subsampling_x;
  h->subsampling_y = p->subsampling_y;
  if (!h->error_res) {
    h->refresh_frame_context = (int)f(b, 1);
    h->frame_parallel = (int)f(b, 1);
  } else {
    h->refresh_frame_context = 0;
    h->frame_parallel = 1;
  }
  h->frame_context_idx = (int)f(b, 2);
  if (is_intra || h->error_res) past_independence(p);
  if (is_intra)
    h->frame_context_idx = 0;  // libvpx ignores the coded value here
  // loop filter
  h->lf_level = (int)f(b, 6);
  h->lf_sharpness = (int)f(b, 3);
  h->lf_delta_enabled = (int)f(b, 1);
  if (h->lf_delta_enabled) {
    h->lf_delta_update = (int)f(b, 1);
    if (h->lf_delta_update) {
      for (int i = 0; i < 4; i++)
        if (f(b, 1)) p->lf_ref_deltas[i] = (int8_t)su(b, 6);
      for (int i = 0; i < 2; i++)
        if (f(b, 1)) p->lf_mode_deltas[i] = (int8_t)su(b, 6);
    }
  }
  // quantization
  h->base_q_idx = (int)f(b, 8);
  h->y_dc_delta_q = f(b, 1) ? su(b, 4) : 0;
  h->uv_dc_delta_q = f(b, 1) ? su(b, 4) : 0;
  h->uv_ac_delta_q = f(b, 1) ? su(b, 4) : 0;
  segmentation_params(p, b, h);
  tile_info(b, h);
  h->compressed_header_size = f(b, 16);
  if (b->over) return fail(p, "truncated header");
  if (!h->compressed_header_size) return fail(p, "no compressed header");
  h->uncompressed_header_size = (uint32_t)((b->pos + 7) >> 3);
  if (h->uncompressed_header_size + h->compressed_header_size > size)
    return fail(p, "headers past the frame");
  h->use_prev_frame_mvs =
      p->have_last && !h->error_res && h->width == p->last_width &&
      h->height == p->last_height && p->last_show_frame &&
      !p->last_intra_only && p->last_frame_type != 0;
  return 0;
}

void vp9_frame_done(VP9Parser* p, int dpb_slot) {
  const VP9Frame* h = &p->fh;
  for (int i = 0; i < 8; i++)
    if (h->refresh_frame_flags & (1u << i))
      p->ref[i] = (VP9Ref){.valid = true,
                           .dpb_slot = dpb_slot,
                           .width = h->width,
                           .height = h->height,
                           .bit_depth = h->bit_depth};
  p->last_width = h->width;
  p->last_height = h->height;
  p->last_show_frame = h->show_frame;
  p->last_intra_only = h->intra_only;
  p->last_frame_type = h->frame_type;
  p->have_last = 1;
}

bool vp9_frame_supported(const VP9Parser* p) {
  const VP9Frame* h = &p->fh;
  if (h->profile != 0 && h->profile != 2) return false;
  if (h->bit_depth != 8 && h->bit_depth != 10) return false;
  return h->subsampling_x == 1 && h->subsampling_y == 1;
}

static int clamp63(int v) { return v < 0 ? 0 : v > 63 ? 63 : v; }

void vp9_lf_adj_levels(const VP9Parser* p, uint8_t out[8][4][2]) {
  const VP9Frame* h = &p->fh;
  const int scale = 1 << (h->lf_level >> 5);
  memset(out, 0, 8 * 4 * 2);
  for (int s = 0; s < 8; s++) {
    int base = h->lf_level;
    if (h->seg_enabled && (p->seg_feature_mask[s] & 2)) {
      int d = p->seg_feature_data[s][1];
      if (!p->seg_abs_delta) d += base;
      base = clamp63(d);
    }
    out[s][0][0] = (uint8_t)(h->lf_delta_enabled
                                 ? clamp63(base + p->lf_ref_deltas[0] * scale)
                                 : base);
    for (int r = 1; r < 4; r++)
      for (int m = 0; m < 2; m++)
        out[s][r][m] = (uint8_t)(h->lf_delta_enabled
                                     ? clamp63(base + (p->lf_ref_deltas[r] +
                                                       p->lf_mode_deltas[m]) *
                                                          scale)
                                     : base);
  }
}

int vp9_parse_chunk(VP9Parser* p, const uint8_t* data, size_t size,
                    VP9FrameFn fn, void* ctx) {
  if (!p) return -1;
  if (!size) return 0;
  if (!data || !fn) return fail(p, "invalid chunk input");
  size_t sizes[8], count = 0;
  uint8_t last = data[size - 1];
  if ((last & 0xe0) == 0xc0) {
    int frames = (last & 7) + 1, mag = ((last >> 3) & 3) + 1;
    size_t index = 2 + (size_t)mag * (size_t)frames;
    if (size >= index && data[size - index] == last) {
      const uint8_t* x = data + size - index + 1;
      size_t total = 0;
      for (int i = 0; i < frames; i++) {
        size_t s = 0;
        for (int j = 0; j < mag; j++) s |= (size_t)*x++ << (8 * j);
        if (s > SIZE_MAX - total) return fail(p, "superframe sizes overflow");
        sizes[i] = s;
        total += s;
      }
      if (total != size - index)
        return fail(p, "superframe sizes do not match the chunk");
      count = (size_t)frames;
    }
  }
  if (!count) {
    sizes[0] = size;
    count = 1;
  }
  size_t off = 0;
  for (size_t i = 0; i < count; i++) {
    size_t s = sizes[i];
    if (!s) continue;
    const uint8_t* d = data + off;
    off += s;
    if (vp9_parse_frame_header(p, d, s)) return -1;
    int r = fn(
        ctx, p,
        p->fh.show_existing_frame ? VP9_PARSE_SHOW_EXISTING : VP9_PARSE_DECODE,
        d, s);
    if (r) return r;
  }
  return 0;
}
