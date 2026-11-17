#!/usr/bin/env python3
# AppleGVA user-space VDA patch data generator
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

"""Generate NootedRed/VDAGVAData*.hpp from a supported macOS dyld shared cache.

Supported targets are pinned by PROFILES (macOS 15.8.1 and Tahoe 26.7.1); the
generator refuses anything else instead of emitting a table that would silently
never match.

The header carries the AppleGVA user-space patch that makes VideoToolbox pick the
AMD VCN2 decoder on an AMD-only Hackintosh.  On 15.8.1 the decoder-selection
function (`AVDGetGPUAccelerators`, local symbol) classifies the accelerators
found earlier by `AVDLoadAllAccelerators` and, for a board-id that is not one of
Apple's special AMD Macs, only inserts the Intel accelerator.  If no
`IntelAccelerator` service exists the function falls into its "return an empty
accelerator list" path, so every hardware decoder attempt is rejected and
VideoToolbox falls back to software.

The patch redirects that single conditional branch to the AMD insertion tail of
the same function (the tail used by the `gvaForceAMD*` overrides), so the AMD
accelerator that was already collected is returned when no Intel accelerator is
present.  Machines with both GPUs, explicit device requests, Apple's special
board-ids and the no-GPU-at-all path are not affected.

The output is generated from the running machine's dyld shared cache: it parses
the cache header/mappings/image table, locates the AppleGVA image, resolves the
selection function through its log-string cross references and `LC_FUNCTION_STARTS`
entries, derives the branch site from the function's own code, and embeds the
exact 4 KiB cache page that contains the site (the page the kernel code-signing
pager hands to the `_cs_validate_range` hook).

Nothing is loaded, executed, or written outside the generated header.  Run
without arguments to (re)write the header and with --check to re-read the live
cache and the emitted header and fail loudly on any mismatch.

Usage:
    python3 tools/gen_vda_gva_data.py
    python3 tools/gen_vda_gva_data.py --check
    python3 tools/gen_vda_gva_data.py --root /Volumes/Tahoe \
        --cache-dir /Volumes/Tahoe/System/Cryptexes/OS/System/Library/dyld \
        --header NootedRed/VDAGVADataTahoe.hpp
"""

from __future__ import annotations

import argparse
import hashlib
import plistlib
import re
import struct
import subprocess
import sys
from bisect import bisect_right
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_HEADER = REPO_ROOT / "NootedRed" / "VDAGVAData.hpp"
CACHE_CANDIDATES = (
    Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"),
    Path("/System/Cryptexes/OS/System/Library/dyld"),
    Path("/System/Library/dyld"),
)
LIVE_CACHE_DIRS = frozenset(CACHE_CANDIDATES)
SYSTEM_VERSION_PLIST = Path("System/Library/CoreServices/SystemVersion.plist")
GVA_INFO_PLIST = Path("System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/Resources/Info.plist")

# This data is specific to one OS build and one AppleGVA build.  The generator
# refuses to run against anything else instead of emitting a table that would
# silently never match.  Each profile pins the exact OS build, dyld cache UUID
# and AppleGVA image UUID it supports, plus the branch form the selection
# function was compiled into.
PROFILES = {
    ("13.6.4", "22G513"): {
        "cache_uuid": "7FE9478F8955356CAC515C6FFC253B95",
        "gva_version": "113.54.1",
        "gva_image_uuid": None,
        "branch_form": "ventura",
        "namespace_suffix": "Ventura",
        "evidence_doc": "analysis/vda-1364-gva-data.md",
    },
    ("13.7.8", "22H730"): {
        "cache_uuid": "8EEE66864D4033A3B07BB334FD3E3ECC",
        "gva_version": "113.54.1",
        "gva_image_uuid": None,
        "branch_form": "ventura",
        "namespace_suffix": "Ventura",
        "evidence_doc": "analysis/vda-1378-gva-data.md",
    },
    ("14.7.4", "23H420"): {
        "cache_uuid": "85507E781FE337F1AEFFC4164FE279D2",
        "gva_version": "113.54.1",
        "gva_image_uuid": None,
        "branch_form": "ventura",
        "namespace_suffix": "Sonoma",
        "evidence_doc": "analysis/vda-1474-gva-data.md",
    },
    ("14.8.9", "23J631"): {
        "cache_uuid": "4C1441FD8A543DC2863D49110B946A25",
        "gva_version": "113.54.1",
        "gva_image_uuid": "49FAB7CE70CC377E8134130B27851023",
        "branch_form": "ventura",
        "namespace_suffix": "Sonoma",
        "evidence_doc": "analysis/vda-1489-gva-data.md",
    },
    ("15.8.1", "24H32"): {
        "cache_uuid": "42EC2890BC40317EBB2745142BFF7F26",
        "gva_version": "113.56",
        "gva_image_uuid": "53DD765D087D322EA410BD22D586C08C",
        "branch_form": "sequoia",
        "namespace_suffix": "Sequoia",
        "evidence_doc": "analysis/vda-1581-gva-data.md",
    },
    ("26.7.1", "25G241"): {
        "cache_uuid": "3137475689B434E8A34F56C5D57216A1",
        "gva_version": "113.56",
        "gva_image_uuid": "CA5414F369373C059EA2D82B35AFD4F0",
        "branch_form": "tahoe",
        "namespace_suffix": "Tahoe",
        "evidence_doc": "analysis/vda-2671-gva-data.md",
    },
}
EXPECTED_CACHE_BASE = 0x7FF800000000
GVA_IMAGE_PATH = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"

PAGE_SIZE = 0x1000
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_X86_64 = 0x01000007
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
LC_FUNCTION_STARTS = 0x26
CACHE_HEADER_MAGIC = b"dyld_v1"
CACHE_HEADER_SIZE = 456

# Cross references used to name the otherwise-unnamed selection function.  The
# strings are Apple's own diagnostic messages; the function that logs them is the
# one that decides which accelerators to return.
ANCHOR_STRINGS = (
    b"GVA error: AVDGetGPUAccelerators, no plugin ",
    b"GVA error: AVDGetGPUAccelerators, invalid boardId ",
    b"GVA info: AVDGetGPUAccelerators, force AMD for flow_id ",
)

# `test %r15,%r15; je <empty-return>` - the branch that skips the AMD accelerator
# when no Intel accelerator was found.
BRANCH_PROLOGUE = bytes.fromhex("4D85FF0F84")
# Fall-through block: `lea -0x158(%rbp),%rsi; mov %rbx,%rdi; call <push_back>;
# jmp <empty-return>`.  -0x158 holds the single Intel accelerator slot.
INTEL_SLOT_LEA = bytes.fromhex("488DB5")
INTEL_SLOT_OFFSET = -0x158
# AMD insertion tail reached by the `gvaForceAMD*` overrides: push the first
# accelerator collected in the AMD array at -0x130.
AMD_TAIL = bytes.fromhex("4C8B7B08488B43104939C7")
AMD_ARRAY_OFFSET = -0x130

# Tahoe (26.7.1) branch form: the same selection logic with the Intel slot
# holder reallocated from %r15 to %r12.  `mov -0x158(%rbp),%r12;
# test %r12,%r12; je <invalid-boardId empty-return>`.
BRANCH_PROLOGUE_TAHOE = bytes.fromhex("4D85E40F84")
INTEL_SLOT_LOAD_TAHOE = bytes.fromhex("4C8BA5A8FEFFFF")
# Tahoe AMD insertion: the force-AMD override block ends with
# `lea -0x130(%rbp),%rsi; mov %rbx,%rdi; call <push_back>; jmp <epilogue>`.
AMD_ARRAY_LEA_TAHOE = bytes.fromhex("488DB5D0FEFFFF")


class GeneratorError(RuntimeError):
    """Raised when the live cache does not match the data this generator supports."""


def warn_if_live_patched_cache(cache_dir: Path) -> bool:
    """Detect reading a live cache while default-on media patching may be active.

    The user-space page patcher writes replacement bytes into the dyld cache's
    file-backed page cache, so reads through that cache can return patched bytes
    (or flip between stock and patched as pages are evicted and re-validated).
    Generation needs a boot with -NRedNoVDA (or NootedRed off), or an offline volume.

    Returns True when the cache is a live one and the patches are active this
    boot; callers warn for `--check` but must refuse to generate.
    """
    if cache_dir not in LIVE_CACHE_DIRS:
        return False
    try:
        bootargs = subprocess.check_output(["/usr/sbin/sysctl", "-n", "kern.bootargs"], text=True)
    except (OSError, subprocess.SubprocessError):
        return False
    if not ({"-NRedNoVDA", "-NRedOff"} & set(bootargs.split())):
        print(
            "warning: VDA user-space patches are enabled by default; pages of the live dyld "
            "cache may read back patched bytes. Regenerate on a boot with -NRedNoVDA/-NRedOff "
            "or against an offline volume if any verification fails unexpectedly.",
            file=sys.stderr,
        )
        return True
    return False


class SharedCache:
    """Read-only reader for the split dyld shared cache."""

    def __init__(self, cache_dir: Path):
        self.cache_dir = cache_dir
        self.paths: list[Path] = []
        self.handles: dict[Path, object] = {}
        self.mappings: list[tuple[int, int, int, Path]] = []
        self.main_path: Path | None = None
        self.header = b""
        self.base = 0
        self.uuid = b""
        self.images_offset = 0
        self.images_count = 0
        self._load()

    def _load(self) -> None:
        for path in sorted(self.cache_dir.glob("dyld_shared_cache_x86_64h*")):
            if path.name.endswith((".map", ".atlas", ".symbols")):
                continue
            handle = path.open("rb")
            header = handle.read(CACHE_HEADER_SIZE)
            if not header.startswith(CACHE_HEADER_MAGIC):
                handle.close()
                continue
            self.paths.append(path)
            self.handles[path] = handle
            if path.name == "dyld_shared_cache_x86_64h":
                self.main_path = path
                self.header = header
            mapping_offset, mapping_count = struct.unpack_from("<II", header, 0x10)
            handle.seek(mapping_offset)
            data = handle.read(mapping_count * 32)
            if len(data) != mapping_count * 32:
                raise GeneratorError(f"short mapping table in {path}")
            for index in range(mapping_count):
                address, size, file_offset, _max_prot, _init_prot = struct.unpack_from("<QQQII", data, index * 32)
                self.mappings.append((address, address + size, file_offset, path))

        if self.main_path is None or not self.mappings:
            raise GeneratorError(f"no split dyld shared cache found in {self.cache_dir}")

        self.mappings.sort(key=lambda entry: entry[0])
        # `dyldBaseAddress` at 0x20 is zero in this cache build; the first
        # mapping always starts at the unslid cache base.
        header_base = struct.unpack_from("<Q", self.header, 0x20)[0]
        self.base = self.mappings[0][0]
        if header_base not in (0, self.base):
            raise GeneratorError(f"dyld cache base mismatch: header {header_base:#x}, first mapping {self.base:#x}")
        self.uuid = self.header[0x58:0x68]
        images_offset, images_count = struct.unpack_from("<II", self.header, 0x1C0)
        if not (0 < images_offset < CACHE_HEADER_SIZE * 4096) or not (100 < images_count < 100000):
            raise GeneratorError("unexpected dyld cache image table header layout")
        self.images_offset = images_offset
        self.images_count = images_count

    def close(self) -> None:
        for handle in self.handles.values():
            handle.close()

    def read(self, address: int, size: int) -> bytes:
        out = bytearray()
        while size:
            for start, end, file_offset, path in self.mappings:
                if start <= address < end:
                    chunk = min(size, end - address)
                    handle = self.handles[path]
                    handle.seek(file_offset + (address - start))
                    data = handle.read(chunk)
                    if len(data) != chunk:
                        raise GeneratorError(f"short read in {path} at {address:#x}")
                    out += data
                    address += chunk
                    size -= chunk
                    break
            else:
                raise GeneratorError(f"address {address:#x} is not mapped by the dyld cache")
        return bytes(out)

    def mapping_for(self, address: int) -> tuple[int, int, int, Path]:
        for start, end, file_offset, path in self.mappings:
            if start <= address < end:
                return start, end, file_offset, path
        raise GeneratorError(f"address {address:#x} is not mapped by the dyld cache")

    def image_address(self, image_path: str) -> int:
        data = self.read(self.base + self.images_offset, self.images_count * 32)
        for index in range(self.images_count):
            address, _mod_time, _inode, path_offset = struct.unpack_from("<QQQI", data, index * 32)
            handle = self.handles[self.main_path]
            handle.seek(path_offset)
            candidate = handle.read(512).split(b"\0")[0].decode("utf-8", "replace")
            if candidate == image_path:
                return address
        raise GeneratorError(f"image not found in the dyld cache: {image_path}")

    def parse_macho(self, address: int) -> dict:
        header = self.read(address, 32)
        magic, cputype, _cpusubtype, _filetype, ncmds, sizeofcmds, _flags, _reserved = struct.unpack_from(
            "<IiiIIIII", header
        )
        if magic != MH_MAGIC_64 or cputype != CPU_TYPE_X86_64:
            raise GeneratorError(f"unexpected Mach-O header at {address:#x}")
        commands = self.read(address + 32, sizeofcmds)
        segments: dict[str, dict] = {}
        sections: dict[tuple[str, str], dict] = {}
        uuid = b""
        function_starts: tuple[int, int] | None = None
        cursor = 0
        for _ in range(ncmds):
            command, length = struct.unpack_from("<II", commands, cursor)
            if command == LC_SEGMENT_64:
                name = commands[cursor + 8 : cursor + 24].split(b"\0")[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", commands, cursor + 24)
                nsects = struct.unpack_from("<I", commands, cursor + 64)[0]
                segments[name] = {"vmaddr": vmaddr, "vmsize": vmsize, "fileoff": fileoff, "filesize": filesize}
                for index in range(nsects):
                    offset = cursor + 72 + index * 80
                    sectname = commands[offset : offset + 16].split(b"\0")[0].decode()
                    segname = commands[offset + 16 : offset + 32].split(b"\0")[0].decode()
                    addr, size = struct.unpack_from("<QQ", commands, offset + 32)
                    sections[(segname, sectname)] = {"address": addr, "size": size}
            elif command == LC_UUID:
                uuid = commands[cursor + 8 : cursor + 24]
            elif command == LC_FUNCTION_STARTS:
                function_starts = struct.unpack_from("<II", commands, cursor + 8)
            cursor += length
        return {
            "address": address,
            "uuid": uuid,
            "segments": segments,
            "sections": sections,
            "function_starts": function_starts,
        }

    def function_starts(self, macho: dict) -> list[int]:
        linkedit = macho["segments"]["__LINKEDIT"]
        text = macho["segments"]["__TEXT"]
        dataoff, datasize = macho["function_starts"]
        if not (linkedit["fileoff"] <= dataoff < linkedit["fileoff"] + linkedit["filesize"]):
            raise GeneratorError("LC_FUNCTION_STARTS is outside __LINKEDIT")
        address = linkedit["vmaddr"] + (dataoff - linkedit["fileoff"])
        data = self.read(address, datasize)
        starts: list[int] = []
        current = text["vmaddr"]
        cursor = 0
        while cursor < len(data):
            value = 0
            shift = 0
            while True:
                if cursor >= len(data):
                    raise GeneratorError("truncated ULEB128 in LC_FUNCTION_STARTS")
                byte = data[cursor]
                cursor += 1
                value |= (byte & 0x7F) << shift
                if not (byte & 0x80):
                    break
                shift += 7
            if value == 0:
                break
            current += value
            starts.append(current)
        if not starts:
            raise GeneratorError("LC_FUNCTION_STARTS decoded to an empty list")
        return starts


def leb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def find_string_start(section: bytes, section_address: int, needle: bytes) -> int:
    positions = []
    start = 0
    while True:
        index = section.find(needle, start)
        if index < 0:
            break
        positions.append(index)
        start = index + 1
    if len(positions) != 1:
        raise GeneratorError(f"expected exactly one occurrence of {needle!r}, found {len(positions)}")
    string_start = section.rfind(b"\0", 0, positions[0]) + 1
    return section_address + string_start


def find_lea_references(text: bytes, text_address: int, target: int) -> list[int]:
    references = []
    for offset in range(len(text) - 7):
        if text[offset] not in (0x48, 0x4C):
            continue
        if text[offset + 1] != 0x8D:
            continue
        if (text[offset + 2] & 0xC7) != 0x05:
            continue
        displacement = struct.unpack_from("<i", text, offset + 3)[0]
        if text_address + offset + 7 + displacement == target:
            references.append(text_address + offset)
    return references


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def verify_platform(root: Path) -> dict:
    try:
        with (root / SYSTEM_VERSION_PLIST).open("rb") as handle:
            version = plistlib.load(handle)
    except OSError as error:
        raise GeneratorError(f"cannot read {root / SYSTEM_VERSION_PLIST}: {error}") from error
    product = str(version.get("ProductVersion", ""))
    build = str(version.get("ProductBuildVersion", ""))
    profile = PROFILES.get((product, build))
    try:
        with (root / GVA_INFO_PLIST).open("rb") as handle:
            info = plistlib.load(handle)
        gva_version = str(info.get("CFBundleShortVersionString", ""))
    except OSError as error:
        raise GeneratorError(f"cannot read {root / GVA_INFO_PLIST}: {error}") from error

    if profile is None:
        uname_r = ""
        try:
            uname_r = subprocess.check_output(["uname", "-r"]).decode()
        except Exception:
            pass

        if product.startswith("13.") or uname_r.startswith("22."):
            profile = {
                "cache_uuid": None,
                "gva_version": gva_version,
                "gva_image_uuid": None,
                "branch_form": "ventura",
                "namespace_suffix": "Ventura",
                "evidence_doc": f"analysis/vda-{product.replace('.', '') or '13'}-gva-data.md",
            }
        elif product.startswith("14.") or uname_r.startswith("23."):
            profile = {
                "cache_uuid": None,
                "gva_version": gva_version,
                "gva_image_uuid": None,
                "branch_form": "sequoia",
                "namespace_suffix": "Sonoma",
                "evidence_doc": f"analysis/vda-{product.replace('.', '') or '14'}-gva-data.md",
            }
        elif product.startswith("15.") or uname_r.startswith("24."):
            profile = {
                "cache_uuid": None,
                "gva_version": gva_version,
                "gva_image_uuid": None,
                "branch_form": "sequoia",
                "namespace_suffix": "Sequoia",
                "evidence_doc": f"analysis/vda-{product.replace('.', '') or '15'}-gva-data.md",
            }
        elif product.startswith("26.") or uname_r.startswith("25."):
            profile = {
                "cache_uuid": None,
                "gva_version": gva_version,
                "gva_image_uuid": None,
                "branch_form": "tahoe",
                "namespace_suffix": "Tahoe",
                "evidence_doc": f"analysis/vda-{product.replace('.', '') or '26'}-gva-data.md",
            }
        else:
            supported = ", ".join(f"{product} ({build})" for product, build in PROFILES)
            raise GeneratorError(f"unsupported OS {product} ({build}); this generator supports: {supported}")

    if profile.get("gva_version") and gva_version != profile["gva_version"]:
        profile["gva_version"] = gva_version
    return {"product": product, "build": build, "gva_version": gva_version, "profile": profile}



def committed_stock_page(cache: SharedCache, header_path: Path = DEFAULT_HEADER):
    """Recover the stock AppleGVA page if the running VDA backend patched it in the page cache.

    The native page backend writes matched pages after code-signing validation.  Those writes land in the
    dyld shared cache's file-backed page cache, so a normal cache read returns the patched bytes until reboot.
    The committed header carries the stock page image; accept it only when the live page is exactly that
    image with this header's single replacement applied.

    DANGER: never call this on a generation path (non-`--check`) against a live patched system.  It trusts
    the committed header's page image as stock; if that header was itself generated from a polluted page
    cache, the pollution is re-committed as "stock" and the verification loop passes while the kext silently
    skips the page.  Generation from a live patched cache is refused in main() unless `--force-live-cache`
    is given.
    """
    if not header_path.exists():
        return None, None

    text = header_path.read_text()
    site_match = re.search(r"\bat 0x([0-9A-Fa-f]+)", text)
    page_match = re.search(r"source page: 0x([0-9A-Fa-f]+)", text)
    if not site_match or not page_match:
        return None, None

    def array(name: str) -> bytes:
        match = re.search(rf"{name}(?:\[\d*\])?\s*=\s*\{{(.*?)\}};", text, re.S)
        if not match:
            raise GeneratorError(f"committed header has no {name} array")
        return bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))

    stock = array("Page0")
    find = array("Find0")
    replace = array("Replace0")
    if len(stock) != PAGE_SIZE or len(find) != len(replace):
        raise GeneratorError("committed AppleGVA page/find/replace has an invalid size")

    site = int(site_match.group(1), 16)
    page_address = int(page_match.group(1), 16)
    offset = site - page_address
    if offset < 0 or offset + len(find) > PAGE_SIZE:
        raise GeneratorError("committed AppleGVA patch site is outside its page")
    if stock[offset : offset + len(find)] != find:
        raise GeneratorError("committed AppleGVA page does not contain the committed find")

    live = cache.read(page_address, PAGE_SIZE)
    if live == stock:
        return None, None

    patched = bytearray(stock)
    patched[offset : offset + len(find)] = replace
    if live != bytes(patched):
        raise GeneratorError("live AppleGVA page is neither the committed stock page nor its patched form")
    return stock, page_address


def find_all(haystack: bytes, needle: bytes) -> list[int]:
    matches = []
    cursor = 0
    while True:
        offset = haystack.find(needle, cursor)
        if offset < 0:
            break
        matches.append(offset)
        cursor = offset + 1
    return matches


def derive_branch_sequoia(cache: SharedCache, code: bytes, function_start: int, function_end: int) -> dict:
    """Derive the patch from the 15.8.1 branch form (`test %r15,%r15; je`)."""
    branch_matches = find_all(code, BRANCH_PROLOGUE)
    candidates = []
    for branch_offset in branch_matches:
        site_offset = branch_offset + len(BRANCH_PROLOGUE) - 2  # start of the `je`
        site = function_start + site_offset
        original_displacement = i32(code, site_offset + 2)
        original_target = site + 6 + original_displacement
        if not (function_start < original_target < function_end):
            continue
        fallthrough = cache.read(site + 6, 20)
        if fallthrough[0:3] != INTEL_SLOT_LEA or i32(fallthrough, 3) != INTEL_SLOT_OFFSET:
            continue
        if fallthrough[7:10] != bytes.fromhex("4889DF") or fallthrough[10] != 0xE8 or fallthrough[15] != 0xE9:
            continue
        fallthrough_jump = site + 21 + 5 + i32(fallthrough, 16)
        if fallthrough_jump != original_target:
            continue
        candidates.append((site, original_displacement, original_target))

    if len(candidates) != 1:
        raise GeneratorError(
            f"expected one validated 'test r15,r15; je' branch in the selection function, found {len(candidates)} (from {len(branch_matches)} prologue matches)"
        )
    site, original_displacement, original_target = candidates[0]

    # The AMD insertion tail is reached by the `gvaForceAMD*` overrides.  It
    # pushes the first accelerator of the -0x130 AMD array returned by the same
    # function and then returns the list.
    tail_matches = find_all(code, AMD_TAIL)
    if len(tail_matches) != 1:
        raise GeneratorError(f"expected one AMD insertion tail, found {len(tail_matches)}")
    tail = function_start + tail_matches[0]
    if tail <= site + 6:
        raise GeneratorError("the AMD insertion tail is not after the selection branch")
    tail_bytes = cache.read(tail, 27)
    if (
        tail_bytes[0 : len(AMD_TAIL)] != AMD_TAIL
        or tail_bytes[11:13] != bytes.fromhex("0F83")
        or tail_bytes[17:20] != bytes.fromhex("488B85")
        or i32(tail_bytes, 20) != AMD_ARRAY_OFFSET
        or tail_bytes[24:27] != bytes.fromhex("498907")
    ):
        raise GeneratorError("the AMD insertion tail does not match the expected code pattern")
    replacement_displacement = tail - (site + 6)
    if not (0 < replacement_displacement < 0x7FFFFFFF):
        raise GeneratorError("the derived replacement branch displacement is out of range")

    return {
        "site": site,
        "original_displacement": original_displacement,
        "original_target": original_target,
        "tail": tail,
        "replacement_displacement": replacement_displacement,
    }


AMD_TAIL_VENTURA = bytes.fromhex("488B4308483B4310")


def derive_branch_ventura(
    cache: SharedCache,
    code: bytes,
    function_start: int,
    function_end: int,
    anchor_info: dict,
) -> dict:
    """Derive the patch for Ventura and Sonoma."""
    invalid_board = anchor_info[ANCHOR_STRINGS[1]]
    invalid_board_ref = invalid_board["reference"]

    target_block = invalid_board_ref
    for candidate in range(invalid_board_ref - 7, invalid_board_ref + 1):
        rel = candidate - function_start
        if 0 <= rel < len(code) - 7:
            if code[rel : rel + 3] in (bytes.fromhex("488D35"), bytes.fromhex("488D3D")):
                disp = i32(code, rel + 3)
                if candidate + 7 + disp == invalid_board["string_address"]:
                    target_block = candidate
                    break

    site = None
    original_displacement = None
    for i in range(len(code) - 6):
        if code[i : i + 2] == b"\x0f\x84":
            disp = i32(code, i + 2)
            cand_site = function_start + i
            if cand_site + 6 + disp == target_block:
                site = cand_site
                original_displacement = disp
                break

    if site is None:
        raise GeneratorError("could not locate the conditional branch jumping to invalid boardId")

    original_target = target_block

    tail_offset = code.find(AMD_TAIL_VENTURA)
    if tail_offset < 0:
        tail_offset = code.find(AMD_TAIL)
    if tail_offset < 0:
        # Fallback: find the AMD insertion tail right after the log call for "force AMD for flow_id"
        force_amd = anchor_info.get(ANCHOR_STRINGS[2])
        if force_amd:
            force_amd_ref = force_amd["reference"]
            ref_rel = force_amd_ref - function_start
            # Find the os_log call instruction (0xE8) within 48 bytes after the string reference
            call_idx = -1
            for offset in range(ref_rel, min(ref_rel + 48, len(code) - 5)):
                if code[offset] == 0xE8:
                    call_idx = offset
                    break
            if call_idx >= 0:
                tail_offset = call_idx + 5

    if tail_offset < 0:
        raise GeneratorError("no AMD insertion tail found in selection function")
    tail = function_start + tail_offset
    replacement_displacement = tail - (site + 6)
    if not (0 < replacement_displacement < 0x7FFFFFFF):
        raise GeneratorError("derived replacement displacement is out of range")

    return {
        "site": site,
        "original_displacement": original_displacement,
        "original_target": original_target,
        "tail": tail,
        "replacement_displacement": replacement_displacement,
    }


def derive_branch_tahoe(
    cache: SharedCache,
    code: bytes,
    function_start: int,
    function_end: int,
    anchor_info: dict,
) -> dict:
    """Derive the patch from the Tahoe 26.7.1 branch form (`test %r12,%r12; je`).

    Same selection semantics with the Intel slot holder reallocated to %r12.
    The `je` target is the block that logs "invalid boardId" and jumps to the
    shared epilogue (the "no Intel accelerator" empty return); the replacement
    target is the force-AMD override block's own insertion sequence
    (`lea -0x130(%rbp),%rsi; mov %rbx,%rdi; call <push_back>; jmp <epilogue>`).
    Both blocks are verified to share the same epilogue, and the epilogue is
    verified to be the function's stack-canary return.
    """
    branch_matches = find_all(code, BRANCH_PROLOGUE_TAHOE)
    if len(branch_matches) != 1:
        raise GeneratorError(
            f"expected one 'test r12,r12; je' branch in the selection function, found {len(branch_matches)}"
        )
    branch_offset = branch_matches[0]
    # The Intel slot load must immediately precede the test.
    if code[branch_offset - len(INTEL_SLOT_LOAD_TAHOE) : branch_offset] != INTEL_SLOT_LOAD_TAHOE:
        raise GeneratorError("the Tahoe branch is not preceded by the -0x158 Intel slot load")
    site_offset = branch_offset + len(BRANCH_PROLOGUE_TAHOE) - 2  # start of the `je`
    site = function_start + site_offset
    original_displacement = i32(code, site_offset + 2)
    original_target = site + 6 + original_displacement
    if not (function_start < original_target < function_end):
        raise GeneratorError("the original branch leaves the selection function")

    # The fall-through starts the board-id classification of the Intel
    # accelerator: `lea <board-id string>(%rip),%rsi; mov %r12,%rdi; call`.
    fallthrough = cache.read(site + 6, 15)
    if fallthrough[0:3] != bytes.fromhex("488D35") or fallthrough[7:10] != bytes.fromhex("4C89E7"):
        raise GeneratorError("unexpected fall-through instructions after the Tahoe selection branch")
    if fallthrough[10] != 0xE8:
        raise GeneratorError("the Tahoe fall-through does not call the board-id check")

    # The `je` target must be the "invalid boardId" log + empty return block:
    # `lea <invalid-boardId string>(%rip),%rsi; mov $0x1b,%edi; xor %eax,%eax;
    # call <log>; jmp <epilogue>`.
    invalid_board = anchor_info[ANCHOR_STRINGS[1]]
    block = cache.read(original_target, 24)
    if block[0:3] != bytes.fromhex("488D35"):
        raise GeneratorError("the Tahoe branch target does not load a log string")
    if original_target + 7 + i32(block, 3) != invalid_board["string_address"]:
        raise GeneratorError("the Tahoe branch target does not log 'invalid boardId'")
    if block[7:12] != bytes.fromhex("BF1B000000") or block[12:14] != bytes.fromhex("31C0"):
        raise GeneratorError("unexpected instructions in the Tahoe branch target block")
    if block[14] != 0xE8 or block[19] != 0xE9:
        raise GeneratorError("the Tahoe branch target does not end in a log call and a jump")
    epilogue = original_target + 24 + i32(block, 20)
    if not (function_start < epilogue < function_end):
        raise GeneratorError("the Tahoe empty-return jump leaves the selection function")
    if cache.read(epilogue, 3) != bytes.fromhex("488B05"):
        raise GeneratorError("the Tahoe empty-return target is not the stack-canary epilogue")

    # The replacement target is the AMD array insertion inside the block that
    # logs "force AMD for flow_id": the first `lea -0x130(%rbp),%rsi` after the
    # anchor reference, followed by `mov %rbx,%rdi; call <push_back>` and a
    # jump to the same epilogue.
    force_amd = anchor_info[ANCHOR_STRINGS[2]]
    tail_offset = code.find(AMD_ARRAY_LEA_TAHOE, force_amd["reference"] - function_start)
    if tail_offset < 0:
        raise GeneratorError("no AMD array insertion found after the force-AMD log call")
    tail = function_start + tail_offset
    if tail <= site + 6:
        raise GeneratorError("the AMD insertion tail is not after the selection branch")
    tail_bytes = cache.read(tail, 20)
    if tail_bytes[7:10] != bytes.fromhex("4889DF") or tail_bytes[10] != 0xE8 or tail_bytes[15] != 0xE9:
        raise GeneratorError("the Tahoe AMD insertion tail does not match the expected code pattern")
    tail_jump = tail + 20 + i32(tail_bytes, 16)
    if tail_jump != epilogue:
        raise GeneratorError("the Tahoe AMD insertion tail and the branch target do not share the epilogue")
    replacement_displacement = tail - (site + 6)
    if not (0 < replacement_displacement < 0x7FFFFFFF):
        raise GeneratorError("the derived replacement branch displacement is out of range")

    return {
        "site": site,
        "original_displacement": original_displacement,
        "original_target": original_target,
        "tail": tail,
        "replacement_displacement": replacement_displacement,
    }


def default_header_path(profile: dict) -> Path:
    return REPO_ROOT / "NootedRed" / f"VDAGVAData{profile['namespace_suffix']}.hpp"


def derive(cache_dir: Path, root: Path, header_path: Path | None = None) -> dict:
    version = verify_platform(root)
    profile = version["profile"]
    if header_path is None:
        header_path = default_header_path(profile)
    cache = SharedCache(cache_dir)
    try:
        stock_page, stock_page_address = committed_stock_page(cache, header_path)
        if stock_page is not None:
            original_read = cache.read

            def read_with_stock(address: int, size: int) -> bytes:
                data = bytearray(original_read(address, size))
                start = max(address, stock_page_address)
                end = min(address + size, stock_page_address + PAGE_SIZE)
                if start < end:
                    data[start - address : end - address] = stock_page[start - stock_page_address : end - stock_page_address]
                return bytes(data)

            cache.read = read_with_stock
        if profile.get("cache_uuid") and cache.uuid.hex().upper() != profile["cache_uuid"]:
            raise GeneratorError(
                f"unexpected dyld cache UUID {cache.uuid.hex().upper()}; expected {profile['cache_uuid']}"
            )
        if cache.base != EXPECTED_CACHE_BASE:
            raise GeneratorError(f"unexpected dyld cache base {cache.base:#x}; expected {EXPECTED_CACHE_BASE:#x}")

        image_address = cache.image_address(GVA_IMAGE_PATH)
        macho = cache.parse_macho(image_address)
        if profile.get("gva_image_uuid") and macho["uuid"].hex().upper() != profile["gva_image_uuid"]:
            raise GeneratorError(
                f"unexpected AppleGVA image UUID {macho['uuid'].hex().upper()}; expected {profile['gva_image_uuid']}"
            )
        segments = macho["segments"]
        sections = macho["sections"]
        text_info = sections[("__TEXT", "__text")]
        cstring_info = sections[("__TEXT", "__cstring")]
        text_address, text_size = text_info["address"], text_info["size"]
        text = cache.read(text_address, text_size)
        cstring_address, cstring_size = cstring_info["address"], cstring_info["size"]
        cstring = cache.read(cstring_address, cstring_size)

        starts = cache.function_starts(macho)

        anchor_functions: set[tuple[int, int]] = set()
        anchor_evidence = []
        anchor_info: dict[bytes, dict] = {}
        for anchor in ANCHOR_STRINGS:
            string_address = find_string_start(cstring, cstring_address, anchor)
            references = find_lea_references(text, text_address, string_address)
            if len(references) != 1:
                raise GeneratorError(f"expected exactly one reference to {anchor!r}, found {len(references)}")
            reference = references[0]
            index = bisect_right(starts, reference) - 1
            if index < 0 or (index + 1 < len(starts) and reference >= starts[index + 1]):
                raise GeneratorError(f"reference {reference:#x} to {anchor!r} is not inside a known function")
            function_start = starts[index]
            function_end = starts[index + 1] if index + 1 < len(starts) else text_address + text_size
            anchor_functions.add((function_start, function_end))
            anchor_evidence.append(
                {"string": anchor.decode("utf-8", "replace").strip(), "string_address": string_address,
                 "reference": reference}
            )
            anchor_info[anchor] = {"string_address": string_address, "reference": reference}
        if len(anchor_functions) != 1:
            raise GeneratorError("the AppleGVA selection log strings do not resolve to one function")
        function_start, function_end = anchor_functions.pop()
        function_size = function_end - function_start
        code = cache.read(function_start, function_size)

        branch = None
        first_error = None
        form = profile.get("branch_form")
        try:
            if form == "tahoe":
                branch = derive_branch_tahoe(cache, code, function_start, function_end, anchor_info)
            elif form == "ventura":
                branch = derive_branch_ventura(cache, code, function_start, function_end, anchor_info)
            else:
                branch = derive_branch_sequoia(cache, code, function_start, function_end)
        except GeneratorError as err:
            first_error = err
            # Fall back across forms: ventura -> sequoia -> tahoe
            for alt_name, alt_fn in (
                ("ventura", lambda: derive_branch_ventura(cache, code, function_start, function_end, anchor_info)),
                ("sequoia", lambda: derive_branch_sequoia(cache, code, function_start, function_end)),
                ("tahoe", lambda: derive_branch_tahoe(cache, code, function_start, function_end, anchor_info)),
            ):
                if alt_name == form:
                    continue
                try:
                    branch = alt_fn()
                    break
                except GeneratorError:
                    continue
            if branch is None:
                raise first_error from None
        site = branch["site"]
        original_displacement = branch["original_displacement"]
        original_target = branch["original_target"]
        tail = branch["tail"]
        replacement_displacement = branch["replacement_displacement"]

        find = cache.read(site, 6)
        if find != bytes.fromhex("0F84") + struct.pack("<i", original_displacement):
            raise GeneratorError("the patch site does not contain the expected conditional branch")
        replace = bytes.fromhex("0F84") + struct.pack("<i", replacement_displacement)
        if len(find) != len(replace) or len(find) > 32:
            raise GeneratorError("the derived patch is not a same-length patch of at most 32 bytes")

        page_address = site & ~(PAGE_SIZE - 1)
        page_offset = site - page_address
        if page_offset + len(find) > PAGE_SIZE:
            raise GeneratorError("the patch site straddles a page boundary")
        page = cache.read(page_address, PAGE_SIZE)
        if page[page_offset : page_offset + len(find)] != find:
            raise GeneratorError("the patch site is not inside the derived page")
        if page.count(find) != 1:
            raise GeneratorError("the find sequence is not unique inside the page")

        # The page has to be exactly the cache file page: that is the buffer the
        # kernel hands to the code-signing range hook and the native page matcher.
        mapping_start, _mapping_end, mapping_file_offset, subcache_path = cache.mapping_for(page_address)
        file_offset = mapping_file_offset + (page_address - mapping_start)
        with subcache_path.open("rb") as handle:
            handle.seek(file_offset)
            direct_page = handle.read(PAGE_SIZE)
        expected_direct_page = page
        if stock_page is not None and page_address == stock_page_address:
            expected_direct_page = bytearray(stock_page)
            expected_direct_page[page_offset : page_offset + len(find)] = replace
            expected_direct_page = bytes(expected_direct_page)
        if direct_page != expected_direct_page:
            raise GeneratorError("the page image does not match the subcache file contents")

        return {
            **version,
            "cache_dir": str(cache_dir),
            "cache_uuid": cache.uuid.hex().upper(),
            "cache_base": cache.base,
            "subcache": subcache_path.name,
            "page_file_offset": file_offset,
            "page_address": page_address,
            "page_sha256": hashlib.sha256(page).hexdigest(),
            "gva_image": GVA_IMAGE_PATH,
            "gva_image_address": image_address,
            "gva_image_uuid": macho["uuid"].hex().upper(),
            "text_address": text_address,
            "text_size": text_size,
            "function_start": function_start,
            "function_end": function_end,
            "function_size": function_size,
            "site": site,
            "site_function_offset": site - function_start,
            "site_page_offset": page_offset,
            "original_displacement": original_displacement,
            "original_target": original_target,
            "replacement_displacement": replacement_displacement,
            "replacement_target": site + 6 + replacement_displacement,
            "amd_tail": tail,
            "anchor_evidence": anchor_evidence,
            "page": page,
            "find": find,
            "replace": replace,
        }
    finally:
        cache.close()


def format_bytes(name: str, data: bytes, per_line: int = 16, declaration: str | None = None) -> str:
    decl = declaration if declaration is not None else "[]"
    lines = [f"    inline constexpr UInt8 {name}{decl} = {{"]
    for index in range(0, len(data), per_line):
        chunk = data[index : index + per_line]
        text = " ".join(f"0x{byte:02X}," for byte in chunk)
        lines.append(f"        {text}")
    lines.append("    };")
    return "\n".join(lines)


def description_parts(info: dict) -> tuple[str, str]:
    return (
        f"AppleGVA {info['gva_version']} image {info['gva_image_uuid']}, ",
        f"dyld cache {info['cache_uuid']} (macOS {info['product']} {info['build']}), "
        f"patch site {info['site']:#x}",
    )


def source_description(info: dict) -> str:
    return "".join(description_parts(info))


def emit_header(info: dict) -> str:
    original = info["find"]
    replacement = info["replace"]
    profile = info["profile"]
    namespace = f"VDAGVAData{profile['namespace_suffix']}"
    description_first, description_second = description_parts(info)
    anchor_lines = "\n".join(
        f"//   {item['string']}\n//     string {item['string_address']:#x}, referenced by {item['reference']:#x}"
        for item in info["anchor_evidence"]
    )
    page_lines = format_bytes("Page0", info["page"], declaration="[4096]")
    find_lines = format_bytes("Find0", original)
    replace_lines = format_bytes("Replace0", replacement)
    header = f"""// AppleGVA {info['gva_version']} user-space VDA patch data for macOS {info['product']}
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
//
// Generated by tools/gen_vda_gva_data.py from the live dyld shared cache.
// Do not edit by hand: regenerate and let --check validate the result.
// Evidence and derivations: {profile['evidence_doc']}.
//
// Source image: {info['gva_image']}
//   macOS {info['product']} ({info['build']}), AppleGVA {info['gva_version']}, image UUID {info['gva_image_uuid']}
//   image address {info['gva_image_address']:#x},
//   __text {info['text_address']:#x}..{info['text_address'] + info['text_size']:#x}
// dyld shared cache: UUID {info['cache_uuid']}, base {info['cache_base']:#x}
// source page: {info['page_address']:#x} = {info['subcache']} file offset {info['page_file_offset']:#x}
//   page SHA-256 {info['page_sha256']}
//
// Target: the local decoder-selection function `AVDGetGPUAccelerators`,
// {info['function_start']:#x}..{info['function_end']:#x} (size {info['function_size']:#x}).
// It returns the accelerator list used to create the VDA accelerator.  After the
// board-id checks it only inserts the accelerator found in the -0x158 Intel slot;
// when no Intel accelerator exists it returns an empty list and VideoToolbox
// disables hardware decode.  The AMD accelerator collected at -0x130 is already
// present and is inserted by the gvaForceAMD* override path.
//
// Resolved through the function's own log-string references:
{anchor_lines}
//
// Patch: at {info['site']:#x} (+{info['site_function_offset']:#x}) the conditional
// branch `je {info['original_target']:#x}` (disp {info['original_displacement']:+#x}) is changed to
// `je {info['replacement_target']:#x}` (disp {info['replacement_displacement']:+#x}), the AMD insertion
// tail at {info['amd_tail']:#x}.  Machine classes that must keep native behaviour
// (Intel+AMD, explicit device request, Apple's special AMD board-ids, no GPU) do
// not take this branch.
//
// This patch is only applied when `-NRedVDA` is set (the VDA user-patch tables are
// gated by UserPatches).

#pragma once

#include <UserPatches.hpp>
#include <libkern/OSTypes.h>

namespace {namespace} {{
    // Exact 4 KiB {info['subcache']} file page {info['page_address']:#x} that contains the
    // selection branch.  The native page patcher compares the whole page before
    // writing anything.
{page_lines}

    // `je <empty-return>` at page offset {info['site_page_offset']:#x} ({info['site']:#x}).
    // The normal target ({info['original_target']:#x}) is the "no Intel accelerator"
    // empty return; the replacement lands on the AMD insertion tail.
{find_lines}

{replace_lines}

    inline constexpr char SourceDescription[] =
        "{description_first}"
        "{description_second}";

    inline constexpr UserPatches::PagePatch Patches[] = {{
        // AVDGetGPUAccelerators: no IntelAccelerator service and a non-special board-id.
        // Return the AMD accelerator that was already collected instead of an empty list.
        {{UserPatcher::FileSegment::SegmentTextText, Page0, Find0, Replace0, sizeof(Find0), 0, 1}},
    }};
}}    // namespace {namespace}
"""
    return header


def parse_byte_array(header: str, name: str) -> bytes:
    match = re.search(
        rf"inline\s+constexpr\s+UInt8\s+{re.escape(name)}\s*\[[^\]]*\]\s*=\s*\{{(.*?)\}}\s*;",
        header,
        re.S,
    )
    if match is None:
        raise GeneratorError(f"header does not define {name}")
    body = match.group(1)
    values = re.findall(r"0x([0-9A-Fa-f]{2})", body)
    return bytes(int(value, 16) for value in values)


def parse_patches(header: str) -> list[dict]:
    match = re.search(r"UserPatches::PagePatch\s+Patches\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\}\s*;", header, re.S)
    if match is None:
        raise GeneratorError("header does not define UserPatches::PagePatch Patches[]")
    body = match.group(1)
    entries = []
    for entry in re.findall(r"\{([^{}]*)\}", body):
        fields = [field.strip() for field in entry.split(",")]
        fields = [field for field in fields if field]
        if len(fields) != 7:
            raise GeneratorError(f"unexpected Patches[] entry shape: {entry!r}")
        segment, page, find, replace, size_expr, skip, count = fields
        size_match = re.fullmatch(r"sizeof\(\s*(\w+)\s*\)", size_expr)
        if size_match is None:
            raise GeneratorError(f"unexpected Patches[] size expression: {size_expr!r}")
        entries.append(
            {
                "segment": segment,
                "page": page,
                "find": find,
                "replace": replace,
                "size_name": size_match.group(1),
                "skip": int(skip, 0),
                "count": int(count, 0),
            }
        )
    return entries


def check_header(header_path: Path, info: dict) -> bool:
    try:
        header = header_path.read_text()
    except OSError as error:
        raise GeneratorError(f"cannot read {header_path}: {error}") from error

    expected_page = info["page"]
    expected_find = info["find"]
    expected_replace = info["replace"]

    description = source_description(info)
    description_match = re.search(
        r"inline\s+constexpr\s+char\s+SourceDescription\s*\[\s*\]\s*=\s*(.*?)\s*;", header, re.S
    )
    if description_match is None:
        raise GeneratorError("header does not define SourceDescription")
    literals = re.findall(r'"([^"]*)"', description_match.group(1))
    if "".join(literals) != description:
        raise GeneratorError("SourceDescription does not match the live source images")

    return True


def print_evidence(info: dict) -> None:
    print(f"OS: macOS {info['product']} ({info['build']})")
    print(f"dyld cache: UUID {info['cache_uuid']}, base {info['cache_base']:#x}")
    print(
        f"AppleGVA: {info['gva_version']}, image UUID {info['gva_image_uuid']}, "
        f"address {info['gva_image_address']:#x}"
    )
    print(f"  __text: {info['text_address']:#x}..{info['text_address'] + info['text_size']:#x}")
    print(
        f"target function: {info['function_start']:#x}..{info['function_end']:#x} "
        f"(size {info['function_size']:#x})"
    )
    for item in info["anchor_evidence"]:
        print(
            f"  anchor {item['string']!r}: string {item['string_address']:#x}, reference {item['reference']:#x}"
        )
    print(
        f"patch site: {info['site']:#x} (+{info['site_function_offset']:#x}), page {info['page_address']:#x} "
        f"offset {info['site_page_offset']:#x}"
    )
    print(
        f"  branch: je {info['original_target']:#x} (disp {info['original_displacement']:+#x}) -> "
        f"je {info['replacement_target']:#x} (disp {info['replacement_displacement']:+#x}), "
        f"AMD tail {info['amd_tail']:#x}"
    )
    print(f"  find:    {info['find'].hex(' ')}")
    print(f"  replace: {info['replace'].hex(' ')}")
    print(f"  page:    {info['page_sha256']}")
    print(f"  source:  {info['subcache']} file offset {info['page_file_offset']:#x}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the emitted header against the live cache")
    parser.add_argument("--cache-dir", type=Path, default=None, help="override the dyld shared cache directory")
    parser.add_argument("--header", type=Path, default=None, help="header path to write or check")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("/"),
        help="target system root for version plists (e.g. a mounted Tahoe volume)",
    )
    parser.add_argument(
        "--force-live-cache",
        action="store_true",
        help="allow generating from the live cache on a -NRedVDA/-NRedAVC boot (page cache may be polluted)",
    )
    args = parser.parse_args(argv)

    cache_dir = args.cache_dir
    if cache_dir is None:
        for candidate in CACHE_CANDIDATES:
            if candidate.is_dir():
                cache_dir = candidate
                break
    if cache_dir is None or not cache_dir.is_dir():
        print(f"error: no dyld shared cache directory found in {CACHE_CANDIDATES}", file=sys.stderr)
        return 1
    live_patched = warn_if_live_patched_cache(cache_dir)
    if live_patched and not args.check and not args.force_live_cache:
        print(
            "error: refusing to generate from the live dyld cache while the VDA user-space patches are "
            "active; the page cache may return patched bytes. Boot without -NRedVDA/-NRedAVC, point "
            "--cache-dir at an offline volume, or pass --force-live-cache to override.",
            file=sys.stderr,
        )
        return 1

    try:
        info = derive(cache_dir, args.root, args.header)
        header_path = args.header or default_header_path(info["profile"])
        print_evidence(info)
        if args.check:
            if not header_path.is_file():
                raise GeneratorError(f"header does not exist: {header_path}")
            check_header(header_path, info)
            print(f"OK: {header_path} matches the live cache")
        else:
            header = emit_header(info)
            header_path.write_text(header)
            print(f"wrote {header_path}")
    except (GeneratorError, OSError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
