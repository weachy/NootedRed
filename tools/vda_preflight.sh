#!/bin/bash
# Read-only build/device/cache preflight. No VideoToolbox calls or EFI writes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEXT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kext)
            [[ $# -ge 2 && -n "$2" ]] || { echo "missing --kext path" >&2; exit 1; }
            KEXT="$2"; shift 2 ;;
        *) echo "usage: tools/vda_preflight.sh [--kext /path/to/NootedRed.kext]" >&2; exit 1 ;;
    esac
done
if [[ -z "${KEXT}" ]]; then
    KEXT="$(sh "${ROOT}/tools/release_kext_path.sh")"
fi
echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), Darwin $(uname -r)"
echo "CPU: $(sysctl -n machdep.cpu.brand_string)"
python3 "${ROOT}/tools/media_patch_manifest.py"

DISPLAY_INFO="$(system_profiler SPDisplaysDataType)"
DEVICE_ID="$(echo "${DISPLAY_INFO}" | sed -n 's/.*Device ID: \(0x[0-9A-Fa-f]*\).*/\1/p' | head -n1 | tr '[:upper:]' '[:lower:]')"
case "${DEVICE_ID}" in
    0x1638|0x15e7|0x1636|0x164c) echo "PASS VCN 2.x target ${DEVICE_ID}" ;;
    *) echo "FAIL unsupported/unknown iGPU ${DEVICE_ID:-unknown}"; exit 1 ;;
esac
MARKERS="$(ioreg -l -w0 -r -n IGPU)"
LOADED_KEXTS="$(kextstat 2>/dev/null)"
if grep -q 'com.apple.kext.AMDRadeonX6000 ' <<<"${LOADED_KEXTS}"; then
    if grep -qE '"NRedVCNNativeLoadReady"[[:space:]]*=[[:space:]]*1' <<<"${MARKERS}"; then
        echo "PASS X6000 loaded by the current bridge"
    else
        echo "FAIL X6000 already loaded without a ready bridge; investigate the dGPU/driver owner"; exit 1
    fi
fi
if [[ ! -s "${KEXT}/Contents/MacOS/NootedRed" ]]; then
    echo "FAIL missing selected release kext: ${KEXT}"; exit 1
fi
echo "source kext: ${KEXT}"
plutil -lint "${KEXT}/Contents/Info.plist"
shasum -a 256 "${KEXT}/Contents/MacOS/NootedRed"
echo "boot-args: $(sysctl -n kern.bootargs)"
echo "PASS preflight; runtime validation still requires booting this artifact and tools/vda_hwtest.sh"
