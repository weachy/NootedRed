// Exercise the actual message builder with mock driver objects; no GPU access.
#include <assert.h>
#include "vcnvp9/vcnvp9.c"

typedef struct {
  uint8_t bytes[128];
  unsigned position;
} HeaderBits;

static void put_bits(HeaderBits *bits, uint32_t value, unsigned count) {
  for (unsigned i = count; i > 0; i--) {
    unsigned at = bits->position++;
    bits->bytes[at / 8] |= ((value >> (i - 1)) & 1) << (7 - at % 8);
  }
}

// Minimal 64x64 inter-frame uncompressed header. A dummy compressed-header byte
// is sufficient: this test parses syntax/state and never submits the payload.
static unsigned inter_header(HeaderBits *b, unsigned error_resilient) {
  memset(b, 0, sizeof(*b));
  put_bits(b, 2, 2); // Frame marker.
  put_bits(b, 0, 2); // Profile 0.
  put_bits(b, 0, 1); // Not show_existing.
  put_bits(b, 1, 1); // Inter frame.
  put_bits(b, 1, 1); // Shown.
  put_bits(b, error_resilient, 1);
  if (!error_resilient) put_bits(b, 0, 2); // Reset context.
  put_bits(b, 1, 8); // Refresh LAST.
  for (unsigned i = 0; i < 3; i++) {
    put_bits(b, i, 3);
    put_bits(b, 0, 1);
  }
  put_bits(b, 1, 1); // Size from LAST.
  put_bits(b, 0, 1); // Default render size.
  put_bits(b, 0, 1); // No high-precision MVs.
  put_bits(b, 1, 1); // Switchable interpolation.
  if (!error_resilient) {
    put_bits(b, 1, 1); // Refresh context.
    put_bits(b, 0, 1); // Not frame-parallel.
  }
  put_bits(b, 0, 2); // Context 0.
  put_bits(b, 0, 6); // Loop filter.
  put_bits(b, 0, 3); // Sharpness.
  put_bits(b, 0, 1); // No loop-filter deltas.
  put_bits(b, 0, 8); // Quantizer.
  put_bits(b, 0, 3); // No quantizer deltas.
  put_bits(b, 0, 1); // No segmentation.
  put_bits(b, 0, 1); // One tile row (one column implied by 64x64).
  put_bits(b, 1, 16); // Compressed-header length.
  return (b->position + 7) / 8 + 1;
}

static unsigned test_previous_mvs(void) {
  VP9Parser p;
  vp9_parser_init(&p);
  p.bit_depth = 8;
  p.subsampling_x = p.subsampling_y = 1;
  p.fh = (VP9Frame){.width = 64, .height = 64, .bit_depth = 8,
                   .show_frame = 1, .refresh_frame_flags = 0xFF};
  vp9_frame_done(&p, 0); // Shown keyframe; intra_only syntax is zero.
  HeaderBits header;
  unsigned size = inter_header(&header, 0);
  assert(vp9_parse_frame_header(&p, header.bytes, size) == 0);
  assert(!p.fh.use_prev_frame_mvs); // Keyframes have no reusable motion field.
  vp9_frame_done(&p, 1);
  assert(vp9_parse_frame_header(&p, header.bytes, size) == 0);
  assert(p.fh.use_prev_frame_mvs);
  VP9Parser baseline = p;
  unsigned checks = 2;
  for (unsigned failure = 0; failure < 6; failure++) {
    p = baseline;
    size = inter_header(&header, failure == 5);
    switch (failure) {
    case 0: p.last_frame_type = 0; break;
    case 1: p.last_intra_only = 1; break;
    case 2: p.last_show_frame = 0; break;
    case 3: p.last_width = 32; break;
    case 4: p.last_height = 32; break;
    }
    assert(vp9_parse_frame_header(&p, header.bytes, size) == 0);
    assert(!p.fh.use_prev_frame_mvs);
    checks++;
  }
  return checks;
}

int main(void) {
  uint8_t proc[80] = {0}, dec[160] = {0}, data[704] = {0};
  uint8_t context[1136] = {0}, hw[160] = {0}, texture[160] = {0};
  uint8_t message[4096], segments[VCN_VP9_PROBS_DATA_SIZE + 256 + 16];
  uint8_t bindings[208] = {0};
  VP9Parser parser;
  vp9_parser_init(&parser);
  PTR(proc, 16) = dec;
  PTR(proc, 24) = context;
  PTR(dec, 88) = data;
  PTR(context, 1120) = hw;
  PTR(data, 224) = message;
  PTR(data, 632) = segments;
  U32(data, 688) = BS_SIZE;
  VP9Params params = {.vp9 = &parser, .mif_wrc_en = 0};
  const uint32_t dimensions[][2] = {
      {64, 64}, {1920, 1080}, {1918, 1080}, {3840, 2160},
      {4096, 2304}, {7680, 4320}, {8192, 4320}, {7680, 4352},
      {8192, 4352}};
  const unsigned devices[] = {0x1636, 0x1638, 0x164C, 0x15E7, 0x73BF, 0x73AF};
  unsigned checks = test_previous_mvs();
  for (unsigned i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
    uint32_t width = dimensions[i][0], height = dimensions[i][1];
    assert(vcnvp9_dimensions_supported(width, height));
    for (unsigned dev = 0; dev < sizeof(devices) / sizeof(devices[0]); dev++) {
      unsigned tier = nredMediaVP9DPBTier(devices[dev]);
      if (!tier && (uint64_t)width * height > VCN_VP9_TIER0_MAX_PIXELS)
        continue;
      params.dpb_tier = tier;
      params.mif_wrc_en = nredMediaVP9WriteCombine(devices[dev]);
      for (uint32_t depth = 8; depth <= 10; depth += 2) {
        uint32_t bpe = depth == 10 ? 2 : 1;
        uint32_t pitch = al(width * bpe, 256), texh = al(height, 64);
        memset(message, 0xA5, sizeof(message));
        memset(segments, 0xA5, sizeof(segments));
        DATA_DPB_SIZE(data) = DATA_CTX_SIZE(data) = 0;
        DEC_WIDTH(dec) = parser.fh.width = width;
        DEC_HEIGHT(dec) = parser.fh.height = height;
        parser.fh.bit_depth = depth;
        parser.fh.profile = depth == 10 ? 2 : 0;
        TEX_HEIGHT(texture) = texh;
        TEX_PITCH(texture) = pitch;
        TEX_BPE(texture) = bpe;
        assert(hook_set_message(proc, &params, texture, bindings, 0, 0) == 0);
        assert(U32(message, 8) == (tier ? 5 : 4));
        uint32_t decode_offset = tier ? 104 : 88;
        assert(U32(message, 28) == decode_offset);
        uint8_t *m = message + decode_offset;
        assert(U32(m, 0) == VCN_CODEC_VP9);
        assert(U32(m, 4) == (tier ?
            (VCN_DECODE_USE_DYNAMIC_DPB | VCN_DECODE_USE_PAL) : 0));
        assert(U32(m, 8) == width && U32(m, 12) == height);
        assert(U32(m, 24) == pitch * texh * 3 / 2);
        assert(U32(m, 72) == al(width, 64) && U32(m, 76) == texh);
        assert(U32(m, 100) == pitch / bpe);
        assert(U32(m, 104) == pitch / bpe / 2);
        assert(U32(m, 140) < U32(m, 24));
        assert(U32(m, 148) < U32(m, 24));
        // Verify each device's actual packet: VCN 2.2 cannot receive Navi's
        // write-combining flag, and Navi retains the original flag/layout.
        assert(U32(m, 160) == (tier ? 0 : 1));
        VcnMsgVP9 *vp9 = (VcnMsgVP9 *)(m + 188);
        assert(vp9->bit_depth_luma_minus8 == depth - 8);
        assert(vp9->p010_mode == (depth == 10));
        assert(vp9->msb_mode == (depth == 10));
        assert(DATA_DPB_SIZE(data) == hook_dpb_size(proc));
        assert(DATA_CTX_SIZE(data) == hook_ctx_size(proc));
        if (tier) {
          assert(U32(message, 88) == VCN_MESSAGE_DYNAMIC_DPB);
          assert(U32(message, 96) == sizeof(VcnMsgDynamicDPB));
          VcnMsgDynamicDPB *dynamic =
              (VcnMsgDynamicDPB *)(message + U32(message, 92));
          assert(dynamic->array_size == NUM_DPB);
          assert(dynamic->luma_pitch == VCN_VP9_MAX_WIDTH);
          assert(dynamic->luma_height == VCN_VP9_MAX_HEIGHT);
          assert(dynamic->chroma_pitch == dynamic->luma_pitch / 2);
          assert(dynamic->chroma_height == dynamic->luma_height / 2);
          uint32_t luma = VCN_VP9_MAX_PIXELS;
          uint32_t chroma = luma / 2;
          if (depth == 10) { luma = luma * 3 / 2; chroma = chroma * 3 / 2; }
          assert(dynamic->luma_size == luma && dynamic->chroma_size == chroma);
          assert((dynamic->luma_size + dynamic->chroma_size) *
                 dynamic->array_size == DATA_DPB_SIZE(data));
          assert(dynamic->reserved[0] == 64);
          assert(dynamic->flags == 0 && dynamic->current_offset == 0);
          assert(dynamic->current_slice == 0 && dynamic->reserved[1] == 0);
          for (unsigned at = 0; at < 16; at++)
            assert(dynamic->offsets[at] == 0 && dynamic->ref_slices[at] == 0);
        }
        uint32_t end = U32(message, 4);
        assert(end < sizeof(message));
        for (unsigned at = end; at < sizeof(message); at++)
          assert(message[at] == 0xA5);
        for (unsigned at = VCN_VP9_PROBS_DATA_SIZE + 256;
             at < sizeof(segments); at++)
          assert(segments[at] == 0xA5);
        checks++;
      }
    }
  }
  const uint32_t invalid[][2] = {
      {0, 1080}, {1920, 0}, {8193, 4352}, {8192, 4353},
      {UINT32_MAX, 1080}, {1920, UINT32_MAX}};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    char error[128] = "";
    assert(!vcnvp9_dimensions_supported(invalid[i][0], invalid[i][1]));
    if (!vcnvp9_bounds_supported(invalid[i][0], invalid[i][1])) {
      assert(!vcnvp9_create(invalid[i][0], invalid[i][1], error, sizeof(error)));
      assert(strstr(error, "invalid VP9 session maximum"));
    } else {
      VcnVP9 session = {.maxW = VCN_VP9_MAX_WIDTH, .maxH = VCN_VP9_MAX_HEIGHT};
      parser.fh.width = invalid[i][0];
      parser.fh.height = invalid[i][1];
      parser.fh.profile = 0;
      parser.fh.bit_depth = 8;
      parser.fh.subsampling_x = parser.fh.subsampling_y = 1;
      assert(on_frame(&session, &parser, VP9_PARSE_DECODE, NULL, 0) == -1);
      assert(strstr(session.err, "over the session maximum"));
    }
    checks++;
  }
  // Exercise the real frame gate for every backend and both bit depths.
  // An oversized bitstream stops valid dimensions before any allocation or
  // submission. This distinguishes VCN 2.2's full rectangle from Navi's
  // retained tier-0 area bound without involving the GPU.
  for (unsigned dev = 0; dev < sizeof(devices) / sizeof(devices[0]); dev++) {
    VcnVP9 session = {.maxW = VCN_VP9_MAX_WIDTH, .maxH = VCN_VP9_MAX_HEIGHT,
                      .deviceID = devices[dev]};
    for (unsigned depth = 8; depth <= 10; depth += 2) {
      parser.fh = (VP9Frame){.width = 8192, .height = 4352,
                             .profile = depth == 10 ? 2 : 0,
                             .bit_depth = depth, .subsampling_x = 1, .subsampling_y = 1};
      assert(on_frame(&session, &parser, VP9_PARSE_DECODE, NULL, BS_SIZE) == -1);
      assert(strstr(session.err, nredMediaUsesVCN2Bridge(devices[dev])
                    ? "frame too big" : "over the session maximum"));
      checks++;
    }
  }
  printf("PASS %u VP9 dimensions/actual message/buffer-bound checks; no GPU work\n", checks);
  return 0;
}
