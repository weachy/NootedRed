#!/usr/bin/env python3
"""Replay the reviewed GVA setter and native slice validator; no GPU work.

Use real CoreFoundation numbers/conversions. Only mutexes and optional logging
are stubbed. Replay the actual native validator after the setter, as native
encoder initialization does, including its row/256 bounds. Also check that
runtime requests fail without changing the saved configuration.
"""
import ctypes as C
import mmap
import struct

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache
from media_patch_manifest import GVA_ENCODE_SITES

SETTER = 0x7FFA0C5D6650
VALIDATOR = 0x7FFA0C5F49EE
CF = C.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
CF.CFNumberCreate.argtypes = (C.c_void_p, C.c_int, C.c_void_p)
CF.CFNumberCreate.restype = C.c_void_p
CF.CFRelease.argtypes = (C.c_void_p,)


def main():
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        original = bytearray(cache.read(SETTER, 0x1500))
        validator = bytearray(cache.read(VALIDATOR, 0x60))
        properties = {}
        for table, first, count in ((0x7FFA0C5D79FC, 0x76613030, 10),
                                    (0x7FFA0C5D7A24, 0x76613130, 10),
                                    (0x7FFA0C5D7A4C, 0x76613231, 9)):
            for i, delta in enumerate(struct.unpack(
                    "<" + "i" * count, cache.read(table, count * 4))):
                properties[table + delta] = first + i
    patched = bytearray(original)
    for _, address, find, replace in GVA_ENCODE_SITES:
        offset = address - SETTER
        find, replace = bytes.fromhex(find), bytes.fromhex(replace)
        assert original[offset:offset + len(find)] in (find, replace)
        original[offset:offset + len(find)] = find
        patched[offset:offset + len(find)] = replace

    # Keep callbacks and executable regions alive until all calls return.
    regions = []
    glob = (C.c_ubyte * 0x11840)()
    wrapper = (C.c_ubyte * 0x3B0)()
    encoder = (C.c_ubyte * 0x800)()
    glob_address, encoder_address = C.addressof(glob), C.addressof(encoder)
    struct.pack_into("<QQ", glob, 0x48, 1, C.addressof(wrapper))
    struct.pack_into("<Q", wrapper, 0x3A8, encoder_address)

    def region(code):
        memory = mmap.mmap(-1, 0x4000,
                           prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        memory[:len(code)] = code
        regions.append(memory)
        return memory, C.addressof(C.c_char.from_buffer(memory))

    def call(memory, base, offset, target):
        assert memory[offset] == 0xE8
        struct.pack_into("<i", memory, offset + 1, target - (base + offset + 5))

    def thunk(memory, base, offset, target):
        memory[offset:offset + 12] = b"\x48\xb8" + struct.pack("<Q", target) + b"\xff\xe0"
        return base + offset

    validator_memory, validator_base = region(validator)
    validator_memory[0x100:0x103] = b"\x31\xc0\xc3"  # optional printf
    call(validator_memory, validator_base, 0x54, validator_base + 0x100)
    validate = C.CFUNCTYPE(C.c_int, C.c_void_p)(validator_base)

    def executable(code):
        memory, base = region(code)
        memory[0x1800:0x1803] = b"\x31\xc0\xc3"
        # Original SetProperty mutex lock/unlock; all relative branches stay intact.
        for offset in (0x20, 0xA9):
            call(memory, base, offset, base + 0x1800)
        targets = {
            0x7FFA0C641CFA: CF.CFNumberGetTypeID,
            0x7FFA0C641CEE: CF.CFGetTypeID,
            0x7FFA0C641D00: CF.CFNumberGetValue,
        }
        # These are reviewed instruction boundaries, not byte-pattern scans.
        offsets = [0x50C, 0x517]
        if code == patched:
            offsets += [0x4F5, 0x532]
        else:
            offsets += [0x537]
        for index, offset in enumerate(offsets):
            destination = SETTER + offset + 5 + struct.unpack_from("<i", memory, offset + 1)[0]
            target = C.cast(targets[destination], C.c_void_p).value
            local = thunk(memory, base, 0x1900 + index * 16, target)
            call(memory, base, offset, local)
        return C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint, C.c_void_p)(base)

    old, new = executable(original), executable(patched)
    count_property = properties[GVA_ENCODE_SITES[0][1]]
    duration_property = properties[0x7FFA0C5D6B53]
    checks = 0
    for height in (64, 1088, 2304, 4096, 8192):
        struct.pack_into("<I", encoder, 0x7CC, height)
        for request in (0, 1, 2, 4, 6, 68, 144, 256, 257, -1, 1000000):
            raw = C.c_int64(request)
            number = CF.CFNumberCreate(None, 4, C.byref(raw))
            try:
                struct.pack_into("<I", glob, 0x10E68, 99)
                assert old(glob_address, count_property, number) == 0
                assert struct.unpack_from("<I", glob, 0x10E68)[0] == 99
                assert new(glob_address, count_property, number) == 0
                assert struct.unpack_from("<i", glob, 0x10E68)[0] == request
                # Native init replays this field through SetParameter(8).
                struct.pack_into("<I", encoder, 0x7D0,
                                 struct.unpack_from("<I", glob, 0x10E68)[0])
                status = validate(encoder_address)
                maximum = min((height + 15) // 16, 256)
                expected = 1 if request == 0 else min(request & 0xFFFFFFFF, maximum)
                assert struct.unpack_from("<I", encoder, 0x7D0)[0] == expected
                assert status == (7 if expected != request else 0)
                checks += 1
            finally:
                CF.CFRelease(number)
    # Failed int32 conversion must not mutate the configuration.
    raw = C.c_int64(1 << 62)
    number = CF.CFNumberCreate(None, 4, C.byref(raw))
    try:
        struct.pack_into("<I", glob, 0x10E68, 4)
        assert new(glob_address, count_property, number) == -12915
        assert struct.unpack_from("<I", glob, 0x10E68)[0] == 4
        checks += 1
    finally:
        CF.CFRelease(number)
    struct.pack_into("<Q", wrapper, 0x3A8, 0)
    raw = C.c_int64(6)
    number = CF.CFNumberCreate(None, 4, C.byref(raw))
    try:
        assert new(glob_address, count_property, number) == 0
        assert struct.unpack_from("<I", glob, 0x10E68)[0] == 6
        checks += 1
    finally:
        CF.CFRelease(number)
    struct.pack_into("<Q", wrapper, 0x3A8, encoder_address)
    struct.pack_into("<I", encoder, 0x38, 1)
    for request in (0, 1, 2, 4, 256, -1):
        raw = C.c_int64(request)
        number = CF.CFNumberCreate(None, 4, C.byref(raw))
        try:
            struct.pack_into("<I", glob, 0x10E68, 6)
            assert new(glob_address, count_property, number) == -12915
            assert struct.unpack_from("<I", glob, 0x10E68)[0] == 6
            checks += 1
        finally:
            CF.CFRelease(number)
    # The adjacent property's value and update flag must retain their semantics.
    for value in (0.0, 0.5, 2.25, 60.0):
        raw = C.c_double(value)
        number = CF.CFNumberCreate(None, 13, C.byref(raw))
        try:
            for function in (old, new):
                glob[0x1182A] = 0
                assert function(glob_address, duration_property, number) == 0
                assert struct.unpack_from("<d", glob, 0x105D8)[0] == value
                assert glob[0x1182A] == 1
                checks += 1
        finally:
            CF.CFRelease(number)
    for memory in reversed(regions):
        memory.close()
    print(f"PASS {checks} actual GVA setter/native slice-bound checks; no GPU work")


if __name__ == "__main__":
    main()
