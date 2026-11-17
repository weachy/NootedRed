#!/usr/bin/env python3
"""Execute the actual reviewed x86_64 HEVC helper with a mock packet writer."""
import ctypes
import mmap
import struct

from hevc_bitstream_patch import BUILDER, PACKET_WRITER, patches, rel32
from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH


def main():
    with DyldCache(DEFAULT_CACHE_DIR) as cache:
        image = MachOImage(cache, VA_IMAGE_PATH)
        builder = image.symbol(BUILDER)
        writer = image.symbol(PACKET_WRITER)
        original = patches(builder, writer)
        for address, before, after in original:
            # The running revision-9 kext may already have patched a live
            # cache page. The manifest accepts precisely these two images.
            assert cache.read(address, len(before)) in (before, after)

    region = mmap.mmap(-1, 0x30000, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    base = ctypes.addressof(ctypes.c_char.from_buffer(region))
    runtime_builder = base + 0x2000
    slide = runtime_builder - builder
    runtime_writer = writer + slide
    runtime_sites = patches(runtime_builder, runtime_writer, slide=slide)
    for address, _, after in runtime_sites:
        offset = address-base
        assert 0 <= offset and offset+len(after) <= len(region)
        region[offset:offset+len(after)] = after

    # The original writer in this CPU test records the descriptor it sees,
    # returns the native 28-byte packet size and preserves all callee-saved
    # registers. This tests both the 64 MiB temporary value and restoration.
    writer_code = bytes.fromhex("8b83380400008906b81c000000c3")
    at = runtime_writer-base
    region[at:at+len(writer_code)] = writer_code
    callsite = runtime_sites[0][0]
    region[callsite+5-base:callsite+7-base] = bytes.fromhex("5bc3")
    wrapper = base+0x100
    # push rbx; mov rbx,rdi; jmp HEVC builder call site.
    wrapper_code = bytes.fromhex("534889fb") + b"\xe9" + rel32(wrapper+9, callsite)
    region[wrapper-base:wrapper-base+len(wrapper_code)] = wrapper_code
    fn = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)(wrapper)
    command = (ctypes.c_ubyte*0x900)()
    seen = ctypes.c_uint32()
    for native in (0x100000, 0x4000000, 0x4800000, 0x7f00000, 0x8000000):
        ctypes.memset(command, 0x5a, len(command))
        struct.pack_into("<I", command, 0x438, native)
        seen.value = 0
        result = fn(ctypes.addressof(command), ctypes.addressof(seen))
        expected = 0x4000000 if native == 0x8000000 else native
        assert result == 28
        assert seen.value == expected, (hex(native), hex(seen.value))
        assert struct.unpack_from("<I", command, 0x438)[0] == native
        assert command[0x437] == command[0x43c] == 0x5a
    region.close()
    print("PASS 5 executed HEVC packet/restore cases, exact cache sites and surrounding canaries; no GPU work")


if __name__ == "__main__":
    main()
