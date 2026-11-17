#!/usr/bin/env python3
"""Build the reviewed HEVC-only VCN2 bitstream descriptor workaround.

The replacement call in Vcn2EncHevcCommand::buildGeneralCommand reaches
short helpers in zero alignment padding inside the executable __text section.
Only a 128 MiB descriptor is changed to 64 MiB while the packet is
serialized; the command object's native allocation size is restored before
returning. Other sizes and AVC use the original descriptor and call.
"""
import struct

BUILDER = "__ZN18Vcn2EncHevcCommand19buildGeneralCommandEP11_UveCmdInfo"
PACKET_WRITER = "__ZN14Vcn2EncCommand24addBitstreamBufferPacketEP11_UveCmdInfo"
CALL_OFFSET = 0x21F
CAVES = (
    (0x7FFB10B63811, 15),
    (0x7FFB10B60903, 13),
    (0x7FFB10B5FCB4, 12),
    (0x7FFB10B60AD6, 10),
    (0x7FFB10B678E6, 10),
)
ORIGINAL_CALL = bytes.fromhex("e868f6ffff")
NATIVE_BYTES = 0x8000000
FIRMWARE_BYTES = 0x4000000


def rel32(source_after_instruction, target):
    return struct.pack("<i", target - source_after_instruction)


def patches(builder_address, packet_writer_address, *, slide=0):
    site = builder_address + CALL_OFFSET
    assert site + 5 + struct.unpack("<i", ORIGINAL_CALL[1:])[0] == packet_writer_address
    caves = tuple((address+slide, length) for address, length in CAVES)
    # The high byte of the native 128 MiB descriptor is 8. SETE produces
    # 0 or 1, and the saved AL is restored after the original packet writer.
    # One stack push also supplies the x86_64 call alignment.
    parts = (
        bytes.fromhex("80bb3b040000080f94c0") + b"\xe9" + rel32(caves[0][0]+15, caves[1][0]),
        bytes.fromhex("c0e00250") + b"\xe9" + rel32(caves[1][0]+9, caves[2][0]),
        bytes.fromhex("28833b040000") + b"\xe9" + rel32(caves[2][0]+11, caves[3][0]),
        b"\xe8" + rel32(caves[3][0]+5, packet_writer_address) +
            b"\xe9" + rel32(caves[3][0]+10, caves[4][0]),
        bytes.fromhex("59008b3b040000c3"),
    )
    assert all(len(part) <= length for part, (_, length) in zip(parts, caves))
    call = b"\xe8" + rel32(site + 5, caves[0][0])
    assert len(call) == len(ORIGINAL_CALL)
    return ((site, ORIGINAL_CALL, call), *(
        (address, bytes(length), part + bytes(length-len(part)))
        for (address, length), part in zip(caves, parts)
    ))


if __name__ == "__main__":
    from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        image = MachOImage(cache, VA_IMAGE_PATH)
        builder = image.symbol(BUILDER)
        writer = image.symbol(PACKET_WRITER)
        sites = patches(builder, writer)
        for address, before, after in sites:
            assert cache.read(address, len(before)) in (before, after)
            assert len(before) == len(after)
            if address != sites[0][0]:
                assert image.text[2] <= address
                assert address + len(before) <= image.text[2] + image.text[3]
        print("VA UUID", image.uuid.hex())
        for address, before, after in sites:
            print(f"{address:#x} {before.hex()} -> {after.hex()}")
