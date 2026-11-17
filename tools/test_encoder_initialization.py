#!/usr/bin/env python3
"""Replay native VCN2 initialization and HEVC parameter updates on CPU only.

The real constructor, parameter getter/setter, and HEVC SetConfigParam execute
against mock objects. Only the parent constructor, bzero and vtable addresses
are relocated. No VideoToolbox session, IOKit connection or GPU work is created.
"""
import ctypes
import mmap
import struct

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH
from media_patch_manifest import ENCODER_INIT_SITES


def main():
    names = (
        "__ZN14Vcn2EncCommandC2Ev",
        "__ZN14Vcn2EncCommand19setCmdPackageParamsEjPv",
        "__ZN14Vcn2EncCommand19getCmdPackageParamsEjPv",
        "__ZN27AVDVcn2HevcEncDataProcessor14SetConfigParamEjPvS0_",
    )
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        image = MachOImage(cache, VA_IMAGE_PATH)
        code = {}
        for name in names:
            address = image.symbol(name)
            code[name] = bytearray(cache.read(address, image.function_end(address) - address))
    ctor = code[names[0]]
    assert len(ctor) == 94
    original, patched = bytearray(ctor), bytearray(ctor)
    for name, offset, find, replace in ENCODER_INIT_SITES:
        assert name == names[0]
        find, replace = bytes.fromhex(find), bytes.fromhex(replace)
        assert ctor[offset:offset + len(find)] in (find, replace)
        original[offset:offset + len(find)] = find
        patched[offset:offset + len(find)] = replace

    regions = []
    callbacks = []
    zero_calls = []

    @ctypes.CFUNCTYPE(None, ctypes.c_void_p)
    def parent(obj):
        # UveCommand/HWCommand initialize only the base object below +0x38.
        ctypes.memset(obj, 0, 0x34)

    @ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_size_t)
    def zero(obj, length):
        zero_calls.append((obj, length))
        ctypes.memset(obj, 0, length)

    callbacks.extend((parent, zero))

    def executable(raw, signature, constructor=False):
        region = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        regions.append(region)
        data = bytearray(raw)
        if constructor:
            # Calls remain calls: local trampolines preserve the native ABI.
            for offset, stub, callback in ((0x9, 0x800, parent), (0x3D, 0x820, zero)):
                assert data[offset] == 0xE8
                struct.pack_into("<i", data, offset + 1, stub - offset - 5)
                region[stub:stub + 12] = (
                    b"\x48\xb8" + struct.pack("<Q", ctypes.cast(callback, ctypes.c_void_p).value)
                    + b"\xff\xe0")
            assert data[0xE:0x11] == bytes.fromhex("488d05")
            # The constructor only stores the vtable address; it never calls it.
            struct.pack_into("<i", data, 0x11, 0x900 - 0x15)
        region[:len(data)] = data
        return signature(ctypes.addressof(ctypes.c_char.from_buffer(region)))

    ctor_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
    param_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p)
    config_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)
    old, new = (executable(raw, ctor_type, True) for raw in (original, patched))
    setter = executable(code[names[1]], param_type)
    getter = executable(code[names[2]], param_type)
    configure = executable(code[names[3]], config_type)

    # VAUveEncoder's +e8/+f0 virtual methods forward to the real command
    # set/get methods at +70/+78. This mock just makes that forwarding explicit.
    command = (ctypes.c_ubyte * 0xA00)()
    address = ctypes.addressof(command)

    @param_type
    def forward_set(_encoder, key, source):
        setter(address, key, source)

    @param_type
    def forward_get(_encoder, key, dest):
        getter(address, key, dest)

    callbacks.extend((forward_set, forward_get))
    vtable = (ctypes.c_void_p * 31)()
    vtable[0xE8 // 8] = ctypes.cast(forward_set, ctypes.c_void_p).value
    vtable[0xF0 // 8] = ctypes.cast(forward_get, ctypes.c_void_p).value
    encoder = (ctypes.c_ubyte * 0x600)()
    processor = (ctypes.c_ubyte * 0x200)()
    struct.pack_into("<Q", encoder, 0, ctypes.addressof(vtable))
    struct.pack_into("<Q", processor, 0x30, ctypes.addressof(encoder))
    config = (ctypes.c_ubyte * 0x60)()
    packet = (ctypes.c_uint32 * 8)()
    checks = 0

    def require(condition, context):
        nonlocal checks
        assert condition, context
        checks += 1

    for poison in (0xA5, 0x5A, 0xFF, 0):
        results = []
        for constructor, fixed in ((old, False), (new, True)):
            ctypes.memset(address, poison, len(command))
            zero_calls.clear()
            constructor(address)
            require(zero_calls == [(address + (0x80 if fixed else 0xDC),
                                     0x544 if fixed else 0x4E8)], "bzero bounds")
            require(bytes(command[0xDC:0x5C4]) == bytes(0x4E8), "original cleared range")
            require(bytes(command[0x80:0xDC]) == bytes([0 if fixed else poison]) * 0x5C,
                    "previously uninitialized rate/quality records")
            require(bytes(command[0x5C4:0x644]) == bytes([poison]) * 0x80, "upper gap unchanged")
            require(bytes(command[0x660:]) == bytes([poison]) * (len(command) - 0x660),
                    "derived objects and trailing canary untouched")
            results.append(bytes(command))
            getter(address, 8, ctypes.addressof(packet))
            require(list(packet) == [0 if fixed else poison * 0x01010101] * 8, "packet read")
            # This is the normal HEVC rate update: it reads packet 8 first and
            # only replaces min/max QP, max AU size, skip and HRD fields.
            for off, value in ((0x38, 1), (0x3C, 51), (0x40, 1000000),
                               (0x4C, 1), (0x50, 0)):
                struct.pack_into("<I", config, off, value)
            result = configure(ctypes.addressof(processor), 16, ctypes.addressof(config), None)
            require(result == 0, "native HEVC rate update")
            getter(address, 8, ctypes.addressof(packet))
            require(packet[4] == (0 if fixed else poison * 0x01010101),
                    "filler word survives read/modify/write")
            require([packet[i] for i in (1, 2, 3, 5, 6)] == [1, 51, 1000000, 0, 1],
                    "explicit rate fields preserved")
            # An explicit filler-data request must still override the default.
            for filler in (0, 1):
                explicit = (ctypes.c_uint32 * 2)(20, filler)
                result = configure(ctypes.addressof(processor), 15,
                                   ctypes.addressof(explicit), None)
                require(result == 0, "explicit rate config")
                getter(address, 8, ctypes.addressof(packet))
                require(packet[4] == filler, "explicit filler value")
                require(struct.unpack_from("<I", encoder, 0x528)[0] == 20,
                        "native feedback setting")
        require(results[0][8:0x80] == results[1][8:0x80], "base and early records unchanged")
        require(results[0][0xDC:] == results[1][0xDC:], "all bytes above repaired records unchanged")

    for region in regions:
        region.close()
    print(f"PASS {checks} native constructor/HEVC rate-update checks; "
          "dirty-memory failure reproduced and repaired; no GPU work")


if __name__ == "__main__":
    main()
