#!/usr/bin/env python3
"""Run the 25G332 VCN frame initializers and commits in Unicorn with a memory-only TTL queue."""

import argparse
from pathlib import Path
import struct
import uuid

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_64
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDI, UC_X86_REG_RDX, UC_X86_REG_RIP,
    UC_X86_REG_RSI, UC_X86_REG_RSP, UC_X86_REG_R8,
)

import gen_vcn_kernel_data as kcdata


def check_engine_ownership(producer, consumer, bias):
    hardware_init = producer.symbols["__ZN26AMDRadeonX5000_AMDHardware4initEP11IOPCIDevice"
                                     "P28AMDRadeonX5000_IAMDHWHandlerRjP16_GART_PARAMETERSP14_FB_PARAMETERS"]
    assert producer.symbols[kcdata.TABLE_5000]
    assert producer.read(hardware_init + 0x1B, 6) == bytes.fromhex("4989f5 4989fe")
    assert producer.read(hardware_init + 0x9E, 4) == bytes.fromhex("4d896e10")
    channel_init = consumer.symbols["__ZN27AMDRadeonX6000_AMDHWChannel4initEiP30AMDRadeonX6000_IAMDHWInterface"
                                    "P27AMDRadeonX6000_IAMDHWEngineP25AMDRadeonX6000_IAMDHWRingPKc"]
    assert consumer.read(channel_init + 0x18, 4) == bytes.fromhex("48894dd0")
    assert consumer.read(channel_init + 0x73, 8) == bytes.fromhex("488b45d0 48894328")

    engine = "__ZN30AMDRadeonX6000_AMDVCN2HWEngine"
    init = consumer.symbols[engine + "4initEP30AMDRadeonX6000_IAMDHWInterface20_eAMD_HW_ENGINE_TYPE"]
    assert consumer.read(init, 15) == bytes.fromhex("554889e5 4157 4156 53 50 31c0 8945e4")
    address = consumer.symbols[engine + "4freeEv"]
    for parent in ("__ZN29AMDRadeonX6000_AMDVCNHWEngine", "__ZN26AMDRadeonX6000_AMDHWEngine"):
        body = consumer.read(address, 18)
        assert body[:7] == bytes.fromhex("554889e5 488d05") and body[11:] == bytes.fromhex("5d ffa0a0000000")
        table = address + 11 + struct.unpack_from("<i", body, 7)[0]
        address = consumer.read_u64(table + 0xA0) - bias
        assert address == consumer.symbols[parent + "4freeEv"]
    assert consumer.read(address, 18) == bytes.fromhex("554889e5 53 50 4889fb 488b07 ff9050020000")
    print("X5000 PCI owner, X6000 channel owner and VCN2 engine lifetime verified")


def check(kc_path, output):
    collection = kcdata.KernelCollection(kc_path)
    producer = collection.kexts[kcdata.X5000_ID]
    consumer = collection.kexts[kcdata.X6000_ID]
    libs_id = kcdata.X5000_ID + "HWLibs"
    libs = kcdata.KextSlice(collection, libs_id, *collection.fileset_entries[libs_id])
    for image, expected in ((producer, "E27EFB64-DE35-3CE5-B3AB-A32218B495D3"),
                            (consumer, "1540D089-EE2D-3339-B5A8-2024F9E38581"),
                            (libs, "98779CEA-E395-3829-A5B9-FEA1CA9E11B4")):
        if image.uuid != uuid.UUID(expected).bytes:
            raise ValueError(f"Unsupported {image.kext_id} UUID {uuid.UUID(bytes=image.uuid)}")
    replay = producer.symbols["__ZN30AMDRadeonX5000_AMDAccelChannel13replayBuffersEP11IOAccelTask"]
    assert producer.read(replay + 0x261, 37) == bytes.fromhex(
        "c7840504ffffff00000000 4883c028 4939c7 75ec 808db8feffff02 c785bcfeffff00000000")
    schedule = producer.symbols["__ZN29AMDRadeonX5000_AMDSWScheduler7advanceERNS_12HwChannelCtxE"]
    assert producer.read(schedule + 0x418, 32) == bytes.fromhex(
        "4189742478 4983c460 488b45b8 488b7808 488b07 4c89e6 4589dc ff9088010000")
    print("X5000 discard lengths, flags, VMID and scheduler stamp verified")

    machine = Uc(UC_ARCH_X86, UC_MODE_64)
    for image in (consumer, libs):
        for section in kcdata.SEGMENT_LIST:
            base, _, offset, size = image.segments[section]
            machine.mem_map(base, (size + 4095) & ~4095)
            machine.mem_write(base, collection.data[offset:offset + size])
    scratch = 0x20000000
    machine.mem_map(scratch, 0x10000)
    machine.mem_map(0x10000, 0x1000)
    channel, vtable, ttl, ttl_table, queue, frame, job = [scratch + 0x1000 * index for index in range(1, 8)]
    device, queues = scratch + 0x8000, scratch + 0x9000
    submit, fail_submit, stop = [scratch + offset for offset in (0x100, 0x110, 0x130)]
    fence_address, ib_address, stamp = 0xF123456000, 0x40000C000, 0x97E
    native_stamps = []
    bias = kcdata.derive_prelink_bias(collection)
    check_engine_ownership(producer, consumer, bias)
    read = machine.reg_read
    write = machine.reg_write

    def pointer(address, value):
        machine.mem_write(address, struct.pack("<Q", value))

    def word(address, value):
        machine.mem_write(address, struct.pack("<I", value))

    def run(address, *args):
        stack = scratch + 0xFFF8
        pointer(stack, stop)
        write(UC_X86_REG_RSP, stack)
        for register, value in zip((UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX, UC_X86_REG_RCX, UC_X86_REG_R8), args):
            write(register, value)
        machine.emu_start(address, stop, count=5000)
        assert read(UC_X86_REG_RIP) == stop, "native method exceeded its instruction bound"
        return read(UC_X86_REG_RAX) & 0xFFFFFFFF

    def redirect(address, callback):
        machine.mem_write(address, b"\xc3")
        machine.hook_add(UC_HOOK_CODE, lambda *_: callback(), begin=address, end=address)

    def copy_bytes():
        destination, source, count = read(UC_X86_REG_RDI), read(UC_X86_REG_RSI), read(UC_X86_REG_RDX)
        machine.mem_write(destination, bytes(machine.mem_read(source, count)))

    pointer(channel, vtable)
    pointer(channel + 0x140, ttl)
    pointer(channel + 0xC8, fence_address)
    pointer(ttl, ttl_table)
    pointer(ttl + 0x38, device)
    pointer(device + 0x18, queues)
    redirect(0x1020A, lambda: write(UC_X86_REG_RAX, frame))
    redirect(0x100B4, lambda: machine.mem_write(read(UC_X86_REG_RDI), bytes(read(UC_X86_REG_RSI))))
    redirect(0x100E4, copy_bytes)
    redirect(libs.symbols["_internal_cgs_read_register"], lambda: write(UC_X86_REG_RAX, 0x1234))
    ttl_name = "__ZN14AmdTtlServices16queryCommandInfoE19AMD_SWIP_QUEUE_TYPEj20AMD_TTL_COMMAND_TYPEP21_TtlCommandInfoOutput"
    ttl_query = libs.symbols[ttl_name]
    ttl_dispatch = libs.symbols["__ZTV14AmdTtlServices"] + 16
    for slot, method in ((0x90, "queryCommandInfo"), (0x98, "buildCommand"), (0xA0, "submitFrame")):
        address = libs.read_u64(ttl_dispatch + slot) - bias
        assert method in libs.symbol_at(address)
        pointer(ttl_table + slot, address)
    for slot, entry in ((0x6A8, "_queue_query_command_info"), (0x6B0, "_queue_build_command"),
                        (0x6B8, "_queue_submit_frame")):
        pointer(ttl + slot, libs.symbols[entry])

    def enqueue():
        assert read(UC_X86_REG_RDI) == queue
        native_stamps.append(bytes(machine.mem_read(read(UC_X86_REG_RSI), size)))

    redirect(submit, enqueue)
    redirect(fail_submit, lambda: write(UC_X86_REG_RAX, 1))
    for index, name in enumerate(("Dec", "Enc", "EncLLQ")):
        channel_type = f"AMDRadeonX6000_AMDVCN2{name}Channel"
        table = consumer.symbols[f"__ZTV{len(channel_type)}{channel_type}"] + 16
        for slot in range(0x338, 0x370, 8):
            pointer(vtable + slot, consumer.read_u64(table + slot) - bias)
        initializer = consumer.read_u64(table + 0x368) - bias
        commit = consumer.read_u64(table + 0x2F8) - bias
        prefix = "_queue_" + ("encode" if index else "decode") + "_2_0_"
        queue_type = (1, 3, 4)[index]
        pointer(ttl + 0x978 + index * 32, queue)
        word(queue + 0x10, queue_type)
        pointer(queues + 0x20 + 8 * (queue_type - 1), queue)
        pointer(queue + 0x90, libs.symbols[prefix + "query_command_info"])
        pointer(queue + 0x98, libs.symbols[prefix + "build_command"])
        pointer(queue + 0xA0, submit)
        word(channel + 0x14C, 1 if index else 2)
        word(channel + 0x150, index)
        size = 128 if index else 256
        info = scratch + 0x300
        assert run(ttl_query, ttl, index, 0, 0x14 if index else 1, info) == 0
        assert struct.unpack("<III", machine.mem_read(info, 12))[1] == (20 if index else 56)
        machine.mem_write(frame, b"\xCC" * 256)
        run(initializer, channel)
        assert struct.unpack("<I", machine.mem_read(channel + 0xD8, 4))[0] * 4 == size
        template = bytes(machine.mem_read(frame, size))
        assert template[-4:] == bytes(4)
        (output / f"{name}.bin").write_bytes(template)
        machine.mem_write(job, bytes(0xE8))
        word(job, 2)
        word(job + 0x14, 1)
        word(job + 0x18, stamp)
        native_stamps.clear()
        assert run(commit, channel, job) == stamp and not native_stamps
        word(job, 0)
        assert run(commit, channel, job) == stamp and native_stamps == [template]
        word(job + 4, 8)
        word(job + 0x4C, 19)
        pointer(job + 0x58, ib_address)
        assert run(commit, channel, job) == stamp and len(native_stamps) == 2
        encoded = struct.unpack(f"<{size // 4}I", native_stamps[-1])
        assert encoded[8 if index else 19] == stamp
        assert encoded[1 if index else 5] == 8 and encoded[4 if index else 17] == 19
        (output / f"{name}-single.bin").write_bytes(native_stamps[-1])
        word(job + 0x14, 2)
        word(job + 0x74, 23)
        pointer(job + 0x80, ib_address + 0x1000)
        assert run(commit, channel, job) == stamp and len(native_stamps) == 3
        assert native_stamps[-1] == native_stamps[-2]
        pointer(ttl + 0x6B8, fail_submit)
        native_stamps.clear()
        assert run(libs.read_u64(ttl_dispatch + 0xA0) - bias, ttl, index, 0, frame) != 0
        assert run(commit, channel, job) == stamp and not native_stamps
        pointer(ttl + 0x6B8, libs.symbols["_queue_submit_frame"])
        print(f"{name}: native {size}-byte template, primary selection, empty, discard and failed submit verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="directory for the native reference frames")
    parser.add_argument("--kc", type=Path, default=kcdata.DEFAULT_KC)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    check(args.kc, args.output)
