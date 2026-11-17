#!/usr/bin/env python3
"""Execute reviewed VCN2 format/offset instructions with mock objects, no GPU.

Uses only named functions in the live supported cache. Requires x86_64 macOS.
Run media_patch_manifest.py first to validate the full image/page identities.
"""
import ctypes
import mmap
import struct

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH
from media_patch_manifest import HEVC_ENCODE_SITES


def main():
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        image = MachOImage(cache, VA_IMAGE_PATH)
        caller = next(site for site in HEVC_ENCODE_SITES if "EncodePicture" in site[0])
        format_site = next(site for site in HEVC_ENCODE_SITES if "setInputAndOutputFormatParams" in site[0])
        chroma_sites = [site for site in HEVC_ENCODE_SITES if "encodeSliceHeader" in site[0]]
        assert len(chroma_sites) == 2
        for symbol, offset, find, replace in HEVC_ENCODE_SITES:
            at = image.symbol(symbol) + offset
            assert cache.read(at, len(bytes.fromhex(find))) in (bytes.fromhex(find), bytes.fromhex(replace))
        start = image.symbol(format_site[0])
        original = bytearray(cache.read(start, image.function_end(start) - start))
    assert len(original) == 190
    offset, find, replacement = format_site[1], bytes.fromhex(format_site[2]), bytes.fromhex(format_site[3])
    original[offset:offset + len(find)] = find
    patched = bytearray(original)
    patched[offset:offset + len(find)] = replacement

    def executable(code, modified):
        region = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        address = ctypes.addressof(ctypes.c_char.from_buffer(region))
        data = bytearray(code)
        # Relocate only the two original RGB constants into this mock image.
        for ins, where, value in ((0x3A, 0x300, (0, 4)), (0x44, 0x308, (1, 5))):
            ins += 2 if modified else 0
            struct.pack_into("<i", data, ins + 4, where - ins - 8)
            region[where:where + 8] = struct.pack("<II", *value)
        region[:len(data)] = data
        return region, ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)(address)

    records = []

    @ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
    def config(obj, key, pointer):
        count = 7 if key == 20 else 4 if key == 21 else 0
        records.append((key, list((ctypes.c_uint32 * count).from_address(pointer))))

    vtable = (ctypes.c_void_p * 15)()
    vtable[14] = ctypes.cast(config, ctypes.c_void_p).value
    command = (ctypes.c_void_p * 1)(ctypes.addressof(vtable))
    encoder = (ctypes.c_ubyte * 0x600)()
    texture = (ctypes.c_ubyte * 0x100)()
    struct.pack_into("<Q", encoder, 0x458, ctypes.addressof(command))
    checks = 0

    def check(fn, context, bpe, depth, hevc, input_words, output_words):
        nonlocal checks
        struct.pack_into("<I", encoder, 0x9C, context)
        struct.pack_into("<I", texture, 8, 0x13)
        struct.pack_into("<I", texture, 0x84, bpe)
        encoder[0x42C], encoder[0x52C] = depth, hevc
        records.clear()
        fn(ctypes.addressof(encoder), ctypes.addressof(texture))
        assert records == [(20, input_words), (21, output_words)], records
        checks += 1

    old_region, old = executable(original, False)
    new_region, new = executable(patched, True)
    for depth in (0, 1):
        for hevc in (0, 1):
            output = [0, 0, 0, depth if hevc else 0]
            check(old, 0x4E563132, 2, depth, hevc, [0] * 7, output)
            check(new, 0x4E563132, 2, depth, hevc, [0, 0, 0, 0, 0, 1, 1], output)
            check(new, 0x78343230, 2, depth, hevc, [0, 0, 0, 0, 0, 1, 1], output)
            for fmt, words, rgb in (
                (0x4E563132, [0] * 7, 0),
                (0x61726762, [0, 1, 0, 1, 0, 0, 4], 1),
                (0x6C313072, [0, 1, 0, 1, 0, 1, 5], 1),
            ):
                for fn in (old, new):
                    check(fn, fmt, 4 if rgb else 1, depth, hevc, words, [0, 0, rgb, depth if hevc else 0])

    # Execute the exact original/patched signed-offset loads while preserving
    # r15 according to the ABI. Negative and positive slice deltas become zero;
    # this does not touch the source structure or the PPS base offsets.
    source = (ctypes.c_ubyte * 0x800)()
    for _, _, find_hex, replace_hex in chroma_sites:
        for value in (-12, -3, 0, 3, 12):
            struct.pack_into("<ii", source, 0x784, value, value)
            before = bytes(source)
            for code, expected in ((find_hex, value), (replace_hex, 0)):
                region = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
                raw = bytes.fromhex("41574989ff") + bytes.fromhex(code) + bytes.fromhex("89f0415fc3")
                region[:len(raw)] = raw
                function = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p)(
                    ctypes.addressof(ctypes.c_char.from_buffer(region)))
                assert function(ctypes.addressof(source)) == expected
                assert bytes(source) == before
                region.close()
                checks += 1
    assert caller[2:] == ("4c89ee", "4c89fe")
    print(f"PASS {checks} real instruction cases: P010/NV12/RGB, AVC/HEVC, signed chroma deltas; no GPU work")


if __name__ == "__main__":
    main()
