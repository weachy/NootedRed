#!/usr/bin/env python3
"""Derive/check the bounded VA/GVA page manifest; never scan the whole cache.

Only patch windows are normalized. Original and already patched windows are
accepted explicitly; other bytes are never "recovered" or described as stock.
The resulting SHA-256 fingerprints describe the observed bytes OUTSIDE those
windows. New OS/image identities require review and a new checked-in manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct
import uuid

from gen_vda_va_data import DEFAULT_CACHE_DIR, DyldCache, MachOImage, VA_IMAGE_PATH
from hevc_bitstream_patch import BUILDER, PACKET_WRITER, patches as hevc_bitstream_patches

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "NootedRed/MediaPatchManifest.json"
HEADER = ROOT / "NootedRed/MediaPatchManifest.inc"
GVA_PATH = "/System/Library/PrivateFrameworks/AppleGVA.framework/Versions/A/AppleGVA"

# Each required VA site is tied to a named function, not an arbitrary occurrence
# of a short constant-return stub in another image.
SITES = [
    ("Identify", "Identify", 17, "__ZN17VAAcceleratorInfo8identifyEjj"),
    ("AddrLibInit", "AddrLibInit", 9, "__ZN18VAAddrLibInterface4initEP11VAIOConnectP13sHardwareInfo"),
    ("AddrLibCreate", "AddrLibCreate", 2, "__ZN4Addr3Lib6CreateEPK18_ADDR_CREATE_INPUTP19_ADDR_CREATE_OUTPUT"),
    ("ConvertFamily", "ConvertFamily_Modern", 0, "__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj"),
    ("EngineAndBlt", "EngineAndBlt", 0, "__ZN9VAFactory20createGraphicsEngineEP9VAContext"),
    ("EngineAndBlt", "EngineAndBlt", 0, "__ZN9VAFactory14createImageBltEP9VAContextj"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory29createColourFormatConverterVPEi"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory17createDitheringVPEi"),
    ("VideoProcessors", "VideoProcessors", 7, "__ZN9VAFactory17createDeblockerVPEi"),
    ("EncFeedbackStride", "EncFeedbackStride", 4, "__ZN14Vcn2EncCommand15getFeedbackSizeEv"),
    ("StatusStride_Modern", "StatusStride_Modern", 3,
     "__ZN26AVDVcn2AvcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture"),
    ("StatusStride_Modern", "StatusStride_Modern", 3,
     "__ZN27AVDVcn2HevcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture"),
    ("SourcePitch_Modern", "SourcePitch_Modern", 0, "__ZN11Vcn2Encoder15setEncodeParamsEPvP15VAVendorTexture"),
]

# The native VCN2 constructor clears [0x38,0x80) and [0xdc,0x5c4), but leaves
# the four rate-control/quality parameter records in [0x80,0xdc) uninitialized.
# In particular, HEVC SetConfigParam(16) and checkRateControlPerPicUpdate read
# the existing per-picture packet and preserve its filler-data word. Extend
# the existing bzero down to 0x80, keeping its end at 0x5c4. Explicit parameter
# setters still run afterwards. This is independent of the HEVC height hang.
ENCODER_INIT_SITES = [
    ("__ZN14Vcn2EncCommandC2Ev", 0x18,
     "488dbbdc000000", "488dbb80000000"),
    ("__ZN14Vcn2EncCommandC2Ev", 0x35,
     "bee8040000", "be44050000"),
]

# Exact instruction windows for the reviewed 25G332 VCN2 encoder. These are
# additional required sites, not a fallback signature search on another build.
HEVC_ENCODE_SITES = [
    # The VCN2.2 square frame succeeds with the native 128 MiB allocation
    # when firmware sees a 64 MiB circular descriptor. The HEVC-only builder
    # call below applies that exact, temporary adjustment. See
    # analysis/hevc-square-repair-20261117.md. The separate height gate can
    # therefore admit the independently checked 4096x4096 Main/Main10 cases.
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0x1DF,
     "448b9b5cb30000", "41bb0010000090"),
    # Pass the actual source texture to the format builder. The old second
    # argument was an unused command-info pointer; the context can remain NV12
    # even when the source texture is P010.
    ("__ZN12VAUveEncoder13EncodePictureEPvP15VAVendorTextureS0_", 0x14A,
     "4c89ee", "4c89fe"),
    # NV12 and P010 share texture enum 0x13. Detect the two-byte source elements
    # instead, and set both 10-bit depth and P010 packing to 1. RGB has four-byte
    # elements and retains its context-based branches/constants. Store the
    # shared colour flags from EAX to make room for the texture-field check.
    ("__ZN11Vcn2Encoder29setInputAndOutputFormatParamsEP11_UveCmdInfo", 0x1F,
     "8b879c0000003d3032347874293d7230316c74113d626772617523c5fb10058ce20400"
     "eb08c5fb100572e20400b001b901000000eb0cc5fa100509e9030031c931c0894dc4894dcc",
     "83be8400000002742c8b879c0000003d7230316c74113d626772617521c5fb10058ae20400"
     "eb08c5fb100570e2040031c0ffc0eb0b660f76c0660f72d01f31c08945c48945cc9090"),
    # This VCN2 path uses PPS chroma offsets, but does not apply the separate
    # per-slice deltas to encoded pixels. Explicitly signal zero slice deltas
    # rather than AppleGVA's Main10 Cr=-3, which produces a visible colour error.
    # These loads execute only when the PPS enables per-slice chroma offsets.
    # PPS offsets and all luma/rate-control/deblock settings remain native.
    ("__ZN23AvdVcn2HevcHeaderEncode17encodeSliceHeaderEPvS0_S0_", 0x520,
     "418bb784070000", "31f69090909090"),
    ("__ZN23AvdVcn2HevcHeaderEncode17encodeSliceHeaderEPvS0_S0_", 0x530,
     "418bb788070000", "31f69090909090"),
]
# Renoir's VCN2.2 HEVC decoder supports the 8192x4352 rectangle. Apple's
# capability table already contains an 8192-wide internal-app limit, but the
# ordinary-client branch reads the adjacent 4096-wide field. Select the wider
# field and retain the existing minimum/profile/rate checks. Height and total
# pixel count are bounded explicitly, rather than skipping capability checks.
HEVC_DECODE_SITES = [
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0x2B7,
     "8b8320b30000", "8b8324b30000"),
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0x2D6,
     "8b8328b30000", "b80011000090"),
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0x2FA,
     "448ba304b30000", "41bc0000200290"),
]
AVC_DECODE_SITES = [
    # Allow portrait AVC while retaining the native Level-5.2 pixel budget.
    # The codec-7 firmware stores the 256*256 macroblock count in u16, then
    # rejects every slice against the wrapped zero count. See
    # analysis/avc-firmware-limit-20261117.md; a larger pixel budget alone
    # would also admit cropped sizes whose coded macroblocks still overflow.
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0xA7,
     "448bbb14b30000", "41bf0010000090"),
]
AVC_ENCODE_SITES = [
    # The AVC encoder height table retains 2304 despite working 4096x4096
    # Baseline/Main/High output. Preserve its width/profile checks. HEVC's
    # separate +0xB35C height field is changed above with its packet fix.
    ("__ZN13AvdCapability19checkCapabilityListEPK21AVD_DriverContextInfo", 0x14D,
     "448b9b54b30000", "41bb0010000090"),
]
# Apple's NumberOfSlices branch logs "unsupported" and returns success, although
# glob+0x10E68 already feeds the native encoder's SetParameter(8) during init.
# These two windows belong to the same UUID-pinned SetProperty function.
# Convert into its existing stack scratch first; failed conversions leave the
# configuration untouched. Save the count only before the first frame. The
# native init path validates/clamps it against macroblock rows and 256 entries
# before building matching slice headers. Once encoder+0x38 is set, return the
# existing -12915 error without mutating the session: this VCN path does not
# apply runtime changes even if its native setter reports success. A missing
# lazy encoder (glob+0x50 -> wrapper+0x3A8) can still save the initial request.
# Never write the driver's final +0x308 count and bypass native validation.
#
# The continuation occupies the adjacent MaxKeyFrameIntervalDuration diagnostic
# window. That property's conversion, value store, update flag, and return
# status are preserved; only its optional debug log is omitted.
GVA_ENCODE_SITES = [
    ("AVFQTXENC_SetProperty::NumberOfSlices", 0x7FFA0C5D6B39,
     "4180be82160100010f855c0e0000488d35c9080800e9ed060000",
     "488d5dc86a035e4c89ff535ae8b6b1060084c0eb46e99dfbffff"),
    ("AVFQTXENC_SetProperty::SliceContinuation", 0x7FFA0C5D6B75,
     "498d9ed8050100be060000004c89ff4889dae874b106004180be82160100017517"
     "f20f1003488d35ea060800bf1b000000b001e8bdb6060041c6862a18010001e9e90d0000",
     "498d96d80501006a065e4c89ffe879b1060041c6862a18010001e90f0e000074b8"
     "498b4650488b80a80300004885c074068378380075a28b03418986680e0100e9e90d0000"),
]
HEVC_PACKET_SITE_COUNT = 6  # HEVC builder call plus five executable alignment gaps.
REQUIRED_SITES = (14 + len(HEVC_ENCODE_SITES) + HEVC_PACKET_SITE_COUNT + len(HEVC_DECODE_SITES) +
                  len(AVC_DECODE_SITES) + len(AVC_ENCODE_SITES) +
                  len(GVA_ENCODE_SITES) + len(ENCODER_INIT_SITES))


def arrays():
    text = (ROOT / "NootedRed/VDASignatures.inc").read_text()
    return {
        name: bytes(int(n, 16) for n in re.findall(r"0x([0-9a-fA-F]{2})", re.sub(r"//[^\n]*", "", body)))
        for name, body in re.findall(r"UInt8\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S)
    }


def matches(data, context, mask, patch_offset, find, replace):
    result = []
    for offset in range(len(data) - len(context) + 1):
        window = data[offset:offset + len(context)]
        at = offset + patch_offset
        if data[at:at + len(find)] not in (find, replace):
            continue
        # Account only for the declared replacement; keep all surrounding guards.
        window = window[:patch_offset] + find + window[patch_offset + len(find):]
        if all((a & m) == (b & m) for a, b, m in zip(window, context, mask)):
            result.append(at)
    return result


def kernel_uuid(path):
    with path.open("rb") as file:
        header = file.read(32)
        if len(header) != 32 or struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
            raise ValueError("kernel must be a thin x86_64 Mach-O image")
        count, length = struct.unpack_from("<II", header, 16)
        if length > 0x10000:
            raise ValueError("unexpected kernel load-command size")
        data = file.read(length)
    offset, result = 0, None
    for _ in range(count):
        if len(data) - offset < 8:
            raise ValueError("truncated kernel load commands")
        command, length = struct.unpack_from("<II", data, offset)
        if length < 8 or length > len(data) - offset:
            raise ValueError("invalid kernel load command")
        if command == 0x1B:
            if length != 24 or result is not None:
                raise ValueError("invalid/duplicate kernel UUID")
            result = str(uuid.UUID(bytes=data[offset + 8:offset + 24])).upper()
        offset += length
    if result is None or offset != len(data):
        raise ValueError("missing UUID or inconsistent kernel command size")
    return result


def collect(cache_dir, build, dump_pages=None, kernel=Path("/System/Library/Kernels/kernel")):
    a = arrays()
    with DyldCache(cache_dir) as cache:
        va, gva = MachOImage(cache, VA_IMAGE_PATH), MachOImage(cache, GVA_PATH)
        info = {
            "schema": 1, "build": build, "kernel_uuid": kernel_uuid(kernel),
            "cache_uuid": str(uuid.UUID(bytes=cache.uuid)).upper(),
            "images": [
                {"path": p, "uuid": str(uuid.UUID(bytes=m.uuid)).upper(),
                 "text_start": hex(m.text[2]), "text_size": m.text[3]}
                for p, m in ((VA_IMAGE_PATH, va), (GVA_PATH, gva))
            ],
            "pages": [],
        }
        sites = []
        for suffix, context_suffix, patch_offset, symbol in SITES:
            function = va.symbol(symbol)
            end = va.function_end(function)
            data = cache.read(function, end - function)
            find, replace = a["kVDAFind_" + suffix], a["kVDAReplace_" + suffix]
            candidates = matches(data, a["kVDACtx_" + context_suffix], a["kVDAMask_" + context_suffix],
                                 patch_offset, find, replace)
            if len(candidates) != 1:
                raise ValueError(f"{symbol}: expected one guarded site, found {len(candidates)}")
            sites.append((function + candidates[0], find, replace, symbol))
        for symbol, offset, find_hex, replace_hex in (
                HEVC_ENCODE_SITES + HEVC_DECODE_SITES +
                AVC_DECODE_SITES + AVC_ENCODE_SITES + ENCODER_INIT_SITES):
            function = va.symbol(symbol)
            find, replace = bytes.fromhex(find_hex), bytes.fromhex(replace_hex)
            address = function + offset
            if (len(find) != len(replace) or address + len(find) > va.function_end(function) or
                    cache.read(address, len(find)) not in (find, replace)):
                raise ValueError(f"{symbol}+{offset:#x}: reviewed VA window differs")
            sites.append((address, find, replace, symbol))
        packet_sites = hevc_bitstream_patches(va.symbol(BUILDER), va.symbol(PACKET_WRITER))
        if len(packet_sites) != HEVC_PACKET_SITE_COUNT:
            raise ValueError("incomplete HEVC bitstream call/helper site set")
        for index, (address, find, replace) in enumerate(packet_sites):
            if (len(find) != len(replace) or
                    cache.read(address, len(find)) not in (find, replace)):
                raise ValueError(f"HEVC bitstream site {index}: reviewed VA window differs")
            if index and not (va.text[2] <= address and
                              address + len(find) <= va.text[2] + va.text[3]):
                raise ValueError(f"HEVC bitstream helper {index} outside executable text")
            sites.append((address, find, replace,
                          f"VCN2_HEVC_BITSTREAM_{'CALL' if index == 0 else 'HELPER_'+str(index)}"))
        # AppleGVA has stripped local symbols. Require exactly one guarded
        # selection branch inside its bounded __text section.
        suffix = "Tahoe"
        find, replace = a["kGVAFind_" + suffix], a["kGVAReplace_" + suffix]
        candidates = matches(gva.text_bytes(), a["kGVACtx_" + suffix], a["kGVAMask_" + suffix],
                             10, find, replace)
        if len(candidates) != 1:
            raise ValueError(f"AppleGVA: expected one guarded branch, found {len(candidates)}")
        sites.append((gva.text[2] + candidates[0], find, replace, "AVDGetGPUAccelerators"))
        for symbol, address, find_hex, replace_hex in GVA_ENCODE_SITES:
            find, replace = bytes.fromhex(find_hex), bytes.fromhex(replace_hex)
            if (len(find) != len(replace) or address < gva.text[2] or
                    address + len(find) > gva.text[2] + gva.text[3] or
                    cache.read(address, len(find)) not in (find, replace)):
                raise ValueError(f"{symbol}: reviewed AppleGVA window differs")
            sites.append((address, find, replace, symbol))
        pages = {}
        for address, find, replace, symbol in sites:
            page_address = address & ~4095
            start, _, file_offset, handle = cache.mapping_for(page_address)
            offset = file_offset + page_address - start
            key = (Path(handle.name).name, offset)
            if key not in pages:
                pages[key] = {"file": key[0], "offset": hex(offset), "sites": [],
                              "data": bytearray(cache.read_page(address))}
            page = pages[key]
            within = address - page_address
            if within + len(find) > 4096:
                raise ValueError(f"{symbol}: patch crosses a code-signing page")
            page["sites"].append({"offset": within, "find": find.hex(), "replace": replace.hex(), "symbol": symbol})
        for index, (_, page) in enumerate(sorted(pages.items())):
            page["sites"].sort(key=lambda s: s["offset"])
            data = page.pop("data")
            if dump_pages:
                dump_pages.mkdir(parents=True, exist_ok=True)
                (dump_pages / f"page-{index}.bin").write_bytes(data)
            previous_end = 0
            for site in page["sites"]:
                off, find, replace = site["offset"], bytes.fromhex(site["find"]), bytes.fromhex(site["replace"])
                if off < previous_end or len(find) != len(replace) or data[off:off + len(find)] not in (find, replace):
                    raise ValueError(f"invalid or overlapping site: {site['symbol']}")
                previous_end = off + len(find)
                data[off:previous_end] = bytes(len(find))
            page["masked_sha256"] = hashlib.sha256(data).hexdigest()
            info["pages"].append(page)
    if sum(len(p["sites"]) for p in info["pages"]) != REQUIRED_SITES:
        raise ValueError("incomplete VA/GVA required-site set")
    return info


def cbytes(data):
    return ", ".join(f"0x{x:02X}" for x in data)


def render(info):
    lines = [
        "// Generated by tools/media_patch_manifest.py; review the companion JSON.",
        "// Only VA/GVA patch windows are masked in page hashes; no stock-page recovery.",
        "// Image/cache UUIDs are checked by preflight. Runtime pins the kernel UUID,",
        "// checks the OS build once populated, and verifies",
        "// exact cache basename/file page and every other byte via SHA-256.",
        "#pragma once", "namespace MediaPatchManifest {",
    ]
    page_sites = []
    for index, page in enumerate(info["pages"]):
        for site_index, site in enumerate(page["sites"]):
            tag = f"Site{index}_{site_index}"
            lines += [
                f"    // {site['symbol']}",
                f"    inline constexpr UInt8 {tag}Find[] = {{{cbytes(bytes.fromhex(site['find']))}}};",
                f"    inline constexpr UInt8 {tag}Replace[] = {{{cbytes(bytes.fromhex(site['replace']))}}};",
            ]
        lines.append(f"    inline constexpr MediaPatchPlan::Site Page{index}Sites[] = {{")
        for site_index, site in enumerate(page["sites"]):
            tag = f"Site{index}_{site_index}"
            lines.append(f"        {{{hex(site['offset'])}, {tag}Find, {tag}Replace, sizeof({tag}Find)}},")
        lines.append("    };")
        page_sites.append(f"Page{index}Sites")
    lines.append("    inline constexpr MediaPatchPlan::Page Pages[] = {")
    for page, sites in zip(info["pages"], page_sites):
        digest = cbytes(bytes.fromhex(page["masked_sha256"]))
        lines.append(f'        {{"{page["file"]}", {page["offset"]}ULL, {{{digest}}}, {sites}, arrsize({sites})}},')
    lines += [
        "    };",
        f'    inline constexpr char Build[] = "{info["build"]}";',
        f'    inline constexpr UInt8 KernelUUID[] = {{{cbytes(uuid.UUID(info["kernel_uuid"]).bytes)}}};',
        f"    inline constexpr size_t RequiredSites = {REQUIRED_SITES};",
        "    static_assert(arrsize(Pages) > 0 && arrsize(Pages) <= 32);",
        "    inline constexpr UInt32 RequiredPageMask = ~UInt32{0} >> (32 - arrsize(Pages));",
        "}", "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    parser.add_argument("--build", help="required for a non-live cache")
    parser.add_argument("--kernel", type=Path, default=Path("/System/Library/Kernels/kernel"))
    parser.add_argument("--candidate", type=Path, help="write a review candidate JSON, never edit the committed files")
    parser.add_argument("--render", type=Path, help="render a reviewed JSON to stdout (no cache access)")
    parser.add_argument("--dump-pages", type=Path, help="save local test snapshots outside the repository")
    args = parser.parse_args()
    if args.render:
        print(render(json.loads(args.render.read_text())), end="")
        return 0
    build = args.build or subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip()
    info = collect(args.cache_dir, build, args.dump_pages, args.kernel)
    if args.cache_dir == DEFAULT_CACHE_DIR:
        running_uuid = subprocess.check_output(["sysctl", "-n", "kern.uuid"], text=True).strip().upper()
        if info["kernel_uuid"] != running_uuid:
            raise ValueError("on-disk kernel differs from the running kernel UUID")
    if args.candidate:
        args.candidate.write_text(json.dumps(info, indent=2) + "\n")
        print(f"Candidate: {len(info['pages'])} bounded pages, {REQUIRED_SITES} required sites. Review before use.")
        print("Fingerprints are observed bytes outside declared patch windows, not independent stock evidence.")
        return 0
    expected = json.loads(MANIFEST.read_text())
    if expected != info or HEADER.read_text() != render(expected):
        raise ValueError("OS/image identities, required sites, page fingerprints or generated header differ")
    print(f"PASS {build}: exact cache and both image UUIDs; {len(info['pages'])} page fingerprints, {REQUIRED_SITES} required sites")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        raise SystemExit(f"FAIL {error}")
