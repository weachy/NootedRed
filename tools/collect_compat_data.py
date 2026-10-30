#!/usr/bin/env python3
"""Collect the data needed to extend NootedRed's fixes to another macOS build or AMD APU.

This is the focused replacement for the older `collect_platform_data.py` (which is kept for the
historical datasets).  It is designed around the two features that are currently being extended:

  * the Chromium/Electron GL/Metal dual-source fix — needs the user-space driver images and the
    candidate patch sites; and
  * the VCN/VDA hardware codec support — needs the VDA kernel ABI data, the user-space VA/AppleGVA
    patch data and the media capability metadata.

Run it on the target machine (no root required).  Everything is read-only; the output contains Apple
binaries, so do not publish it.

Output layout (one directory, optionally zipped):

    manifest.json                 schema/tool version, host identity, file hashes, warnings
    metadata/                     OS, hardware (PII redacted), boot args, kexts, ioreg
    chromium/                     GL driver, MTL image, symbols, candidate sites, 4 KiB pages
    vda/                          generated VCN/VA/GVA headers, generator results, kernel info
    SHA256SUMS

Examples:

    python3 tools/collect_compat_data.py --out /tmp
    python3 tools/collect_compat_data.py --out /tmp --zip
    python3 tools/collect_compat_data.py --zip
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import platform
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
SCHEMA_VERSION = 2
TOOL_VERSION = "collect_compat_data 1.0"

PAGE = 0x1000
GL_DRIVER = Path(
    "/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver")
CACHE_CANDIDATES = (
    Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"),
    Path("/System/Cryptexes/OS/System/Library/dyld"),
    Path("/System/Library/dyld"),
)
MTL_IMAGE = "AMDRadeonX5000MTLDriver"
KERNEL_COLLECTION = Path("/System/Library/KernelCollections/SystemKernelExtensions.kc")

# Patterns that locate the validated 15.8.1 sites.  On another build the offsets move, so the collector
# reports every hit with its page image and lets the generator spec be written for the actual build.
CHROMIUM_PATTERNS = (
    {"name": "GL_HwInfo_numShaderEngines", "source": "gl", "find": "c7477804000000",
     "replace": "c7477801000000",
     "note": "A1: _gfx9_HwInfoInitData Vega10 branch, numShaderEngines 4 -> 1"},
    {"name": "GL_dualQuad", "source": "gl", "find": "83e0fe",
     "replace": "83c801",
     "note": "A2: _glrAMD_GFX9_UpdateHwCbDualQuadMode, and eax,~1 -> or eax,1 (validated skip=1)"},
    {"name": "MTL_gate", "source": "mtl", "find": "750b", "replace": "eb0b",
     "note": "Metal pipeline HwInfo+0xa0 gate (validated 15.8.1 page offset 0x164)"},
    {"name": "MTL_allow", "source": "mtl", "find": "0f94c0", "replace": "31c090",
     "note": "Metal allowRBPlus setz (validated 15.8.1 page offset 0x198)"},
)

# Result caps: a new build can have many matches; the point is to collect evidence, not to flood the
# archive.  Unique 4 KiB pages are stored once.
MAX_HITS_PER_PATTERN = 64
MAX_CONTEXT = 32

PII_KEYS = {
    "serial_number", "serialnumber", "hardware_uuid", "provisioning_udid", "platform_uuid",
    "platform_uuid", "system_uuid", "UUID", "udid",
}
NVRAM_KEYS_OF_INTEREST = {"boot-args", "csr-active-config", "prev-lang:kbd"}


def run(command: list[str], timeout: int = 120) -> dict:
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return {
            "command": command,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    except Exception as error:  # noqa: BLE001 - collector must never fail the whole run here
        return {"command": command, "returncode": -1, "stdout": "", "stderr": str(error)}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def get_macos_version(sys_root: Path = Path("/")) -> str:
    plist_path = sys_root / "System/Library/CoreServices/SystemVersion.plist"
    if plist_path.is_file():
        try:
            data = plistlib.loads(plist_path.read_bytes())
            ver = data.get("ProductUserVisibleVersion") or data.get("ProductVersion")
            if ver:
                return str(ver).strip()
        except Exception:
            pass
    if sys_root == Path("/") or sys_root == Path(""):
        ver = platform.mac_ver()[0]
        if ver:
            return ver.strip()
        try:
            completed = subprocess.run(["sw_vers", "-productVersion"], capture_output=True, text=True, timeout=5)
            if completed.returncode == 0 and completed.stdout.strip():
                return completed.stdout.strip()
        except Exception:
            pass
    return "unknown"


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, default=str) + "\n")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, errors="replace")


def redact_plist(value):
    if isinstance(value, dict):
        return {key: ("<redacted>" if key in PII_KEYS else redact_plist(item))
                for key, item in value.items()}
    if isinstance(value, list):
        return [redact_plist(item) for item in value]
    return value


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_cache_dir(override: Path | None, sys_root: Path = Path("/")) -> Path | None:
    if override is not None:
        return override if override.is_dir() else None
    candidates = [
        sys_root / "System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
        sys_root / "System/Cryptexes/OS/System/Library/dyld",
        sys_root / "System/Library/dyld",
    ]
    for path in candidates:
        if (path / "dyld_shared_cache_x86_64h.map").is_file():
            return path
    if sys_root.is_dir() and sys_root != Path("/"):
        for path in sys_root.glob("System/**/dyld_shared_cache_x86_64h.map"):
            return path.parent
    return None


class CacheReader:
    """Minimal dyld shared cache reader for a single image"""

    def __init__(self, cache_dir: Path):
        self.cache_dir = cache_dir
        self.maps: list[tuple[int, int, int, object]] = []
        self.handles = []
        self.cache_uuid = None
        self.cache_base = 0
        for path in sorted(cache_dir.glob("dyld_shared_cache_x86_64h*")):
            if path.suffix in (".map", ".atlas"):
                continue
            handle = path.open("rb")
            header = handle.read(0x68)
            if not header.startswith(b"dyld_v1"):
                handle.close()
                continue
            if self.cache_uuid is None and len(header) >= 0x68:
                self.cache_uuid = header[0x58:0x68]
                self.cache_base = struct.unpack_from("<Q", header, 0x20)[0]
            mapping_offset, mapping_count = struct.unpack_from("<II", header, 16)
            handle.seek(mapping_offset)
            raw = handle.read(mapping_count * 32)
            if len(raw) != mapping_count * 32:
                handle.close()
                continue
            self.handles.append(handle)
            for index in range(mapping_count):
                va, size, file_offset, _, _ = struct.unpack_from("<QQQII", raw, index * 32)
                self.maps.append((va, va + size, file_offset, handle))
        if not self.maps:
            raise RuntimeError(f"no dyld shared cache mappings in {cache_dir}")

    def read(self, address: int, size: int) -> bytes:
        for start, end, file_offset, handle in self.maps:
            if start <= address and address + size <= end:
                handle.seek(file_offset + address - start)
                data = handle.read(size)
                if len(data) == size:
                    return data
        raise ValueError(f"unmapped {address:#x} (+{size:#x})")

    def image_base(self) -> int:
        map_file = self.cache_dir / "dyld_shared_cache_x86_64h.map"
        lines = map_file.read_text(errors="replace").splitlines()
        for index, line in enumerate(lines):
            if line.strip() == MTL_IMAGE or line.strip().endswith("/" + MTL_IMAGE):
                for following in lines[index + 1:]:
                    fields = following.split()
                    if len(fields) >= 2 and fields[0] == "__TEXT":
                        return int(fields[1], 16)
        raise RuntimeError(f"{MTL_IMAGE} not found in cache map")

    def close(self):
        for handle in self.handles:
            handle.close()


def parse_macho(reader: CacheReader, base: int):
    header = reader.read(base, 32)
    magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from("<IIIIII", header)
    if magic != 0xFEEDFACF:
        raise RuntimeError(f"not Mach-O64 at {base:#x}")
    commands = reader.read(base + 32, sizeofcmds)
    segments, symtab, uuid_bytes = [], None, None
    offset = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", commands, offset)
        if cmd == 0x19:
            name, vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                "<16sQQQQ", commands, offset + 8)
            segments.append((name.rstrip(b"\0").decode(), vmaddr, vmsize, fileoff, filesize))
        elif cmd == 0x1B:
            uuid_bytes = commands[offset + 8:offset + 24]
        elif cmd == 2:
            symtab = struct.unpack_from("<IIII", commands, offset + 8)
        offset += size

    symbols = []
    if symtab is not None:
        symoff, nsyms, stroff, strsize = symtab
        linkedit = next((segment for segment in segments if segment[0] == "__LINKEDIT"), None)
        if linkedit is not None:
            symdata = reader.read(linkedit[1] + symoff - linkedit[3], nsyms * 16)
            strdata = reader.read(linkedit[1] + stroff - linkedit[3], strsize)
            for index in range(nsyms):
                strx, kind, _, _, value = struct.unpack_from("<IBBHQ", symdata, index * 16)
                end = strdata.find(b"\0", strx)
                symbols.append((value, kind, strdata[strx:end].decode(errors="replace")))
            symbols.sort()

    uuid_value = format_uuid(uuid_bytes) if uuid_bytes is not None and len(uuid_bytes) == 16 else None
    return segments, symbols, uuid_value


def format_uuid(raw: bytes) -> str:
    text = raw.hex().upper()
    return f"{text[0:8]}-{text[8:12]}-{text[12:16]}-{text[16:20]}-{text[20:32]}"


def find_all(haystack: bytes, needle: bytes, limit: int) -> tuple[list[int], bool]:
    offsets = []
    start = 0
    truncated = False
    while True:
        index = haystack.find(needle, start)
        if index < 0:
            return offsets, truncated
        if len(offsets) >= limit:
            return offsets, True
        offsets.append(index)
        start = index + 1


def collect_metadata(root: Path, warnings: list[str], sys_root: Path = Path("/")) -> dict:
    meta = root / "metadata"
    uname = run(["uname", "-a"])
    if sys_root != Path("/"):
        plist_path = sys_root / "System/Library/CoreServices/SystemVersion.plist"
        sw_text = ""
        if plist_path.is_file():
            try:
                plist_data = plistlib.loads(plist_path.read_bytes())
                sw_text = "\n".join(f"{k}: {v}" for k, v in sorted(plist_data.items()))
            except Exception as e:
                sw_text = f"Error reading {plist_path}: {e}"
        else:
            sw_text = f"Offline sys-root: {sys_root} (SystemVersion.plist missing)"
        write_text(meta / "sw_vers.txt", sw_text + "\n")
        write_text(meta / "uname.txt", f"Offline scan of {sys_root} from host: " + (uname.get("stdout", "") or uname.get("stderr", "")))
        boot_args = ""
        csr = ""
    else:
        versions = run(["sw_vers"])
        write_text(meta / "sw_vers.txt", versions.get("stdout", "") or versions.get("stderr", ""))
        write_text(meta / "uname.txt", uname.get("stdout", "") or uname.get("stderr", ""))
        boot_args = run(["nvram", "boot-args"]).get("stdout", "").strip()
        csr = run(["csrutil", "status"]).get("stdout", "").strip()
    write_text(meta / "boot-args.txt", boot_args + "\n")
    write_text(meta / "csrutil.txt", csr + "\n")

    hardware = run(["system_profiler", "SPHardwareDataType", "-json"]).get("stdout", "")
    displays = run(["system_profiler", "SPDisplaysDataType", "-json"]).get("stdout", "")
    kexts = run(["kmutil", "showloaded"]).get("stdout", "")
    kextstat = run(["kextstat"]).get("stdout", "")

    try:
        hardware_json = json.loads(hardware)
        hardware_json = redact_plist(hardware_json)
    except json.JSONDecodeError:
        hardware_json = {"raw": hardware}
        warnings.append("SPHardwareDataType is not valid JSON")
    write_json(meta / "hardware.json", hardware_json)

    try:
        write_json(meta / "displays.json", json.loads(displays))
    except json.JSONDecodeError:
        write_text(meta / "displays.txt", displays)

    write_text(meta / "kexts-kmutil.txt", kexts)
    write_text(meta / "kexts-kextstat.txt", kextstat)

    # Only the keys needed for compatibility work; full nvram can contain unrelated identifiers.
    nvram = run(["nvram", "-p"]).get("stdout", "")
    filtered = [line for line in nvram.splitlines()
                if any(key in line for key in NVRAM_KEYS_OF_INTEREST)]
    write_text(meta / "nvram-filtered.txt", "\n".join(filtered) + "\n")

    ioreg = run(["ioreg", "-lw0"], timeout=180).get("stdout", "")
    gpu = filter_lines(ioreg, ("AMDRadeonX5000", "AMDRadeonX6000", "GPUConfigurationVariable",
                               "IOAccelerator", "model", "VRAM"))
    media = filter_lines(ioreg, ("VCN", "VCE", "UVD", "AppleGVA", "VideoToolbox",
                                 "IOGVACodec", "IOGVABundleName", "IOGVA"))
    write_text(meta / "ioreg-gpu.txt", gpu)
    write_text(meta / "ioreg-media.txt", media)
    write_json(meta / "gpu-identity.json", collect_gpu_identity(meta))

    write_text(meta / "videotoolbox-ffmpeg.txt", run(["ffmpeg", "-hide_banner", "-hwaccels"]).get("stdout", ""))
    write_text(meta / "coremedia-defaults.txt", run(["defaults", "read", "com.apple.coremedia"]).get("stdout", ""))

    nootedred_loaded = "org.ChefKiss.NootedRed" in kexts or "org.ChefKiss.NootedRed" in kextstat
    flags = [flag for flag in ("-NRedGLFix", "-NRedVDA", "-NRedAVC", "-NRedAVCEncode", "-NRedNoSubmitGuard", "-NRedGLFixResolve", "-NRedGLFixResolveOnly")
             if flag in boot_args]
    environment = {
        "boot_args": boot_args,
        "csr": csr,
        "nootedred_loaded": nootedred_loaded,
        "userspace_patch_flags": flags,
        "offline_sys_root": str(sys_root) if sys_root != Path("/") else None,
        "patched_page_cache_risk": bool(nootedred_loaded and flags and sys_root == Path("/")),
    }
    if environment["patched_page_cache_risk"]:
        warnings.append(
            "NootedRed is loaded with user-space patches enabled: the GL driver and dyld cache page "
            "cache may contain patched bytes.  Reboot with -NRedOff (or remove NootedRed for that boot) "
            "before collecting data that will be used to generate new patch sets.")
    return environment


def filter_lines(text: str, needles: tuple[str, ...], limit: int = 512 * 1024) -> str:
    lines = []
    for line in text.splitlines():
        if any(needle in line for needle in needles):
            lines.append(line)
    return "\n".join(lines)[:limit]


def _decode_ioreg_value(value: str):
    """Decode the common ioreg property forms enough for identity fields."""

    value = value.strip()
    if value.startswith("<") and value.endswith(">"):
        raw = value[1:-1].strip()
        if raw.startswith('"') and raw.endswith('"'):
            return raw.strip('"')
        try:
            data = bytes.fromhex(raw)
        except ValueError:
            return raw
        if len(data) in (1, 2, 4, 8):
            return int.from_bytes(data, "little")
        return raw
    return value.strip('"')


def _parse_property_block(text: str) -> dict:
    values = {}
    for line in text.splitlines():
        match = re.search(r'"([^"]+)" = (.+)$', line.strip())
        if match:
            values.setdefault(match.group(1), _decode_ioreg_value(match.group(2)))
    return values


def collect_gpu_identity(meta: Path) -> dict:
    """Identity fields from the iGPU node, not from unrelated PCI devices."""

    identity: dict = {}
    for command in (
        ["ioreg", "-lw0", "-r", "-c", "IOPCIDevice", "-n", "IGPU"],
        ["ioreg", "-lw0", "-r", "-c", "IOPCIDevice", "-n", "GFX0"],
    ):
        text = run(command, timeout=120).get("stdout", "")
        if not text.strip():
            continue
        values = _parse_property_block(text)
        if "device-id" not in values:
            continue
        identity = {
            "node": command[-1],
            "device_id": values.get("device-id"),
            "revision_id": values.get("revision-id"),
            "subsystem_id": values.get("subsystem-id"),
            "model": values.get("model"),
            "class_code": values.get("class-code"),
        }
        break

    accelerator = _parse_property_block(run(["ioreg", "-lw0", "-r", "-c", "IOAccelerator"],
                                            timeout=120).get("stdout", ""))
    for key in ("IOGLBundleName", "MetalPluginName", "IOGVACodec", "IOGVABundleName",
                "VRAM,totalMB", "VRAM,totalBytes", "HWChannel VCN0Dec | Commands Submitted"):
        if key in accelerator:
            identity[key] = accelerator[key]

    display = run(["system_profiler", "SPDisplaysDataType", "-json"], timeout=120).get("stdout", "")
    try:
        displays = json.loads(display)
        for entry in displays.get("SPDisplaysDataType", []):
            for key in ("spdisplays_vram", "spdisplays_vram_shared", "spdisplays_device-id",
                        "spdisplays_gmux-version", "_name"):
                if key in entry:
                    identity.setdefault(key, entry[key])
    except json.JSONDecodeError:
        pass

    if not identity:
        identity = {"note": "iGPU identity not found in ioreg; see metadata/ioreg-gpu.txt"}
    return identity


COMMITTED_HEADER = REPO_ROOT / "NootedRed" / "UserPatches.cpp"
# Committed stock page images and the exact patch sites, used to reconstruct the vanilla driver pages when
# the collector runs on a boot where NootedRed already patched the file-backed page cache.
COMMITTED_GL_PAGES = (
    ("kGLDriverHwInfoPage", 0xE000, (("c7477804000000", "c7477801000000", 0),)),
    ("kGLDriverDualQuadPage", 0x8000, (("83e0fe", "83c801", 1),)),
)
COMMITTED_MTL_PAGE = ("kMTLControlPage", (("750b", "eb0b", 0, 0x164), ("0f94c0", "31c090", 0, 0x198)))


def load_committed_pages() -> dict[str, bytes]:
    if not COMMITTED_HEADER.is_file():
        return {}
    text = COMMITTED_HEADER.read_text(errors="replace")
    pages = {}
    for name in ("kGLDriverHwInfoPage", "kGLDriverDualQuadPage", "kMTLControlPage"):
        match = re.search(rf"static const UInt8 {name}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
        if not match:
            continue
        values = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1)))
        if len(values) == PAGE:
            pages[name] = values
    return pages


def apply_sites(page: bytes, sites) -> bytes | None:
    patched = bytearray(page)
    for find_hex, replace_hex, skip, *offset_hint in sites:
        find, replace = bytes.fromhex(find_hex), bytes.fromhex(replace_hex)
        if offset_hint:
            offset = offset_hint[0]
        else:
            hits = []
            start = 0
            while True:
                index = page.find(find, start)
                if index < 0:
                    break
                hits.append(index)
                start = index + 1
            if skip >= len(hits):
                return None
            offset = hits[skip]
        if page[offset:offset + len(find)] != find:
            return None
        patched[offset:offset + len(replace)] = replace
    return bytes(patched)


def restore_page(live_page: bytes, committed_page: bytes | None, sites) -> tuple[bytes | None, str]:
    if committed_page is None:
        return None, "no committed page"
    if live_page == committed_page:
        return committed_page, "stock"
    patched_expected = apply_sites(committed_page, sites)
    if patched_expected is not None and live_page == patched_expected:
        return committed_page, "patched"
    return None, "mismatch"


KNOWN_GL_SITES = (
    ("GL_HwInfo_numShaderEngines", 0xE000, 0x81, "c7477804000000", "c7477801000000"),
    ("GL_dualQuad", 0x8000, 0x160, "83e0fe", "83c801"),
)


def detect_patched_gl(gl_bytes: bytes, environment: dict, warnings: list[str],
                      reconstructed: bool = False) -> dict:
    """Detect whether the GL driver's file-backed page cache already contains our patch bytes."""

    observed: dict = {}
    for name, page_start, page_offset, find_hex, replace_hex in KNOWN_GL_SITES:
        offset = page_start + page_offset
        window = gl_bytes[offset:offset + len(find_hex) // 2]
        if window == bytes.fromhex(replace_hex):
            observed[name] = "patched"
        elif window == bytes.fromhex(find_hex):
            observed[name] = "stock"
        else:
            observed[name] = "unknown"
    patched = any(value == "patched" for value in observed.values())
    if patched:
        if reconstructed:
            warnings.append(
                "the live page cache is patched; the vanilla GL/MTL pages were reconstructed from the "
                "committed 15.8.1 data.  Re-check them before using this dataset for another build.")
        else:
            warnings.append(
                "the GL driver page at the validated 15.8.1 sites is already patched and could not be "
                "reconstructed; reboot with -NRedOff or without NootedRed before collecting")
    return {"sites": observed, "loaded_with_patches": environment.get("patched_page_cache_risk", False),
            "patched": patched or bool(environment.get("patched_page_cache_risk", False))}


def collect_chromium(root: Path, cache_dir: Path | None, warnings: list[str],
                     environment: dict, sys_root: Path = Path("/")) -> dict:
    chromium = root / "chromium"
    chromium.mkdir(parents=True, exist_ok=True)
    result: dict = {"gl": {}, "mtl": {}, "patterns": list(CHROMIUM_PATTERNS)}
    gl_driver = sys_root / "System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver"
    if not gl_driver.is_file():
        warnings.append(f"GL driver missing: {gl_driver}")
        return result

    gl_bytes = gl_driver.read_bytes()
    (chromium / "GLDriver.bin").write_bytes(gl_bytes)
    info_plist = gl_driver.parents[2] / "Contents" / "Info.plist"
    versions: dict = {}
    try:
        info = plistlib.loads(info_plist.read_bytes())
        versions = {
            "bundle_version": info.get("CFBundleVersion"),
            "short_version": info.get("CFBundleShortVersionString"),
            "get_info_string": info.get("CFBundleGetInfoString"),
        }
    except Exception:  # noqa: BLE001
        pass
    gl_uuid = macho_file_uuid(gl_bytes)
    write_text(chromium / "GLDriver-nm.txt", run(["nm", "-m", str(gl_driver)], timeout=180).get("stdout", ""))
    result["gl"] = {
        "path": str(gl_driver),
        "size": len(gl_bytes),
        "sha256": sha256(gl_bytes),
        "uuid": gl_uuid,
        **versions,
    }

    mtl_bytes = None
    mtl_base = 0
    if cache_dir is not None:
        reader = CacheReader(cache_dir)
        try:
            base = reader.image_base()
            mtl_base = base
            segments, symbols, mtl_uuid = parse_macho(reader, base)
            text = next((segment for segment in segments if segment[0] == "__TEXT"), None)
            if text is None:
                raise RuntimeError("MTL __TEXT segment not found")
            mtl_bytes = reader.read(text[1], text[2])
            (chromium / "MTL-image.bin").write_bytes(mtl_bytes)
            write_text(chromium / "MTL-symbols-all.txt",
                       "\n".join(f"{value:#x} {kind} {name}" for value, kind, name in symbols))
            write_json(chromium / "MTL-info.json", {
                "cache_dir": str(cache_dir),
                "cache_uuid": format_uuid(reader.cache_uuid) if reader.cache_uuid else None,
                "cache_base": f"{reader.cache_base:#x}",
                "image_base": f"{base:#x}",
                "image_uuid": mtl_uuid,
                "image_size": len(mtl_bytes),
                "image_sha256": sha256(mtl_bytes),
                "segments": [
                    {"name": name, "vmaddr": f"{vmaddr:#x}", "vmsize": f"{vmsize:#x}",
                     "fileoff": f"{fileoff:#x}", "filesize": f"{filesize:#x}"}
                    for name, vmaddr, vmsize, fileoff, filesize in segments
                ],
            })
            result["mtl"] = {"image_base": f"{base:#x}", "size": len(mtl_bytes),
                             "sha256": sha256(mtl_bytes), "uuid": mtl_uuid}
        except Exception as error:  # noqa: BLE001
            warnings.append(f"MTL image extraction failed: {error}")
        finally:
            reader.close()
    else:
        warnings.append("dyld shared cache not found; MTL data not collected")

    committed = load_committed_pages()
    reconstruction: dict = {}
    stock_gl, stock_mtl = gl_bytes, mtl_bytes

    if committed:
        gl_stock = bytearray(gl_bytes)
        for name, page_start, sites in COMMITTED_GL_PAGES:
            restored, state = restore_page(gl_bytes[page_start:page_start + PAGE], committed.get(name), sites)
            reconstruction[name] = state
            if restored is not None and state == "patched":
                gl_stock[page_start:page_start + PAGE] = restored
        if bytes(gl_stock) != gl_bytes:
            stock_gl = bytes(gl_stock)
            (chromium / "GLDriver.stock.bin").write_bytes(stock_gl)

        if mtl_bytes is not None:
            mtl_stock = bytearray(mtl_bytes)
            for page_start in range(0, len(mtl_bytes) - PAGE + 1, PAGE):
                restored, state = restore_page(mtl_bytes[page_start:page_start + PAGE],
                                               committed.get(COMMITTED_MTL_PAGE[0]),
                                               COMMITTED_MTL_PAGE[1])
                if restored is not None and state == "patched":
                    mtl_stock[page_start:page_start + PAGE] = restored
                    reconstruction["kMTLControlPage"] = state
                    break
            if bytes(mtl_stock) != mtl_bytes:
                stock_mtl = bytes(mtl_stock)
                (chromium / "MTL-image.stock.bin").write_bytes(stock_mtl)

    result["reconstruction"] = reconstruction
    if stock_gl is not gl_bytes:
        result["gl"]["stock_sha256"] = sha256(stock_gl)
        result["gl"]["stock_size"] = len(stock_gl)
    if stock_mtl is not mtl_bytes and stock_mtl is not None:
        result["mtl"]["stock_sha256"] = sha256(stock_mtl)
        result["mtl"]["stock_size"] = len(stock_mtl)
    sites = scan_chromium_sites(chromium, stock_gl, stock_mtl, mtl_base, warnings)
    reconstructed = stock_gl is not gl_bytes or stock_mtl is not mtl_bytes
    patched = detect_patched_gl(gl_bytes, environment, warnings, reconstructed)
    result["patched_page_cache"] = patched
    sites["patched_page_cache"] = patched
    write_json(chromium / "sites.json", sites)
    write_json(chromium / "spec-hints.json", {
        "how_to_use": "Pick one candidate per pattern from sites.json, confirm the surrounding "
                      "instructions with a disassembler, then write a spec for tools/gen_patch_data.py "
                      "(see tools/README-patchgen.md).  The page images are stored in chromium/pages/.",
        "validated_15_8_1": {
            "GL_HwInfo_numShaderEngines": {"file_offset": "0xe000", "page_offset": "0x81", "skip": 0},
            "GL_dualQuad": {"file_offset": "0x8000", "page_offset": "0x160", "skip": 1},
            "MTL_gate": {"page_offset": "0x164", "skip": 0},
            "MTL_allow": {"page_offset": "0x198", "skip": 0},
        },
        "patterns": list(CHROMIUM_PATTERNS),
    })
    return result


def macho_file_uuid(data: bytes) -> str | None:
    try:
        ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    except struct.error:
        return None
    offset = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        if cmd == 0x1B and size >= 24:
            return format_uuid(data[offset + 8:offset + 24])
        offset += size
    return None


def scan_chromium_sites(root: Path, gl_bytes: bytes | None, mtl_bytes: bytes | None,
                        mtl_base: int, warnings: list[str]) -> dict:
    pages_dir = root / "pages"
    pages_dir.mkdir(parents=True, exist_ok=True)
    saved_pages: dict[str, str] = {}
    sites: dict = {"hits": [], "pages": {}}
    for pattern in CHROMIUM_PATTERNS:
        name = pattern["name"]
        source = pattern["source"]
        data = gl_bytes if source == "gl" else mtl_bytes
        if data is None:
            continue
        hits, truncated = find_all(data, bytes.fromhex(pattern["find"]), MAX_HITS_PER_PATTERN)
        for offset in hits:
            page_start = offset & ~(PAGE - 1)
            page_offset = offset - page_start
            if source == "gl":
                page = data[page_start:page_start + PAGE]
                page_id = f"gl-{page_start:08x}"
            else:
                page = data[page_start:page_start + PAGE]
                page_id = f"mtl-{page_start:08x}"
            if len(page) != PAGE:
                continue
            digest = sha256(page)
            if digest not in saved_pages:
                relative = f"pages/{digest[:16]}.bin"
                (root / relative).write_bytes(page)
                saved_pages[digest] = relative
                sites["pages"][digest] = relative
            context_start = max(0, offset - 8)
            context = data[context_start:context_start + MAX_CONTEXT]
            hits_entry = {
                "pattern": name,
                "source": source,
                "find": pattern["find"],
                "replace": pattern["replace"],
                "offset": offset,
                "page_start": page_start,
                "page_offset": page_offset,
                "page_sha256": digest,
                "page_file": saved_pages[digest],
                "context_hex": context.hex(),
            }
            if source == "mtl" and mtl_base:
                hits_entry["va"] = f"{mtl_base + offset:#x}"
            sites["hits"].append(hits_entry)
        sites.setdefault("counts", {})[name] = {
            "count": len(hits),
            "truncated": truncated,
        }
    return sites


def collect_vda(root: Path, cache_dir: Path | None, warnings: list[str],
                include_generators: bool, include_kc: bool, sys_root: Path = Path("/")) -> dict:
    vda = root / "vda"
    generated = vda / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    result: dict = {"kernel": {}, "generated": [], "firmware": []}
    kc_file = sys_root / "System/Library/KernelCollections/SystemKernelExtensions.kc"

    # Kernel source identification: parsed independently of the generator so a new build still leaves
    # useful evidence behind when the header generation refuses to run.
    try:
        module = load_module("nred_vcn_gen", TOOLS_DIR / "gen_vcn_kernel_data.py")
        actual_kc = kc_file if kc_file.is_file() else module.DEFAULT_KC
        kc = module.KernelCollection(actual_kc)
        result["kernel"] = module.collect_source_info(kc)
        write_json(vda / "kernel-info.json", result["kernel"])
        try:
            data = module.build_data(kc)
            write_json(vda / "kernel-abi.json", data)
        except Exception as error:  # noqa: BLE001
            warnings.append(f"VCN kernel ABI extraction failed: {error}")
    except Exception as error:  # noqa: BLE001
        warnings.append(f"kernel collection parsing failed: {error}")

    if include_kc and kc_file.is_file():
        destination = vda / "SystemKernelExtensions.kc"
        try:
            shutil.copyfile(kc_file, destination)
            result["kernel_collection"] = {"path": str(kc_file),
                                           "size": destination.stat().st_size}
            warnings.append("kernel collection copied; the archive will be large")
        except OSError as error:
            warnings.append(f"could not copy kernel collection: {error}")

    if include_generators:
        os_ver = get_macos_version(sys_root)
        if os_ver.startswith("13."):
            os_suffix = "Ventura"
        elif os_ver.startswith("14."):
            os_suffix = "Sonoma"
        elif os_ver.startswith("15."):
            os_suffix = "Sequoia"
        elif os_ver.startswith("26."):
            os_suffix = "Tahoe"
        else:
            os_suffix = ""

        va_name = f"VDAVAData{os_suffix}.hpp" if os_suffix else "VDAVAData.hpp"
        gva_name = f"VDAGVAData{os_suffix}.hpp" if os_suffix else "VDAGVAData.hpp"
        vcn_name = f"VCNKernelData{os_suffix}.hpp" if (os_suffix in ("Ventura", "Tahoe")) else "VCNKernelData.hpp"

        generator_specs = [
            (vcn_name, "gen_vcn_kernel_data.py", "--output"),
            (va_name, "gen_vda_va_data.py", "--output"),
            (gva_name, "gen_vda_gva_data.py", "--header"),
        ]
        # Also include standard names if different
        if os_suffix:
            for fallback_name, script, flag in (
                ("VCNKernelData.hpp", "gen_vcn_kernel_data.py", "--output"),
                ("VDAVAData.hpp", "gen_vda_va_data.py", "--output"),
                ("VDAGVAData.hpp", "gen_vda_gva_data.py", "--header"),
            ):
                if fallback_name not in [s[0] for s in generator_specs]:
                    generator_specs.append((fallback_name, script, flag))

        for name, script, output_flag in generator_specs:
            script_path = TOOLS_DIR / script
            output_path = generated / name
            committed = REPO_ROOT / "NootedRed" / name

            # Pre-seed output with committed header if available
            if not output_path.exists() and committed.is_file():
                shutil.copyfile(committed, output_path)

            command = [sys.executable, str(script_path), output_flag, str(output_path)]
            if script == "gen_vcn_kernel_data.py" and kc_file.is_file():
                command.extend(["--kc", str(kc_file)])
            elif cache_dir is not None:
                command.extend(["--cache-dir", str(cache_dir)])
            outcome = run(command, timeout=900)
            entry = {
                "name": name,
                "returncode": outcome["returncode"],
                "command": command,
                "stdout": outcome["stdout"][-20000:],
                "stderr": outcome["stderr"][-20000:],
                "output": str(output_path) if output_path.is_file() else None,
            }

            if outcome["returncode"] != 0:
                # A patched page cache makes live reads return our replacements; the committed header is
                # then the only usable source. Record a --check verdict and keep the verified header.
                check_command = [sys.executable, str(script_path), "--check"]
                if script == "gen_vcn_kernel_data.py" and kc_file.is_file():
                    check_command.extend(["--kc", str(kc_file)])
                elif cache_dir is not None:
                    check_command.extend(["--cache-dir", str(cache_dir)])
                if committed.is_file():
                    check_command.extend([output_flag, str(committed)])
                check = run(check_command, timeout=900)
                entry["check_returncode"] = check["returncode"]
                entry["check_stdout"] = check["stdout"][-20000:]
                entry["check_stderr"] = check["stderr"][-20000:]
                if check["returncode"] == 0 and committed.is_file():
                    shutil.copyfile(committed, output_path)
                    entry["output"] = str(output_path)
                    entry["fallback"] = "committed header verified by --check against the live system"
                    warnings.append(
                        f"{name}: generation failed but the committed header matches the live system; "
                        "kept the verified header (a pristine boot is needed to regenerate data)")
                else:
                    warnings.append(
                        f"{name}: generation and --check both failed; the live page cache or system "
                        "build differs (use --include-kc and/or a pristine boot without NootedRed)")
            result["generated"].append(entry)
        write_json(vda / "generator-results.json", result["generated"])

    firmware = collect_firmware(vda)
    result["firmware"] = firmware
    return result


def collect_firmware(root: Path, sys_root: Path = Path("/")) -> list[dict]:
    collected = []
    extensions = sys_root / "System/Library/Extensions"
    for bundle in sorted(extensions.glob("AMDRadeon*.kext")):
        resources = bundle / "Contents" / "Resources"
        if not resources.is_dir():
            continue
        for resource in sorted(resources.iterdir()):
            if not resource.is_file() or resource.suffix.lower() not in (".dat", ".bin", ".fw"):
                continue
            if resource.stat().st_size > 16 * 1024 * 1024:
                continue
            destination = root / "firmware" / resource.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(resource.read_bytes())
            collected.append({"name": resource.name, "sha256": sha256(destination.read_bytes())})
    return collected


def collect_manifest(root: Path, args, metadata: dict, chromium: dict, vda: dict,
                     warnings: list[str]) -> dict:
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.name not in ("manifest.json", "SHA256SUMS"):
            relative = path.relative_to(root).as_posix()
            files.append({"path": relative, "size": path.stat().st_size,
                          "sha256": sha256(path.read_bytes())})
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "tool": TOOL_VERSION,
        "collected_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "boot_args": metadata.get("boot_args"),
        "environment": metadata,
        "chromium": chromium,
        "vda": vda,
        "options": {"include_kc": bool(args.include_kc),
                    "include_generators": not args.no_generators},
        "warnings": warnings,
        "files": files,
    }
    write_json(root / "manifest.json", manifest)
    write_text(root / "SHA256SUMS",
               "\n".join(f"{entry['sha256']}  {entry['path']}" for entry in files) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Collect compatibility data for a macOS build/APU")
    parser.add_argument("--sys-root", type=Path, default=Path("/"),
                        help="system root directory (e.g. /Volumes/Macintosh\\ HD for an external disk, default: /)")
    parser.add_argument("--out", type=Path, default=Path("."),
                        help="destination parent directory (default: current directory, generating nred-compat-<ver>)")
    parser.add_argument("--zip", action="store_true", help="also create <out>.zip")
    parser.add_argument("--cache-dir", type=Path, help="override the dyld shared cache directory")
    parser.add_argument("--include-kc", action="store_true",
                        help="copy the whole kernel collection (large; needed for a new macOS build)")
    parser.add_argument("--no-generators", action="store_true",
                        help="do not try to run the VCN/VA/GVA generators")
    args = parser.parse_args()

    sys_root = args.sys_root.resolve()
    macos_ver = get_macos_version(sys_root)
    default_name = f"nred-compat-{macos_ver}"

    out_arg = args.out
    if out_arg.name.startswith("nred-compat"):
        root = out_arg
    else:
        root = out_arg / default_name

    if root.exists():
        raise SystemExit(f"output already exists: {root}")
    root.mkdir(parents=True)
    warnings: list[str] = []

    cache_dir = find_cache_dir(args.cache_dir, sys_root)
    if cache_dir is None:
        warnings.append("dyld shared cache not found; Chromium/MTL and VA/GVA data will be incomplete")

    metadata = collect_metadata(root, warnings, sys_root)
    chromium = collect_chromium(root, cache_dir, warnings, metadata, sys_root)
    vda = collect_vda(root, cache_dir, warnings, not args.no_generators, args.include_kc, sys_root)
    manifest = collect_manifest(root, args, metadata, chromium, vda, warnings)

    if args.zip:
        archive_path = root.with_name(f"{root.name}.zip")
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(root))
        print(f"zip     : {archive_path} ({archive_path.stat().st_size} bytes)")

    for warning in manifest["warnings"]:
        print(f"warning : {warning}", file=sys.stderr)
    print(f"out     : {root} ({len(manifest['files'])} files)")
    print(f"build   : see metadata/sw_vers.txt")
    print(f"gl fix  : {chromium.get('gl', {}).get('uuid')} "
          f"({chromium.get('counts', 'see chromium/sites.json')})")
    print(f"vda     : see vda/generated and vda/generator-results.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
