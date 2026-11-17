#!/usr/bin/env python3
"""Replay the AVC picture-count check from the actual bundled VCN firmware.

This is a small, fail-closed interpreter for the six standard Xtensa
instructions in that check, NOT an emulator of VCN or its custom instructions.
The inputs are macroblock dimensions and the slice index at this basic block.
Passing the block does not establish that the rest of a stream can be decoded.
No decoder is created and no GPU command is submitted.
"""
import argparse
import hashlib
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
ADDRESS_DELTA = 0x5FFFFF00
BUNDLED_SHA256 = "bb7833d842fd19f08d1087c96121b6fc501506762855bc6895e9fcd3ee711668"
LINUX_SHA256 = "5b32a9f1ee1176babe56910fa1fa52df938b60665febbeab1ce0ee38cb4d5c98"

# (start, accept, reject, count register, height register, index register,
#  context-base register, first count offset, exact instruction bytes)
CHECKS = {
    "bundled DEC 6 / ENC 1.20": (
        0x60021D44, 0x60021D59, 0x60021A42, 11, 9, 10, 13, 0x162,
        "90bbc1b25db1b0b0f400cba1c25db2b73a02063aff",
    ),
    "Linux DEC 8 / ENC 1.24": (
        0x60021FDB, 0x60021FF0, 0x600214DC, 10, 9, 8, 12, 0x15E,
        "90aac1a25cafa0a0f400baa1b25cb0a73802c63afd",
    ),
}


def read_at(firmware, address, count):
    offset = address - ADDRESS_DELTA
    assert 0 <= offset <= len(firmware) - count
    return firmware[offset:offset + count]


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def replay_create_dispatch(firmware, codec):
    """Execute the codec comparison ladder, after its input load.

    The caller has loaded the create-message codec into a13 and 17 into
    a14. Stop at the selected handler: do not execute it or emulate hardware.
    In particular, the old codec-0 constant does not select an AVC handler.
    """
    start = 0x60015B11
    code = bytes.fromhex(
        "0b9d16a92b82cdfe16182d92cdfd16a92dc2cdfc164c31f2cdf9162f1f"
        "82cdf016f81fe09dc016c920c11ce2c71d02060afe")
    assert read_at(firmware, start, len(code)) == code
    # These create-message paths establish the comparison value a14 = 17.
    assert read_at(firmware, 0x60015591, 2) == bytes.fromhex("1c1e")
    assert read_at(firmware, 0x60015BFD, 2) == bytes.fromhex("1c1e")
    targets = {
        0x60015DD1, 0x60015DEE, 0x60015DFD, 0x60015E3D,
        0x60015D21, 0x60015D34, 0x60015D47, 0x60015B43, 0x6001536C,
    }
    registers = [None] * 16
    registers[13], registers[14] = codec, 17
    pc = start
    for _ in range(32):
        if pc in targets:
            return pc
        assert start <= pc < start + len(code), hex(pc)
        word = int.from_bytes(read_at(firmware, pc, 3), "little")
        t, s, r = (word >> 4) & 15, (word >> 8) & 15, (word >> 12) & 15
        next_pc = pc + 3
        if word & 15 == 11:  # ADDI.N ar, as, imm4; 0 encodes -1
            registers[r] = (registers[s] + (t or -1)) & 0xFFFFFFFF
            next_pc = pc + 2
        elif word & 0xF00F == 0xC002:  # ADDI at, as, signed imm8
            registers[t] = (registers[s] + signed(word >> 16, 8)) & 0xFFFFFFFF
        elif word & 0xFF000F == 0xC00000:  # SUB ar, as, at
            registers[r] = (registers[s] - registers[t]) & 0xFFFFFFFF
        elif word & 15 == 1:  # L32R at, aligned PC + signed imm16*4
            address = ((pc + 3) & ~3) + signed(word >> 8, 16) * 4
            registers[t] = int.from_bytes(read_at(firmware, address, 4), "little")
        elif word & 0xFF == 0x16:  # BEQZ as, pc+4+signed imm12
            if registers[s] == 0:
                next_pc = pc + 4 + signed(word >> 12, 12)
        elif word & 0xF00F == 0x1007:  # BEQ as, at, pc+4+signed imm8
            if registers[s] == registers[t]:
                next_pc = pc + 4 + signed(word >> 16, 8)
        elif word & 0x3F == 0x06:  # J pc+4+signed imm18
            next_pc = pc + 4 + signed(word >> 6, 18)
        else:
            raise AssertionError(f"unmodelled dispatch instruction {pc:#x}: {word:06x}")
        pc = next_pc
    raise AssertionError("codec dispatch did not terminate within its instruction bound")


def verify_create_dispatch(firmware):
    expected = {
        1: 0x60015DD1, 2: 0x60015DEE, 3: 0x60015DFD, 4: 0x60015E3D,
        7: 0x60015D21, 16: 0x60015D34, 17: 0x60015D47, 0x80000003: 0x60015B43,
    }
    # All ordinary IDs through 255 plus the special MPEG4 ID and UINT32_MAX.
    for codec in (*range(256), 0x80000003, 0xFFFFFFFF):
        assert replay_create_dispatch(firmware, codec) == expected.get(codec, 0x6001536C)
    print("PASS 258 byte-derived create-dispatch checks; codec 0 selects no AVC handler")


def replay(firmware, spec, width_mbs, frame_height_mbs, index, field=0):
    start, accept, reject, count_reg, height_reg, index_reg, base_reg, _, code = spec
    expected = bytes.fromhex(code)
    assert read_at(firmware, start, len(expected)) == expected, hex(start)
    assert field in (0, 1)
    registers = [0] * 16
    registers[count_reg] = width_mbs
    registers[height_reg] = frame_height_mbs >> field
    registers[index_reg] = index
    registers[base_reg] = 0x1000
    stores = {}
    pc = start
    while pc not in (accept, reject):
        assert start <= pc < start + len(expected), hex(pc)
        word = int.from_bytes(read_at(firmware, pc, 3), "little")
        t, s, r = (word >> 4) & 15, (word >> 8) & 15, (word >> 12) & 15
        next_pc = pc + 3
        if word & 0xFF000F == 0xC10000:  # MUL16U ar, as, at
            registers[r] = (registers[s] & 0xFFFF) * (registers[t] & 0xFFFF)
        elif word & 0xF00F == 0x5002:  # S16I at, as, imm8*2
            stores[registers[s] + (word >> 16) * 2] = registers[t] & 0xFFFF
        elif word & 0x0E000F == 0x040000:  # EXTUI ar, at, shift, mask+1
            shift = s + ((word >> 16) & 1) * 16
            bits = (word >> 20) + 1
            registers[r] = (registers[t] >> shift) & ((1 << bits) - 1)
        elif word & 0xFF00FF == 0xA10000:  # SLL ar, as; preceding SSL field
            registers[r] = (registers[s] << field) & 0xFFFFFFFF
        elif word & 0xF00F == 0x3007:  # BLTU as, at, pc+4+imm8
            if registers[s] < registers[t]:
                next_pc = pc + 4 + signed(word >> 16, 8)
        elif word & 0x3F == 0x06:  # J pc+4+signed imm18
            next_pc = pc + 4 + signed(word >> 6, 18)
        else:
            raise AssertionError(f"unmodelled instruction {pc:#x}: {word:06x}")
        pc = next_pc
    return pc == accept, registers[count_reg], stores


def verify(firmware, label):
    spec = CHECKS[label]
    checks = 0
    # Both orientations, native maximum, and the exact failing corner.
    for width, height in ((120, 68), (240, 135), (256, 144),
                          (144, 256), (255, 256), (256, 255), (256, 256)):
        full_count = width * height
        count = full_count & 0xFFFF
        for index in (0, 1, count - 1 if count else 65535, full_count, 0xFFFFFFFF):
            accepted, actual_count, stores = replay(firmware, spec, width, height, index)
            assert accepted == (index < count), (label, width, height, index)
            assert actual_count == count
            assert stores == {0x1000 + spec[7]: count, 0x1000 + spec[7] + 2: count}
            checks += 1
    # Every possible valid first_mb_in_slice in the 65,536-MB progressive
    # picture is rejected. This proves a whole-picture limit, independent of
    # the number of slices or the particular slice start.
    for index in range(65536):
        accepted, count, _ = replay(firmware, spec, 256, 256, index)
        assert not accepted and count == 0
        checks += 1
    # A field picture is different: its picture count is 32,768, while the
    # frame-count store still wraps. Do not generalize the progressive
    # rejection into a claim that this test simulates interlaced decoding.
    accepted, count, stores = replay(firmware, spec, 256, 256, 0, field=1)
    assert accepted and count == 32768
    assert stores == {0x1000 + spec[7]: 32768, 0x1000 + spec[7] + 2: 0}
    checks += 1
    print(f"PASS {label}: {checks} byte-derived picture-count checks; no GPU work")
    return checks


def verify_captured_message(path, firmware, expected_dimensions):
    """Read real builder messages; never create a decoder to obtain them."""
    raw = path.read_bytes()
    header_size, total_size, count, message_type = struct.unpack_from("<4I", raw)
    assert header_size == 40 and total_size <= len(raw) and message_type == 1
    assert 24 + 16 * count <= total_size
    messages = {}
    for i in range(count):
        message_id, offset, size, _ = struct.unpack_from("<4I", raw, 24 + 16 * i)
        assert 24 + 16 * count <= offset <= total_size - size
        assert message_id not in messages
        messages[message_id] = raw[offset:offset + size]
    codec, _, width, height = struct.unpack_from("<4I", messages[2])
    assert codec == 7 and (width, height) == expected_dimensions
    flags = struct.unpack_from("<I", messages[6], 8)[0]
    # AMD's rvcn_dec_message_avc_t defines frame_mbs_only_flag at bit 2.
    # This is an input-format assertion, not emulation of custom bit parsing.
    assert flags & (1 << 2), "the progressive argument requires frame_mbs_only"
    assert width % 16 == height % 16 == 0
    accepted, count, _ = replay(
        firmware, CHECKS["bundled DEC 6 / ENC 1.20"], width // 16, height // 16, 0)
    assert accepted == (expected_dimensions != (4096, 4096))
    print(f"PASS captured {path.name}: progressive {width}x{height}, "
          f"firmware count {count}, first slice {'accepted' if accepted else 'rejected'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-firmware", type=Path,
                        help="optional pinned renoir_vcn.bin with its Linux outer header")
    parser.add_argument("--captured-messages", type=Path,
                        help="optional directory containing the saved square/portrait messages")
    args = parser.parse_args()
    firmware = (ROOT / "NootedRed/Firmware/ativvaxy_nv.dat").read_bytes()
    assert hashlib.sha256(firmware).hexdigest() == BUNDLED_SHA256
    assert struct.unpack_from("<I", firmware, 0x60)[0] == 0x06114007
    # Check the codec-7 dispatcher, installed AVC callbacks, and the failure
    # epilogue. These byte guards keep the arithmetic evidence tied to this
    # firmware revision; they do not emulate the intervening custom opcodes.
    for address, data in (
        (0x60015D2E, "658804"),                 # call8 0x6001a5b4
        (0x6000E52C, "c8ac0160"),               # parser callback 0x6001acc8
        (0x6001A65D, "c262a7"),                 # s32i a12, a2, 0x29c
        (0x60017BB6, "8222a7"),                 # load installed parser callback
        (0x60017BCE, "e00800"),                 # callx8 a8
        (0x6001AB28, "a58b01"),                 # call8 NAL dispatcher 0x6001c3e4
        (0x60001414, "f9c40160"),               # NAL type 1 handler
        (0x60001424, "f9c40160"),               # NAL type 5 handler
        (0x6001C51F, "e51f00"),                 # call8 slice wrapper 0x6001c71c
        # Input frame_mbs_only_flag (bit 2) becomes SPS bit 9 via DEPBITS.
        (0x6001B38E, "8826c985808241b0089ab9e5"),
        # Clear field flag; progressive SPS bit skips field-bit parsing.
        (0x60021450, "324ad5324ad698e9f2a10297e94d"),
        (0x6001B54A, "c02fc1225ee0"),           # MUL16U then 16-bit frame count
        (0x6001C735, "25a804"),                 # call8 slice parser 0x600211b8
        (0x60021A42, "8221140c122248eb0c621df0"),  # error flag; return 6
    ):
        assert read_at(firmware, address, len(bytes.fromhex(data))) == bytes.fromhex(data)
    verify_create_dispatch(firmware)
    verify(firmware, "bundled DEC 6 / ENC 1.20")
    if args.captured_messages:
        for name, dimensions in (
            ("high-square-final-msg-1.bin", (4096, 4096)),
            ("high-portrait-final-msg-1.bin", (4080, 4096)),
        ):
            verify_captured_message(args.captured_messages / name, firmware, dimensions)
    if args.linux_firmware:
        raw = args.linux_firmware.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == LINUX_SHA256
        size, offset = struct.unpack_from("<II", raw, 20)
        assert offset == 256 and offset + size == len(raw)
        verify(raw[offset:], "Linux DEC 8 / ENC 1.24")


if __name__ == "__main__":
    main()
