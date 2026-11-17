#!/usr/bin/env python3
"""Replay the real AVC/HEVC capability checks with mock objects, without GPU work.

Only the lock-status helper at function entry is stubbed. All dimension,
profile and count checks are executed from the reviewed driver instructions.
"""
import ctypes
import mmap
import struct

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH
from media_patch_manifest import HEVC_DECODE_SITES, AVC_DECODE_SITES, AVC_ENCODE_SITES


def main():
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        image = MachOImage(cache, VA_IMAGE_PATH)
        start = image.symbol(HEVC_DECODE_SITES[0][0])
        # Include both error-return stubs after the main epilogue.
        original = bytearray(cache.read(start, 0x370))
    assert original[0x11] == 0xE8
    original[0x11:0x16] = bytes.fromhex("31c0909090")
    patched = bytearray(original)
    for _, offset, find, replace in HEVC_DECODE_SITES + AVC_DECODE_SITES + AVC_ENCODE_SITES:
        find, replace = bytes.fromhex(find), bytes.fromhex(replace)
        assert original[offset:offset + len(find)] in (find, replace)
        original[offset:offset + len(find)] = find
        patched[offset:offset + len(find)] = replace

    capability = bytearray(0xB400)
    for offset, value in {
        0xB300: 4096 * 2304, 0xB304: 8192 * 4320,
        0xB308: 64, 0xB30C: 64, 0xB310: 4096, 0xB314: 2304,
        0xB318: 64, 0xB31C: 64, 0xB320: 4096, 0xB324: 8192, 0xB328: 4320,
        0xB350: 4096, 0xB354: 2304, 0xB358: 4096, 0xB35C: 2304,
        0xB374: 0x3C5E6,
    }.items():
        struct.pack_into("<I", capability, offset, value)
    cap = (ctypes.c_ubyte * len(capability)).from_buffer(capability)
    info = (ctypes.c_ubyte * 64)()
    entries = (ctypes.c_void_p * 1)(ctypes.addressof(info))
    context = (ctypes.c_ubyte * 24)()
    struct.pack_into("<I", context, 0, 1)
    struct.pack_into("<Q", context, 8, ctypes.addressof(entries))

    def executable(code):
        region = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        region[:len(code)] = code
        function = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)(
            ctypes.addressof(ctypes.c_char.from_buffer(region)))
        return region, function

    old_region, old = executable(original)
    new_region, new = executable(patched)
    checks = 0
    for codec, dimensions in (
        (0x800, ((64, 64), (1920, 1080), (3840, 2160), (4096, 2304),
                 (7680, 4320), (8192, 4320), (8192, 4352), (8193, 4352),
                 (8192, 4353), (63, 1080), (1920, 63), (0, 0),
                 (65536, 65536), (0xFFFFFFFF, 1080), (1920, 0xFFFFFFFF))),
        (8, ((64, 64), (1920, 1080), (4096, 2304), (2304, 4096),
             (4096, 4096), (4097, 2304), (4096, 4097), (7680, 4320), (63, 1080),
             (4096, 2305), (2305, 4096), (3072, 3072), (3072, 3073),
             (4080, 4096), (4096, 4080), (4082, 4082),
             (65536, 65536), (0xFFFFFFFF, 1080), (1920, 0xFFFFFFFF))),
    ):
        for internal in (0, 1):
            cap[0xB378] = internal
            for width, height in dimensions:
                for mode in (4, 5):
                    struct.pack_into("<I", info, 0, 1)
                    struct.pack_into("<Q", info, 8, codec)
                    struct.pack_into("<I", info, 0x10, mode)
                    struct.pack_into("<III", info, 0x18, 1, 0, width)
                    struct.pack_into("<I", info, 0x24, height)
                    for function, changed in ((old, False), (new, True)):
                        maxw, maxh = ((8192 if changed or internal else 4096,
                                       4352 if changed else 4320)
                                      if codec == 0x800 else (4096, 4096 if changed else 2304))
                        # Mode 5 requires an additional feature bit which is
                        # absent from this actual capability object.
                        maxpixels = ((8192 * (4352 if changed else 4320))
                                     if codec == 0x800 else 4096 * 2304)
                        supported = (mode == 4 and 64 <= width <= maxw and
                                     64 <= height <= maxh and width * height <= maxpixels)
                        before = bytes(cap), bytes(info), bytes(context)
                        actual = function(ctypes.addressof(cap), ctypes.addressof(context))
                        assert (actual == 0) == supported, (
                            hex(codec), internal, width, height, mode, changed, actual)
                        assert before == (bytes(cap), bytes(info), bytes(context))
                        checks += 1
    # The isolated capability function does not reject the HEVC square
    # geometry in this mock context even with its original instruction. The
    # HEVC patch is independently checked against the pinned driver image,
    # while actual square acceptance requires the hardware test. AVC's
    # original height gate does reject tall frames here.
    for codec in (0x20, 0x1000):
        for width, height in ((1920, 1080), (4096, 2304), (2304, 4096),
                              (4096, 4096), (4097, 2304), (4096, 4097),
                              (4096, 2305), (1920, 2305), (4096, 4080),
                              (4080, 4096), (4082, 4082),
                              (65536, 65536), (0xFFFFFFFF, 1080),
                              (1920, 0xFFFFFFFF)):
            for mode in (4, 5):
                for count in (0, 1):
                    struct.pack_into("<I", info, 0, 0x20)
                    struct.pack_into("<Q", info, 8, codec)
                    struct.pack_into("<I", info, 0x10, mode)
                    struct.pack_into("<III", info, 0x18, count, 0, width)
                    struct.pack_into("<I", info, 0x24, height)
                    for function, changed in ((old, False), (new, True)):
                        maximum = 4096 if changed or codec == 0x1000 else 2304
                        supported = mode == 4 and count == 1 and width <= 4096 and height <= maximum
                        before = bytes(cap), bytes(info), bytes(context)
                        actual = function(ctypes.addressof(cap), ctypes.addressof(context))
                        assert (actual == 0) == supported, (
                            "encode", hex(codec), width, height, mode, count, changed, actual)
                        assert before == (bytes(cap), bytes(info), bytes(context))
                        checks += 1
    old_region.close()
    new_region.close()
    print(f"PASS {checks} actual AVC/HEVC decode/encode boundary/profile checks; no GPU work")


if __name__ == "__main__":
    main()
