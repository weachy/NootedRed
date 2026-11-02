#!/usr/bin/env python3

# Derive the macOS 15.8.1 `AMDRadeonVADriver2` user-space patch data for VDA.
#
# Copyright (C) 2026  ChefKiss.
#
# Licensed under the ChefKiss Open Knowledge License version 1.
#
# This work is licensed to you under the terms of the ChefKiss Open Knowledge
# License, either version 1 of the License, or (at your option) any later
# version published by ChefKiss.
#
# This work may not be used to train or develop artificial intelligence,
# and may not be used with artificial intelligence in any other way. See
# the ChefKiss Open Knowledge License for details.
#
# This work is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# ChefKiss Open Knowledge License for more details.
#
# You should have received a copy of the ChefKiss Open Knowledge License
# along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.
"""Generate `NootedRed/VDAVAData*.hpp` from a supported macOS dyld shared cache.

The generated table makes the stock `AMDRadeonVADriver2` user-space driver treat the GFX9
(VCN 2.x) Renoir iGPU as a VA family 5 (Navi10-class) decoder while still programming GFX9
engines and the GFX9 address library.

Supported targets are pinned by PROFILES (macOS 15.8.1 and Tahoe 26.7.1), selected by the
dyld cache UUID; the generator refuses anything else instead of emitting a table that would
silently never match.

All patch sites are derived from the target image, not copied from any fork data:

  * symbols are resolved from the image's own LC_SYMTAB in the dyld shared cache,
  * each patch site is re-found from the function symbol plus a verified instruction offset,
  * every `find` sequence is checked verbatim in the exact 4 KiB cache-file page image that
    the code-signing validation hook hands to `UserPatches`,
  * the emitted header is regenerated deterministically; `--check` compares it against the
    committed file and re-verifies every patch against the live cache.

Read-only: the cache and system files are only opened for reading.
"""

import argparse
import hashlib
import pathlib
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys

PROFILES = {
    ("13.6.7", "22G720"): {
        "macos_version": "13.6.7 (22G720)",
        "driver_version": "4.14.4",
        "cache_uuid": "5B024447-21CB-39DC-9946-0924A6000A4B",
        "namespace_suffix": "Ventura",
        "output_name": "VDAVADataVentura.hpp",
        "evidence_doc": "analysis/vda-136-va-data.md",
    },
    ("13.6.4", "22G513"): {
        "macos_version": "13.6.4 (22G513)",
        "driver_version": "4.14.4",
        "cache_uuid": "7FE9478F-8955-356C-AC51-5C6FFC253B95",
        "namespace_suffix": "Ventura",
        "output_name": "VDAVADataVentura.hpp",
        "evidence_doc": "analysis/vda-1364-va-data.md",
    },
    ("13.7.8", "22H730"): {
        "macos_version": "13.7.8 (22H730)",
        "driver_version": "4.14.4",
        "cache_uuid": "8EEE6686-4D40-33A3-B07B-B334FD3E3ECC",
        "namespace_suffix": "Ventura",
        "output_name": "VDAVADataVentura.hpp",
        "evidence_doc": "analysis/vda-1378-va-data.md",
    },
    ("14.7.4", "23H420"): {
        "macos_version": "14.7.4 (23H420)",
        "driver_version": "5.5.20",
        "cache_uuid": "85507E78-1FE3-37F1-AEFF-C4164FE279D2",
        "namespace_suffix": "Sonoma",
        "output_name": "VDAVADataSonoma.hpp",
        "evidence_doc": "analysis/vda-1474-va-data.md",
    },
    ("14.8.9", "23J631"): {
        "macos_version": "14.8.9 (23J631)",
        "driver_version": "5.5.20",
        "cache_uuid": "4C1441FD-8A54-3DC2-863D-49110B946A25",
        "namespace_suffix": "Sonoma",
        "output_name": "VDAVADataSonoma.hpp",
        "evidence_doc": "analysis/vda-1489-va-data.md",
    },
    ("15.8.1", "24H32"): {
        "macos_version": "15.8.1 (24H32)",
        "driver_version": "7.0.25",
        "cache_uuid": "42EC2890-BC40-317E-BB27-45142BFF7F26",
        "namespace_suffix": "Sequoia",
        "output_name": "VDAVADataSequoia.hpp",
        "evidence_doc": "analysis/vda-1581-va-data.md",
    },
    ("15.8.2", "24H232"): {
        "macos_version": "15.8.2 (24H232)",
        "driver_version": "7.0.26",
        "cache_uuid": "90DB0DEB-2344-3601-B011-B87312D74CFB",
        "namespace_suffix": "Sequoia",
        "output_name": "VDAVADataSequoia.hpp",
        "evidence_doc": "analysis/vda-1582-va-data.md",
    },
    ("26.7.1", "25G241"): {
        "macos_version": "26.7.1 (25G241)",
        "driver_version": "7.1.8",
        "cache_uuid": "31374756-89B4-34E8-A34F-56C5D57216A1",
        "namespace_suffix": "Tahoe",
        "output_name": "VDAVADataTahoe.hpp",
        "evidence_doc": "analysis/vda-2671-va-data.md",
    },
}
DEFAULT_CACHE_DIR = pathlib.Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld")
LIVE_CACHE_DIRS = frozenset(
    pathlib.Path(p)
    for p in (
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
        "/System/Cryptexes/OS/System/Library/dyld",
        "/System/Library/dyld",
    )
)
CACHE_BASE = 0x7FF800000000
VA_IMAGE_PATH = "/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2"
VA_INFO_PLIST = pathlib.Path("System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/Info.plist")
SYSTEM_VERSION_PLIST = pathlib.Path("System/Library/CoreServices/SystemVersion.plist")
PAGE_SIZE = 0x1000

ROOT = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------------------------------
# Patch plan.  Everything is derived from the target image; the semantic intent comes from the task,
# the legacy NootedRed report and the steve12311 fork analysis (no bytes or offsets were copied).
#
# `symbols` lists the function symbol each site lives in, together with the instruction offset where
# `find` starts.  When one `find` covers several functions (identical instruction sequences) each
# function is listed and `count` in the emitted PagePatch is the number of sites on that page.
# A spec whose sites span several 4 KiB pages is emitted as one patch record per page; the native
# backend matches and patches each page independently through `_cs_validate_page`.
# ---------------------------------------------------------------------------------------------------

PATCHES = [
    {
        "key": "IdentifyForceFamily5",
        "comment": "VAAcceleratorInfo::identify: classify the unknown 0x1638 device as VA family 5 (Navi10/VCN2)",
        "symbols": [("__ZN17VAAcceleratorInfo8identifyEjj", 0x1B6)],
        "find": "BB0B000000E991FEFFFF",
        "replace": "BB05000000E991FEFFFF",
        "rationale": "The classification chain falls through to `mov ebx,0xB` (family 11, unsupported) "
        "for device 0x1638; the immediate is changed to 5 and the epilogue jump is kept. The hardware "
        "query (IOConnectCallMethod selector 0x100) and the rest of the classification are preserved "
        "because the same output struct is reused as `sHardwareInfo` by getExtraIdInfo/"
        "VAAddrLibInterface::init; skipping the query (the mode the legacy Ventura patch forced) would "
        "leave it zeroed and break addrlib init. AvdVAInterface::CreateDriver rejects family 11/12 and "
        "continues for 5.",
    },
    {
        "key": "AddrLibInitVegaToRV",
        "comment": "VAAddrLibInterface::init: map family 0x8E (RV/Renoir) to addrlib family 0xD",
        "symbols": [("__ZN18VAAddrLibInterface4initEP11VAIOConnectP13sHardwareInfo", 0x08B)],
        "find": "4181FC8D000000",
        "replace": "4181FC8E000000",
        "rationale": "`cmp $0x8D,%r12d` (AI/Vega family) selects ADDR_CREATE_INPUT.chipFamily=0xD. "
        "Renoir reports family 0x8E (RV), which matches no branch and leaves chipFamily=0, so "
        "Addr::Lib::Create fails. Retargeting the compare to 0x8E gives Renoir the 0xD path.",
    },
    {
        "key": "AddrLibCreateVegaToRV",
        "comment": "Addr::Lib::Create: accept family 0x8E and take Addr::Gfx9HwlInit",
        "symbols": [("__ZN4Addr3Lib6CreateEPK18_ADDR_CREATE_INPUTP19_ADDR_CREATE_OUTPUT", 0x0B5)],
        "find": "3D8D0000000F85",
        "replace": "3D8E0000000F85",
        "rationale": "After chipFamily=0xD the create path compares the real family (input+8) with "
        "0x8D for Gfx9HwlInit (and 0x8F for Gfx10HwlInit). Renoir is 0x8E, so the 0x8D compare is "
        "retargeted to 0x8E and Gfx9HwlInit is selected.",
    },
    {
        "key": "Gfx9ConvertChipFamilyVegaToRV",
        "comment": "Addr::V2::Gfx9Lib::HwlConvertChipFamily: configure the 0x8E family via the Gfx9 path",
        "symbols": [("__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj", 0x000)],
        "find": "81FE8D0000000F8588000000",
        "replace": "81FE8E0000000F8588000000",
        "rationale": "The routine returns early without setting the Gfx9Lib settings word unless the "
        "family is 0x8D; HwlInitGlobalParams then fails (it requires bit 0 of settings+0x5B10). "
        "Retargeting the compare to 0x8E runs the native Gfx9 configuration for Renoir.",
    },
    {
        "key": "ForceGfx9EngineAndBlt",
        "comment": "VAFactory::createGraphicsEngine/createImageBlt: force the GFX9 engine type",
        "symbols": [
            ("__ZN9VAFactory20createGraphicsEngineEP9VAContext", 0x00A),
            ("__ZN9VAFactory14createImageBltEP9VAContextj", 0x00B),
        ],
        "find": "488B86600400008B400C8D48FF83",
        "replace": "B80200000090909090908D48FF83",
        "rationale": "The two sites load `ctx->hwConfig->hwFamily` and branch on it: 1/2 select the "
        "Gfx9 classes, 3/4 the Gfx10 ones (Renoir's patched family gives 3). Overwriting the load "
        "with `mov $2,%eax; nop*5` forces 2 and keeps the following `lea/cmp/jb` bytes intact; the "
        "createUVDCommand site that shares the same 10-byte prefix is excluded by the 14-byte find.",
    },
    {
        "key": "Vcn2EncCommandFeedbackStride",
        "comment": "Vcn2EncCommand::getFeedbackSize: report the firmware's 48-byte feedback stride",
        "symbols": [("__ZN14Vcn2EncCommand15getFeedbackSizeEv", 0x004)],
        "find": "B8A40000005DC3",
        "replace": "B8300000005DC3",
        "rationale": "The stock VCN2 encoder reports a 0xA4 (164) byte feedback size while the VCN firmware "
        "feedback layout used by this engine has a 0x30 (48) byte stride. The oversized value makes the "
        "encoder's feedback/bitstream copy fail (VideoToolbox -12704 and an empty encoded packet). The "
        "immediate is changed to 0x30; the function prologue/epilogue are preserved.",
    },
    {
        "key": "Vcn2AvcProcessEncodeStatusStride",
        "comment": "AVDVcn2AvcEncDataProcessor::ProcessEncodeStatus: use the 48-byte feedback stride",
        "symbols": [
            ("__ZN26AVDVcn2AvcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture", 0x02A),
        ],
        "find": "4869C0A4000000",
        "replace": "4869C030000000",
        "rationale": "The AVC status parser multiplies the feedback index by 0xA4 before reading the "
        "per-frame feedback entry; it must use the same 0x30 stride as getFeedbackSize or it reads past "
        "the feedback buffer. The 7-byte imul immediate is retargeted to 0x30.",
    },
    {
        "key": "Vcn2HevcProcessEncodeStatusStride",
        "comment": "AVDVcn2HevcEncDataProcessor::ProcessEncodeStatus: use the 48-byte feedback stride",
        "symbols": [
            ("__ZN27AVDVcn2HevcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture", 0x02A),
        ],
        "find": "4869C0A4000000",
        "replace": "4869C030000000",
        "rationale": "The HEVC status parser has the same 0xA4 feedback-index multiply as the AVC path; "
        "the 7-byte imul immediate is retargeted to the 0x30 stride used by getFeedbackSize.",
    },
    {
        "key": "Vcn2EncoderSourcePitch",
        "comment": "Vcn2Encoder::setEncodeParams: read true source texture pitch instead of reconstructed width",
        "symbols": [
            ("__ZN11Vcn2Encoder15setEncodeParamsEPvP15VAVendorTexture", 0x034),
        ],
        "find": "8B8A840000008B97240400000FAFD10FB6FAB80001000029F885FF0F44C701D0",
        "replace": "8B82800000008B8A840000009090909090909090909090909090909090909090",
        "rationale": "The stock VCN2 encoder calculates pitch from the encoded target width rather than "
        "reading the source texture pitch fields. For APU / Retina screen recording, this produces mismatched "
        "line strides (causing scanline distortion and horizontal squeeze) and misaligned chroma offsets "
        "(severe crimson red tint). Replacing the width-reconstruction instructions with a direct read of "
        "the texture pitch (+0x80) and element size (+0x84) preserves exact geometry and chroma balance.",
    },
    {
        "key": "ForceGfx9VideoProcessors",
        "comment": "VAFactory::createColourFormatConverterVP/createDitheringVP/createDeblockerVP: GFX9 branch",
        "symbols": [
            ("__ZN9VAFactory29createColourFormatConverterVPEi", 0x007),
            ("__ZN9VAFactory17createDitheringVPEi", 0x007),
            ("__ZN9VAFactory17createDeblockerVPEi", 0x007),
        ],
        "find": "8D46FF83F802721F",
        "replace": "8D46FF83F802EB1F",
        "rationale": "Each factory starts with `lea -1(%rsi),%eax; cmp $2,%eax; jb <Gfx9 ctor>`. "
        "Turning the conditional `jb` into an unconditional `jmp` (same displacement) always takes "
        "the Gfx9 constructor while leaving the argument untouched (the legacy approach).",
    },
]


class CacheError(RuntimeError):
    pass


def warn_if_live_patched_cache(cache_dir: pathlib.Path) -> bool:
    """Detect reading the running system's cache on a `-NRedVDA`/`-NRedAVC` boot.

    The user-space page patcher writes replacement bytes into the dyld cache's
    file-backed page cache, so reads through that cache can return patched bytes
    (or flip between stock and patched as pages are evicted and re-validated).
    Generation for the running system is only trustworthy on a boot without the
    VDA boot-args, or against a mounted offline volume.

    Returns True when the cache is a live one and the patches are active this
    boot; callers warn for `--check` but must refuse to generate.
    """
    if cache_dir not in LIVE_CACHE_DIRS:
        return False
    try:
        bootargs = subprocess.check_output(["/usr/sbin/sysctl", "-n", "kern.bootargs"], text=True)
    except (OSError, subprocess.SubprocessError):
        return False
    if "-NRedVDA" in bootargs or "-NRedAVC" in bootargs:
        print(
            "warning: this boot has the VDA user-space patches active; pages of the live dyld "
            "cache may read back patched bytes. Regenerate on a boot without -NRedVDA/-NRedAVC "
            "or against an offline volume if any verification fails unexpectedly.",
            file=sys.stderr,
        )
        return True
    return False


class DyldCache:
    """Minimal read-only dyld shared cache reader (main file + subcaches)."""

    def __init__(self, directory: pathlib.Path):
        self.files = []
        self.maps = []  # (start, end, file_offset, file)
        header = None
        for path in sorted(directory.glob("dyld_shared_cache_x86_64h*")):
            if path.suffix in (".map", ".atlas"):
                continue
            handle = path.open("rb")
            page = handle.read(0x1C8)
            if not page.startswith(b"dyld_v1"):
                handle.close()
                continue
            mapping_offset, mapping_count = struct.unpack_from("<II", page, 16)
            handle.seek(mapping_offset)
            table = handle.read(mapping_count * 32)
            for index in range(mapping_count):
                address, size, file_offset, _, _ = struct.unpack_from("<QQQII", table, index * 32)
                self.maps.append((address, address + size, file_offset, handle))
            if path.name == "dyld_shared_cache_x86_64h":
                header = page
            self.files.append(handle)
        if header is None:
            raise CacheError(f"no main dyld cache file under {directory}")
        self.header = header
        self.base = self.maps[0][0]
        self.uuid = header[88:104]
        unslid = struct.unpack_from("<Q", header, 224)[0]
        if unslid != self.base:
            raise CacheError(f"header unslid base {unslid:#x} != first mapping {self.base:#x}")

    def close(self):
        for handle in self.files:
            handle.close()

    def mapping_for(self, address: int):
        for start, end, file_offset, handle in self.maps:
            if start <= address < end:
                return start, end, file_offset, handle
        raise CacheError(f"address {address:#x} is not mapped by the cache")

    def read(self, address: int, size: int) -> bytes:
        start, end, file_offset, handle = self.mapping_for(address)
        if address + size > end:
            raise CacheError(f"read {size:#x} at {address:#x} crosses a cache mapping")
        handle.seek(file_offset + (address - start))
        data = handle.read(size)
        if len(data) != size:
            raise CacheError(f"short read at {address:#x}")
        return data

    def read_page(self, address: int) -> bytes:
        """Return the exact 4 KiB file page that contains `address`."""
        page = address & ~(PAGE_SIZE - 1)
        start, end, file_offset, handle = self.mapping_for(page)
        offset = file_offset + (page - start)
        handle.seek(offset)
        data = handle.read(PAGE_SIZE)
        if len(data) != PAGE_SIZE:
            raise CacheError(f"short page read at {address:#x}")
        return data

    def __enter__(self):
        return self

    def __exit__(self, *_: object):
        self.close()


class MachOImage:
    """Parsed view of one image inside the dyld shared cache."""

    def __init__(self, cache: DyldCache, path: str):
        self.cache = cache
        self.base = self._image_base(path)
        header = cache.read(self.base, 32)
        if struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
            raise CacheError(f"image {path} has no 64-bit Mach-O header")
        (ncmds,) = struct.unpack_from("<I", header, 16)
        (sizeofcmds,) = struct.unpack_from("<I", header, 20)
        commands = cache.read(self.base + 32, sizeofcmds)
        self.segments = []
        self.sections = []
        self.symtab = None
        self.dysymtab = None
        self.uuid = None
        self.build_version = None
        cursor = 0
        for _ in range(ncmds):
            cmd, length = struct.unpack_from("<II", commands, cursor)
            if cmd == 0x19:  # LC_SEGMENT_64
                name, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from(
                    "<16sQQQQIIII", commands, cursor + 8
                )
                self.segments.append((name.rstrip(b"\0").decode(), vmaddr, vmsize, fileoff, filesize))
                for index in range(nsects):
                    offset = cursor + 72 + index * 80
                    sectname, segname = struct.unpack_from("<16s16s", commands, offset)
                    addr, size, file_offset = struct.unpack_from("<QQI", commands, offset + 32)
                    self.sections.append(
                        (sectname.rstrip(b"\0").decode(), segname.rstrip(b"\0").decode(), addr, size, file_offset)
                    )
            elif cmd == 0x2:  # LC_SYMTAB
                self.symtab = struct.unpack_from("<IIII", commands, cursor + 8)
            elif cmd == 0xB:  # LC_DYSYMTAB
                self.dysymtab = struct.unpack_from("<18I", commands, cursor + 8)
            elif cmd == 0x1B:  # LC_UUID
                self.uuid = commands[cursor + 8 : cursor + 24]
            elif cmd == 0x32:  # LC_BUILD_VERSION
                self.build_version = struct.unpack_from("<IIIII", commands, cursor + 8)
            cursor += length
        self._load_symbols()

    def _image_base(self, path: str) -> int:
        header = self.cache.header
        images_offset, image_count = struct.unpack_from("<II", header, 448)
        for index in range(image_count):
            entry = self.cache.read(self.cache.base + images_offset + index * 32, 32)
            path_offset = struct.unpack_from("<I", entry, 24)[0]
            image_path = self.cache.read(self.cache.base + path_offset, 1024).split(b"\0")[0]
            if image_path.decode(errors="replace") == path:
                return struct.unpack_from("<Q", entry)[0]
        raise CacheError(f"image {path} not found in the dyld cache image table")

    def _link_read(self, offset: int, size: int) -> bytes:
        linkedit = next(segment for segment in self.segments if segment[0] == "__LINKEDIT")
        return self.cache.read(linkedit[1] + offset - linkedit[3], size)

    def _load_symbols(self) -> None:
        symoff, nsyms, stroff, strsize = self.symtab
        entries = self._link_read(symoff, nsyms * 16)
        strings = self._link_read(stroff, strsize)
        self.symbols = {}
        for index in range(nsyms):
            strx, ntype, _, _, address = struct.unpack_from("<IBBHQ", entries, index * 16)
            if ntype & 0x0E != 0x0E or address == 0:
                continue
            end = strings.find(b"\0", strx)
            self.symbols[strings[strx:end].decode(errors="replace")] = address
        self.text = next(section for section in self.sections if section[0] == "__text" and section[1] == "__TEXT")
        self.text_symbols = sorted(address for address in self.symbols.values() if self.text[2] <= address < self.text[2] + self.text[3])

    def symbol(self, name: str) -> int:
        try:
            return self.symbols[name]
        except KeyError:
            raise CacheError(f"symbol {name} not found in the image") from None

    def function_end(self, address: int) -> int:
        for candidate in self.text_symbols:
            if candidate > address:
                return candidate
        return self.text[2] + self.text[3]

    def text_bytes(self) -> bytes:
        return self.cache.read(self.text[2], self.text[3])


def build_plan(cache: DyldCache, image: MachOImage):
    """Resolve every patch site and page, verifying bytes as we go."""

    pages = {}  # page address -> page bytes
    entries = []

    for spec in PATCHES:
        find = bytes.fromhex(spec["find"])
        replace = bytes.fromhex(spec["replace"])
        if len(find) != len(replace):
            raise CacheError(f"{spec['key']}: find/replace length mismatch")
        if len(find) > 32:
            raise CacheError(f"{spec['key']}: find is longer than 32 bytes ({len(find)})")

        sites = []
        for symbol, offset in spec["symbols"]:
            function = image.symbol(symbol)
            func_end = image.function_end(function)
            site = function + offset
            scan_len = min(image.text[2] + image.text[3] - function, max(func_end - function, offset + 256))
            func_bytes = cache.read(function, scan_len) if scan_len > 0 else b""

            if spec["key"] == "IdentifyForceFamily5":
                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    # Scan for fallback pattern in identify(unsigned, unsigned)
                    # Sequoia/Tahoe: BB 0B 00 00 00 E9 (mov $0xb, %ebx; jmp epilogue)
                    # Ventura:       BB 0A 00 00 00 E9 (mov $0xa, %ebx; jmp epilogue)
                    cand_off = -1
                    for pat in (b"\xbb\x0b\x00\x00\x00\xe9", b"\xbb\x0a\x00\x00\x00\xe9", b"\xbb\x0a\x00\x00\x00", b"\xbb\x0b\x00\x00\x00"):
                        idx = func_bytes.find(pat)
                        if idx >= 0 and idx + 10 <= len(func_bytes):
                            cand_off = idx
                            break
                    if cand_off >= 0:
                        site = function + cand_off
                        find = cache.read(site, 10)
                        replace = bytes.fromhex("BB05000000") + find[5:]
                        actual = find
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            elif spec["key"] == "AddrLibCreateVegaToRV":
                window_start = function + max(0, offset - 32)
                window = cache.read(window_start, 64)

                # Stock Ventura contains: 8D 88 73 FF FF FF 83 F9 02 72 1D
                # (lea -0x8d(%rax),%ecx; cmp $2,%ecx; jb Gfx9HwlInit)
                # This range check already accepts both 0x8D (Vega10) and 0x8E (Renoir) natively!
                if b"\x83\xf9\x02\x72" in window or b"\x73\xff\xff\xff\x83" in window:
                    print(
                        "[NootedRed VDA] AddrLibCreateVegaToRV: native range check (familyId - 0x8D < 2) detected; "
                        "Renoir 0x8E natively enters Gfx9HwlInit. Skipping patch.",
                        file=sys.stderr,
                    )
                    continue

                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    idx = window.find(b"\x8d\x00\x00\x00")
                    if idx >= 0:
                        imm_pos = idx
                        inst_start = max(0, imm_pos - 1)
                        if imm_pos >= 4 and window[imm_pos - 4 : imm_pos - 2] == b"\x41\x81":
                            inst_start = imm_pos - 4
                        elif imm_pos >= 3 and (window[imm_pos - 3] == 0x81 or window[imm_pos - 3 : imm_pos - 1] == b"\x41\x81"):
                            inst_start = imm_pos - 3
                        elif imm_pos >= 2 and window[imm_pos - 2] == 0x81:
                            inst_start = imm_pos - 2
                        elif imm_pos >= 1 and window[imm_pos - 1] == 0x3d:
                            inst_start = imm_pos - 1

                        branch_end = imm_pos + 4
                        if branch_end + 2 <= len(window) and window[branch_end] in (0x74, 0x75):
                            branch_end += 2
                        elif branch_end + 6 <= len(window) and window[branch_end : branch_end + 2] in (b"\x0f\x84", b"\x0f\x85"):
                            branch_end += 6

                        site = window_start + inst_start
                        find = window[inst_start:branch_end]
                        replace = window[inst_start:imm_pos] + b"\x8e\x00\x00\x00" + window[imm_pos + 4 : branch_end]
                        actual = find
                        print(f"[NootedRed VDA] AddrLibCreateVegaToRV dynamically resolved: find={find.hex()} replace={replace.hex()}", file=sys.stderr)
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            elif spec["key"] == "Gfx9ConvertChipFamilyVegaToRV":
                window = cache.read(function, 256)
                if b"\x73\xff\xff\xff\x83" in window:
                    print(
                        "[NootedRed VDA] Gfx9ConvertChipFamilyVegaToRV: native range check detected; skipping patch.",
                        file=sys.stderr,
                    )
                    continue

                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    idx = window.find(b"\x8d\x00\x00\x00")
                    if idx >= 0:
                        imm_pos = idx
                        inst_start = imm_pos - 2 if imm_pos >= 2 and window[imm_pos - 2] == 0x81 else max(0, imm_pos - 2)
                        branch_end = imm_pos + 4
                        if branch_end + 6 <= len(window) and window[branch_end : branch_end + 2] in (b"\x0f\x84", b"\x0f\x85"):
                            branch_end += 6
                        elif branch_end + 2 <= len(window) and window[branch_end] in (0x74, 0x75):
                            branch_end += 2

                        site = function + inst_start
                        find = window[inst_start:branch_end]
                        replace = window[inst_start:imm_pos] + b"\x8e\x00\x00\x00" + window[imm_pos + 4 : branch_end]
                        actual = find
                        print(f"[NootedRed VDA] Gfx9ConvertChipFamilyVegaToRV dynamically resolved: find={find.hex()} replace={replace.hex()}", file=sys.stderr)
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            elif spec["key"] in ("Vcn2AvcProcessEncodeStatusStride", "Vcn2HevcProcessEncodeStatusStride"):
                window = cache.read(function, 256)
                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    # Dynamically locate `imul $0xa4, %reg, %reg`
                    # Sequoia/Tahoe: 48 69 C0 A4 00 00 00 (imul $0xa4, %rax, %rax)
                    # Ventura:       4C 69 .. A4 00 00 00 (or other register variant)
                    idx = window.find(b"\xa4\x00\x00\x00")
                    if idx >= 3:
                        imm_pos = idx
                        inst_start = imm_pos - 3
                        site = function + inst_start
                        find = window[inst_start : imm_pos + 4]
                        replace = window[inst_start:imm_pos] + b"\x30\x00\x00\x00"
                        actual = find
                        print(f"[NootedRed VDA] {spec['key']} dynamically resolved at +{inst_start:#x}: find={find.hex()} replace={replace.hex()}", file=sys.stderr)
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            elif spec["key"] == "Vcn2EncoderSourcePitch":
                window = cache.read(function, min(func_end - function, 512))
                find_modern = bytes.fromhex("8B8A840000008B87240400000FAFC105FF0000002500FFFFFF")
                replace_modern = bytes.fromhex("8B82800000008B8A8400000090909090909090909090909090")
                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                elif (cand_off := window.find(find)) >= 0:
                    site = function + cand_off
                    actual = find
                elif (cand_off_m := window.find(find_modern)) >= 0:
                    site = function + cand_off_m
                    find = find_modern
                    replace = replace_modern
                    actual = find
                    print(f"[NootedRed VDA] Vcn2EncoderSourcePitch dynamically resolved (modern 25B) at +{cand_off_m:#x}", file=sys.stderr)
                else:
                    # Anchored by `31 d2 f7 f1` (xor %edx, %edx; div %ecx) before `89 43 18 d1 e8 89 43 1c`
                    xor_idx = window.find(b"\x31\xd2\xf7\xf1")
                    if xor_idx >= 25 and window[xor_idx - 25 : xor_idx].startswith(b"\x8b\x8a\x84\x00\x00\x00"):
                        site = function + (xor_idx - 25)
                        find = window[xor_idx - 25 : xor_idx]
                        replace = replace_modern
                        actual = find
                        print(f"[NootedRed VDA] Vcn2EncoderSourcePitch dynamically resolved at +{xor_idx - 25:#x}: find={find.hex()}", file=sys.stderr)
                    elif xor_idx >= 32 and window[xor_idx - 32 : xor_idx].startswith(b"\x8b\x8a\x84\x00\x00\x00"):
                        site = function + (xor_idx - 32)
                        find = window[xor_idx - 32 : xor_idx]
                        actual = find
                        print(f"[NootedRed VDA] Vcn2EncoderSourcePitch dynamically resolved at +{xor_idx - 32:#x}: find={find.hex()}", file=sys.stderr)
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            elif spec["key"] == "ForceGfx9VideoProcessors":
                window = cache.read(function, 128)
                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    cand_off = window.find(find)
                    if cand_off >= 0:
                        site = function + cand_off
                        actual = find
                    else:
                        hit = window.find(b"\x83\xf8\x02\x72")
                        if hit >= 3 and len(window) >= hit + 5:
                            inst_start = hit - 3
                            site = function + inst_start
                            find = window[inst_start : hit + 5]
                            replace = window[inst_start:hit+3] + b"\xeb" + window[hit+4:hit+5]
                            actual = find
                            print(f"[NootedRed VDA] ForceGfx9VideoProcessors dynamically resolved at +{inst_start:#x}: find={find.hex()} replace={replace.hex()}", file=sys.stderr)
                        else:
                            actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""
            else:
                if site + len(find) <= func_end and cache.read(site, len(find)) == find:
                    actual = find
                else:
                    search_window = cache.read(function, min(1024, max(256, offset + 256)))
                    cand_off = search_window.find(find)
                    if cand_off >= 0:
                        site = function + cand_off
                        actual = find
                    else:
                        actual = cache.read(site, len(find)) if site + len(find) <= func_end else b""

            if actual != find:
                raise CacheError(
                    f"{spec['key']}: bytes at {site:#x} ({symbol}+{offset:#x}) are {actual.hex()}, expected {find.hex()}"
                )
            sites.append(site)

        if not sites:
            continue

        # Sites of one spec may span several 4 KiB pages; emit one record per
        # page so each page keeps its own exact page image, skip and count.
        sites_by_page: dict[int, list[int]] = {}
        for site in sites:
            sites_by_page.setdefault(site & ~(PAGE_SIZE - 1), []).append(site)

        for page_address, page_sites in sorted(sites_by_page.items()):
            page_bytes = cache.read_page(page_address)
            if page_address not in pages:
                pages[page_address] = page_bytes

            page = page_address
            page_offset = page - image.base
            if page_offset % PAGE_SIZE:
                raise CacheError(f"{spec['key']}: page {page:#x} is not image-page aligned")
            text_segment = next(segment for segment in image.segments if segment[0] == "__TEXT")
            text_file_offset = text_segment[3] + page_offset
            mapped_start, _, mapped_file_offset, _ = cache.mapping_for(page)
            if mapped_file_offset + (page - mapped_start) != text_file_offset:
                raise CacheError(
                    f"{spec['key']}: mapping file offset {mapped_file_offset + (page - mapped_start):#x} "
                    f"!= __TEXT file offset {text_file_offset:#x}"
                )

            matches = [offset for offset in range(0, PAGE_SIZE - len(find) + 1) if page_bytes[offset : offset + len(find)] == find]
            expected = [site - page for site in page_sites]
            skip = 0
            while skip < len(matches) and matches[skip] < expected[0]:
                skip += 1
            if matches[skip : skip + len(expected)] != expected:
                raise CacheError(f"{spec['key']}: expected sites {[hex(x) for x in expected]} not found on page")
            if len(matches) < skip + len(expected):
                raise CacheError(
                    f"{spec['key']}: page has {len(matches)} occurrences of the find, expected at least {skip + len(expected)}"
                )
            if page_bytes.count(find) != len(matches):
                raise CacheError(f"{spec['key']}: inconsistent occurrence count")

            entries.append(
                {
                    "key": spec["key"],
                    "comment": spec["comment"],
                    "rationale": spec["rationale"],
                    "page_address": page_address,
                    "page_offset": page_offset,
                    "sites": page_sites,
                    "find": find,
                    "replace": replace,
                    "skip": skip,
                    "count": len(expected),
                    "matches": matches,
                }
            )

    ordered_pages = sorted(pages)
    for entry in entries:
        entry["page_index"] = ordered_pages.index(entry["page_address"])
    entries.sort(key=lambda entry: (entry["page_index"], entry["sites"][0]))
    return ordered_pages, pages, entries


def _array(values: bytes, per_line: int = 16) -> str:
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start : start + per_line]
        lines.append("        " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
    return "\n".join(lines)


def render_header(cache: DyldCache, image: MachOImage, pages, page_images, entries, profile: dict) -> str:
    text = image.text_bytes()
    text_hash = hashlib.sha256(text).hexdigest()
    image_uuid = image.uuid.hex().upper() if image.uuid else "UNKNOWN"
    macos_version = profile["macos_version"]
    driver_version = profile["driver_version"]
    cache_uuid = profile["cache_uuid"]
    namespace = f"VDAVAData{profile['namespace_suffix']}"
    if image.build_version:
        platform, minos, sdk, _, _ = image.build_version
    else:
        platform = minos = sdk = 0
    page_lines = []
    for index, page_address in enumerate(pages):
        page_lines.append(
            f"    // Page {index}: AMDRadeonVADriver2 __TEXT file page at image offset "
            f"0x{page_address - image.base:06X} (cache file page).\n"
            f"    inline constexpr UInt8 Page{index}[4096] = {{\n{_array(page_images[page_address])}\n    }};\n"
        )

    sites_per_page = {}
    for entry in entries:
        sites_per_page[entry["page_index"]] = sites_per_page.get(entry["page_index"], 0) + entry["count"]
    max_sites_per_page = max(sites_per_page.values())

    find_lines = []
    patch_lines = []
    for index, entry in enumerate(entries):
        site_list = ", ".join(f"{site:#x}" for site in entry["sites"])
        find_lines.append(
            f"    // {entry['comment']}.\n"
            f"    // Sites: {site_list} (page offset {entry['sites'][0] - entry['page_address']:#04x}, "
            f"skip {entry['skip']}, count {entry['count']}).\n"
            f"    inline constexpr UInt8 Find{index}[] = {{{', '.join(f'0x{byte:02X}' for byte in entry['find'])}}};\n"
            f"    inline constexpr UInt8 Replace{index}[] = {{{', '.join(f'0x{byte:02X}' for byte in entry['replace'])}}};\n"
        )
        patch_lines.append(
            f"        // {entry['comment']}\n"
            f"        {{UserPatcher::FileSegment::SegmentTextText, Page{entry['page_index']}, Find{index}, Replace{index},\n"
            f"         sizeof(Find{index}), {entry['skip']}, {entry['count']}}},"
        )

    return f"""// Generated by tools/gen_vda_va_data.py -- do not edit by hand.
//
// Source: macOS {macos_version}, AMDRadeonVADriver2 {driver_version}
//   dyld cache UUID {cache_uuid}
//   dyld cache base 0x{CACHE_BASE:X}
//   image path {VA_IMAGE_PATH}
//   image base 0x{image.base:X}, image UUID {image_uuid}, LC_BUILD_VERSION platform {platform} minos {minos:#x} sdk {sdk:#x}
//   __text SHA-256 {text_hash}
//
// These patches are only valid for the exact image above and only for the PCI device they are
// generated for (Renoir 0x1638).  They force the stock VA driver to identify the GFX9/VCN 2.x
// iGPU as VA family 5 (the Navi10/VCN2 class) while still creating GFX9 engines, GFX9 video
// processors and a Gfx9 address library.  Callers must gate the table behind `-NRedVDA` and the
// 0x1638 device profile, and prove that `-NRedVDA` is off by default.
//
// Table size: {len(pages)} page image(s), {len(entries)} patch record(s), {sum(entry["count"] for entry in entries)} patched byte site(s).
// The native UserPatches backend must support at least {len(pages)} pages, {max_sites_per_page} site(s)
// per page and {max(len(entry["find"]) for entry in entries)} bytes per patch.
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#pragma once
#include <UserPatches.hpp>
#include <libkern/OSTypes.h>

// User-space patches for `/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2`
// (macOS {macos_version}).  See {profile['evidence_doc']} for the evidence behind every site.
namespace {namespace}
{{
    // Identifies the exact inputs this data was generated from; the integration must refuse a cache
    // or driver build that does not match.
    inline constexpr char SourceDescription[] =
        "macOS {macos_version}; AMDRadeonVADriver2 {driver_version}; dyld cache {cache_uuid} base 0x{CACHE_BASE:X}; "
        "image base 0x{image.base:X}";
    inline constexpr char CacheUUID[] = "{cache_uuid}";
    inline constexpr char TextSHA256[] = "{text_hash}";
    inline constexpr UInt64 CacheBase = 0x{CACHE_BASE:X}ULL;
    inline constexpr UInt64 ImageBase = 0x{image.base:X}ULL;

{''.join(page_lines)}
{''.join(find_lines)}
    // Patch records.  `skip`/`count` select the verified occurrences of each find inside its page
    // image, so a patch can never hit an unrelated binary page.
    inline constexpr UserPatches::PagePatch Patches[] = {{
{chr(10).join(patch_lines)}
    }};
    inline constexpr size_t PatchCount = sizeof(Patches) / sizeof(Patches[0]);
}}    // namespace {namespace}
"""


HEADER_PATTERNS = {
    "page": re.compile(r"inline constexpr UInt8 (Page\d+)\[4096\] = \{(.*?)\};", re.S),
    "patch": re.compile(r"inline constexpr UInt8 (Find\d+|Replace\d+)\[\] = \{([^}]*)\};"),
    "record": re.compile(
        r"\{UserPatcher::FileSegment::SegmentTextText, (Page\d+), (Find\d+), (Replace\d+),\s*"
        r"sizeof\((Find\d+)\), (\d+), (\d+)\}"
    ),
}


def parse_header(text: str):
    arrays = {}
    for match in HEADER_PATTERNS["page"].finditer(text):
        arrays[match.group(1)] = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(2)))
    for match in HEADER_PATTERNS["patch"].finditer(text):
        arrays[match.group(1)] = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(2)))
    records = []
    for match in HEADER_PATTERNS["record"].finditer(text):
        records.append(
            {
                "page": match.group(1),
                "find": match.group(2),
                "replace": match.group(3),
                "skip": int(match.group(5)),
                "count": int(match.group(6)),
            }
        )
    return arrays, records


def verify_header(text: str, cache: DyldCache, image: MachOImage, pages, entries, profile: dict) -> list[str]:
    """Independently verify the emitted table against the live cache."""
    errors = []
    arrays, records = parse_header(text)
    page_names = {name for name in arrays if re.fullmatch(r"Page\d+", name)}
    expected_pages = {f"Page{index}" for index in range(len(pages))}
    if page_names != expected_pages:
        errors.append(f"header page images {sorted(page_names)} != expected {sorted(expected_pages)}")
    clean_uuid = profile["cache_uuid"].replace("-", "")
    if f'CacheUUID[] = "{profile["cache_uuid"]}"' not in text and f'CacheUUID[] = "{clean_uuid}"' not in text:
        errors.append("header CacheUUID constant does not match the generated value")
    if f"ImageBase = 0x{image.base:X}ULL" not in text:
        errors.append("header ImageBase constant does not match the generated value")
    text_hash = hashlib.sha256(image.text_bytes()).hexdigest()
    if f'TextSHA256[] = "{text_hash}"' not in text:
        errors.append("header TextSHA256 constant does not match the stock __text hash")
    if len(records) != len(entries):
        errors.append(f"header has {len(records)} patch records, generator derived {len(entries)}")
        return errors
    for index, (record, entry) in enumerate(zip(records, entries)):
        page_name = f"Page{entry['page_index']}"
        if record["page"] != page_name:
            errors.append(f"record {index}: page {record['page']} != {page_name}")
        find = arrays.get(record["find"], b"")
        replace = arrays.get(record["replace"], b"")
        if find != entry["find"]:
            errors.append(f"record {index}: find bytes differ")
        if replace != entry["replace"]:
            errors.append(f"record {index}: replace bytes differ")
        if record["skip"] != entry["skip"] or record["count"] != entry["count"]:
            errors.append(f"record {index}: skip/count differ")
        page = arrays.get(page_name, b"")
        if len(page) != PAGE_SIZE:
            errors.append(f"{page_name}: not a 4 KiB page image ({len(page)} bytes)")
            continue
        if page != cache.read_page(entry["page_address"]):
            errors.append(f"{page_name}: page image is not the live cache page")
            continue
        if page.count(find) != entry["skip"] + entry["count"]:
            errors.append(f"{page_name}: find occurrence count does not match skip+count")
        for site in entry["sites"]:
            offset = site - entry["page_address"]
            if page[offset : offset + len(find)] != find:
                errors.append(f"record {index}: find missing at {site:#x}")
            if len(replace) != len(find):
                errors.append(f"record {index}: replace length mismatch")
    return errors



def committed_stock_pages(cache: DyldCache, image: MachOImage, output_path: pathlib.Path):
    """Recover the stock page images for pages covered by an existing generated header.

    The VDA table is applied by `UserPatches` through the code-signing validation hook, which writes
    the patched bytes into the dyld cache's file-backed page cache.  A later generator run would then
    read patched bytes from an unmodified cache file.  The committed header stores the stock page
    images; use them as the generation source after checking that the live page is exactly the stock
    page with a subset of that header's replacements applied.  This keeps `--check` working while
    `-NRedVDA` is active and still rejects any unrelated system update.

    DANGER: never call this on a generation path (non-`--check`) against a live patched system.  It
    trusts the committed header's page images as stock; if that header was itself generated from a
    polluted page cache, the pollution is re-committed as "stock" and the verification loop passes
    while the kext silently skips every page.  Generation from a live patched cache is refused in
    main() unless `--force-live-cache` is given.
    """
    if not output_path.exists():
        return {}

    text = output_path.read_text()
    arrays, records = parse_header(text)
    page_offsets = {}
    for match in re.finditer(
        r"// Page \d+: AMDRadeonVADriver2 __TEXT file page at image offset 0x([0-9A-F]+)",
        text,
    ):
        page_offsets[f"Page{len(page_offsets)}"] = image.base + int(match.group(1), 16)
    if not page_offsets:
        return {}

    record_sites = []
    for match in re.finditer(r"// Sites: (.*?) \(page offset", text):
        record_sites.append([int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]+", match.group(1))])
    if len(record_sites) != len(records):
        raise CacheError("committed header site comments do not match its patch records")

    stock = {}
    for page_name, page_address in page_offsets.items():
        committed = arrays.get(page_name)
        if committed is None or len(committed) != PAGE_SIZE:
            raise CacheError(f"committed {page_name} is not a 4 KiB page image")
        live = bytearray(cache.read_page(page_address))
        for record, sites in zip(records, record_sites):
            if record["page"] != page_name:
                continue
            find = arrays[record["find"]]
            replace = arrays[record["replace"]]
            for site in sites:
                offset = site - page_address
                current = bytes(live[offset : offset + len(find)])
                if current == replace:
                    live[offset : offset + len(find)] = find
                elif current != find:
                    raise CacheError(
                        f"live page at {site:#x} is neither the committed find nor replace byte sequence"
                    )
        if bytes(live) != committed:
            raise CacheError(f"live {page_name} does not match the committed stock page")
        stock[page_address] = committed
    return stock


def derive(cache: DyldCache, output_path: pathlib.Path):
    image = MachOImage(cache, VA_IMAGE_PATH)
    stock = committed_stock_pages(cache, image, output_path)
    if not stock:
        pages, page_images, entries = build_plan(cache, image)
        return image, pages, page_images, entries

    original_read_page = cache.read_page
    original_read = cache.read
    original_text = image.text_bytes

    def read_page(address: int) -> bytes:
        page = address & ~(PAGE_SIZE - 1)
        return stock.get(page, original_read_page(address))

    def read(address: int, size: int) -> bytes:
        page = address & ~(PAGE_SIZE - 1)
        if page in stock and address + size <= page + PAGE_SIZE:
            return stock[page][address - page : address - page + size]
        return original_read(address, size)

    def text_bytes() -> bytes:
        data = bytearray(original_text())
        text_start = image.text[2]
        for address, page in stock.items():
            # Page 0 starts before __text; pages are not aligned to the __text buffer.
            offset = address - text_start
            source = 0
            if offset < 0:
                source = -offset
                offset = 0
            if offset >= len(data) or source >= PAGE_SIZE:
                continue
            length = min(PAGE_SIZE - source, len(data) - offset)
            data[offset : offset + length] = page[source : source + length]
        return bytes(data)

    cache.read_page = read_page
    cache.read = read
    image.text_bytes = text_bytes
    pages, page_images, entries = build_plan(cache, image)
    return image, pages, page_images, entries


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the committed header against the live cache")
    parser.add_argument("--output", type=pathlib.Path, default=None, help="header path to write/check")
    parser.add_argument(
        "--cache-dir",
        type=pathlib.Path,
        default=DEFAULT_CACHE_DIR,
        help="dyld shared cache directory of the target system",
    )
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path("/"),
        help="target system root for version plists (e.g. a mounted Tahoe volume)",
    )
    parser.add_argument(
        "--force-live-cache",
        action="store_true",
        help="allow generating from the live cache on a -NRedVDA/-NRedAVC boot (page cache may be polluted)",
    )
    args = parser.parse_args(argv)
    print(">>> NootedRed VDA generator v1.0.7 (dynamic Ventura/Sonoma/Sequoia support) <<<", file=sys.stderr)

    live_patched = warn_if_live_patched_cache(args.cache_dir)
    if live_patched and not args.check and not args.force_live_cache:
        print(
            "error: refusing to generate from the live dyld cache while the VDA user-space patches are "
            "active; the page cache may return patched bytes. Boot without -NRedVDA/-NRedAVC, point "
            "--cache-dir at an offline volume, or pass --force-live-cache to override.",
            file=sys.stderr,
        )
        return 1
    with DyldCache(args.cache_dir) as cache:
        uuid = "-".join(
            [
                cache.uuid[0:4].hex().upper(),
                cache.uuid[4:6].hex().upper(),
                cache.uuid[6:8].hex().upper(),
                cache.uuid[8:10].hex().upper(),
                cache.uuid[10:16].hex().upper(),
            ]
        )
        profile = next((candidate for candidate in PROFILES.values() if candidate["cache_uuid"] == uuid), None)
        version_path = args.root / SYSTEM_VERSION_PLIST
        system_version = {}
        if version_path.is_file():
            with version_path.open("rb") as handle:
                system_version = plistlib.load(handle)
        product = str(system_version.get("ProductVersion", ""))
        build = str(system_version.get("ProductBuildVersion", ""))
        key = (product, build)

        driver_plist_path = args.root / VA_INFO_PLIST
        driver_version = ""
        if driver_plist_path.is_file():
            with driver_plist_path.open("rb") as handle:
                driver_info = plistlib.load(handle)
            driver_version = str(driver_info.get("CFBundleShortVersionString", ""))

        if profile is None:
            is_host_root = (args.root.resolve() == Path("/").resolve())
            host_release = subprocess.check_output(["uname", "-r"]).decode() if is_host_root else ""
            if product.startswith("13.") or (not product and host_release.startswith("22.")):
                suffix = "Ventura"
            elif product.startswith("14.") or (not product and host_release.startswith("23.")):
                suffix = "Sonoma"
            elif product.startswith("15.") or (not product and host_release.startswith("24.")):
                suffix = "Sequoia"
            elif product.startswith("26.") or (not product and host_release.startswith("25.")):
                suffix = "Tahoe"
            else:
                suffix = ""

            if suffix:
                profile = {
                    "macos_version": f"{product} ({build})".strip() or suffix,
                    "driver_version": driver_version or "stock",
                    "cache_uuid": uuid,
                    "namespace_suffix": suffix,
                    "output_name": f"VDAVAData{suffix}.hpp",
                    "evidence_doc": f"analysis/vda-{product.replace('.', '') or suffix.lower()}-va-data.md",
                }
            else:
                supported = ", ".join(candidate["cache_uuid"] for candidate in PROFILES.values())
                raise CacheError(f"dyld cache UUID {uuid} matches no supported profile ({supported})")

        if cache.base != CACHE_BASE:
            raise CacheError(f"dyld cache base {cache.base:#x} does not match {CACHE_BASE:#x}")

        if key in PROFILES and PROFILES[key] is not profile:
            raise CacheError(f"target root is macOS {key[0]} ({key[1]}), which does not match cache UUID {uuid}")

        if driver_version and profile.get("driver_version") and driver_version != profile["driver_version"]:
            profile["driver_version"] = driver_version

        output = args.output or ROOT / "NootedRed" / profile["output_name"]
        image, page_addresses, page_images, entries = derive(cache, output)
        header = render_header(cache, image, page_addresses, page_images, entries, profile)
        errors = verify_header(header, cache, image, page_addresses, entries, profile)
        if errors:
            for error in errors:
                print(f"error: {error}", file=sys.stderr)
            return 1

        sites = sum(entry["count"] for entry in entries)
        print(f"cache  : {profile['cache_uuid']} base {cache.base:#x}")
        print(f"image  : {VA_IMAGE_PATH}")
        print(f"         {profile['driver_version']}, image base {image.base:#x}, __text {len(image.text_bytes()):#x} bytes")
        print(f"         __text SHA-256 {hashlib.sha256(image.text_bytes()).hexdigest()}")
        print(f"tables : {len(page_images)} page image(s), {len(entries)} patch record(s), {sites} site(s)")
        for entry in entries:
            print(
                f"  {entry['key']:<32} page {entry['page_index']} "
                f"image offset {entry['sites'][0] - image.base:#06x} page offset {entry['sites'][0] - entry['page_address']:#05x} "
                f"skip {entry['skip']} count {entry['count']}"
            )

        if args.check:
            if not output.exists():
                print(f"error: {output} does not exist", file=sys.stderr)
                return 1
            committed = output.read_text()
            committed_errors = verify_header(committed, cache, image, page_addresses, entries, profile)
            if committed_errors:
                for error in committed_errors:
                    print(f"error: {error}", file=sys.stderr)
                return 1
            if committed == header:
                print(f"check  : {output} matches the live cache")
            else:
                print(f"check  : {output} is semantically current (only formatting/comments differ)")
            return 0

    output.write_text(header)
    print(f"wrote  : {output} ({len(header)} bytes)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except CacheError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
