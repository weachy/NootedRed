#!/bin/bash
# VDA hardware-decode test: run after booting with -NRedVDA.
# Collects the kernel/user-space markers, then decodes small H.264/HEVC clips with VideoToolbox and compares
# the output with software decoding. Does not install kexts or reboot. A GPU hang is possible on a failing
# engine; keep the rollback runbook ready.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d /tmp/vda-hwtest.XXXXXX)" || exit 1
FAIL=0
WARN=0

pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAIL=$((FAIL + 1)); }
warn() { echo "WARN  $*"; WARN=$((WARN + 1)); }

run_timeout() {
    # macOS has no timeout(1); perl's alarm is enough for a bounded ffmpeg run.
    perl -e 'alarm shift; exec @ARGV' "$@"
}

raven2_smoke() {
    local clip="$1" work="$2" status=0
    mkdir -m 700 "$work" || return 1
    echo "Raven2 smoke-test artifacts: $work"

    ioreg -l -w0 -r -n IGPU >"$work/before.ioreg" || return 1
    if ! grep -Eq '"NRed(Raven2|Picasso)DecodeRequested" = 1' "$work/before.ioreg" ||
       ! grep -q '"NRedVCNEngineAllocated" = 1' "$work/before.ioreg"; then
        echo 'Raven2 experiment is not active or its engine was not allocated.' >&2
        return 1
    fi
    if ! run_timeout 15 ffmpeg -nostdin -hide_banner -loglevel error -i "$clip" \
        -frames:v 1 -vf format=nv12 -f rawvideo "$work/software.nv12" >"$work/software.log" 2>&1 ||
        [[ ! -s "$work/software.nv12" ]]; then
        echo 'Software reference frame failed. See software.log.' >&2
        return 1
    fi

    echo 'Requesting one hardware-decoded output frame from VideoToolbox.'
    sync
    run_timeout 15 ffmpeg -nostdin -hide_banner -loglevel verbose -hwaccel videotoolbox \
        -hwaccel_output_format videotoolbox_vld -i "$clip" -frames:v 1 \
        -vf hwdownload,format=nv12 -f rawvideo "$work/hardware.nv12" >"$work/hardware.log" 2>&1 || status=$?
    ioreg -l -w0 -r -n IGPU >"$work/after.ioreg"
    run_timeout 15 log show --last boot --style compact \
        --predicate 'eventMessage CONTAINS "Raven2 first decode IB commit" OR eventMessage CONTAINS "VCN2 initHardware"' \
        >"$work/bridge.log" 2>"$work/bridge-log-error.txt" || true
    local before after
    before="$(vcn_completions "$work/before.ioreg")"
    after="$(vcn_completions "$work/after.ioreg")"
    echo "VCN0Dec completion high-water mark: ${before:-unavailable} -> ${after:-unavailable}"
    if [[ $status -ne 0 || ! -s "$work/hardware.nv12" ]]; then
        echo "Hardware decode failed (exit $status). See hardware.log." >&2
        return 1
    fi
    if ! cmp -s "$work/software.nv12" "$work/hardware.nv12"; then
        echo 'Hardware frame differs from the software reference.' >&2
        return 1
    fi
    if [[ -z "$before" || -z "$after" || "$after" -le "$before" ]]; then
        echo 'The frames match, but VCN completion counter advancement was not observed.' >&2
        return 1
    fi
    echo 'Hardware frame matches the software reference and the VCN0Dec completion counter advanced.'
}

vcn_completions() {
    grep -oE 'HWChannel VCN0Dec \| Commands Completed"=[0-9]+' "$1" |
        cut -d= -f2 | sort -n | tail -n 1
}

if [[ $# -ne 0 ]]; then
    if [[ $# -ne 3 || $1 != --raven2-smoke ]]; then
        echo "Usage: $0 [--raven2-smoke <8-bit-h264-or-hevc-file> <new-output-directory>]" >&2
        exit 2
    fi
    raven2_smoke "$2" "$3"
    exit $?
fi

echo "== VDA hardware test =="
echo "work dir: ${WORK}"
BOOTARGS="$(sysctl -n kern.bootargs 2>/dev/null || true)"
echo "boot-args: ${BOOTARGS}"

if ! echo "${BOOTARGS}" | grep -q -- '-NRedVDA'; then
    fail "-NRedVDA is not in boot-args; boot with it before running the decode test"
    echo "Diagnostics were still collected in ${WORK}; rerun after adding -NRedVDA."
fi

echo
echo "== kernel/user-space markers =="
ioreg -l -w0 -r -n IGPU 2>/dev/null | grep -oE '"NRed[A-Za-z0-9]+"[[:space:]]*=[[:space:]]*[0-9]+' | sort -u | tee "${WORK}/markers.txt" || true
cat "${WORK}/markers.txt" 2>/dev/null || true
BRIDGE_READY=0
if [[ -s "${WORK}/markers.txt" ]]; then
    pass "NRedVDA/NRedVCN ioreg markers present"
else
    fail "no NRedVDA/NRedVCN ioreg markers (bridge did not run)"
fi
BRIDGE_READY=1
for key in NRedVCNX5000ABIValidated NRedVCNX6000ABIValidated NRedVCNNativeLoadReady NRedVCNEngineAllocated; do
    if grep -qE "\"${key}\"[[:space:]]*=[[:space:]]*1" "${WORK}/markers.txt"; then
        pass "${key}=1"
    else
        fail "${key} is not 1"
        BRIDGE_READY=0
    fi
done
if grep -qE '"NRedVCNUserApplied"[[:space:]]*=[[:space:]]*[0-9]+' "${WORK}/markers.txt"; then
    pass "VDA user-space pages were patched (NRedVCNUserApplied)"
else
    warn "NRedVCNUserApplied is absent (VA/AppleGVA pages may not have been validated yet)"
fi

echo
echo "== logs =="
log show --last boot --style compact --info --debug \
    --predicate 'eventMessage CONTAINS "VCN" OR eventMessage CONTAINS "VDA" OR eventMessage CONTAINS "UserPatch"' \
    2>/dev/null >"${WORK}/vda.log" || true
grep -E "ABI|native load|initHardware|engine|patched|failed|mismatch" "${WORK}/vda.log" | tail -n 40 || true
if grep -qiE "panic|gpuRestart|watchdog" "${WORK}/vda.log"; then
    warn "VDA log contains panic/gpuRestart/watchdog text (inspect ${WORK}/vda.log)"
else
    pass "no panic/gpuRestart/watchdog text in the VDA log"
fi

if [[ ${BRIDGE_READY} -ne 1 ]]; then
    warn "kernel VCN bridge is not ready; skipping VideoToolbox decode comparison because a software fallback would produce a misleading pass"
elif ! command -v ffmpeg >/dev/null 2>&1; then
    warn "ffmpeg is not installed; decode tests were skipped"
else
    echo
    echo "== VideoToolbox availability =="
    ffmpeg -hide_banner -hwaccels 2>/dev/null | tee "${WORK}/hwaccels.txt"
    if grep -qi videotoolbox "${WORK}/hwaccels.txt"; then
        pass "ffmpeg advertises VideoToolbox"
    else
        fail "ffmpeg does not advertise VideoToolbox"
    fi

    make_clip() {
        local out="$1"; shift
        run_timeout 120 ffmpeg -hide_banner -loglevel error -y "$@" "${out}" >/dev/null 2>&1
    }
    decode_case() {
        local name="$1" clip="$2"
        local soft="${WORK}/${name}-soft.yuv" hard="${WORK}/${name}-hw.yuv" log="${WORK}/${name}-hw.log"
        echo
        echo "-- ${name} --"
        if ! run_timeout 120 ffmpeg -hide_banner -loglevel error -y -i "${clip}" -f rawvideo -pix_fmt yuv420p \
            "${soft}" >/dev/null 2>&1; then
            fail "${name}: software decode failed"
            return
        fi
        if run_timeout 180 ffmpeg -hide_banner -loglevel verbose -y -hwaccel videotoolbox -i "${clip}" \
            -f rawvideo -pix_fmt yuv420p "${hard}" >"${log}" 2>&1; then
            pass "${name}: hardware decode command completed"
        else
            fail "${name}: VideoToolbox decode failed (see ${log})"
            return
        fi
        if grep -qiE "videotoolbox|VideoToolbox" "${log}"; then
            pass "${name}: log mentions VideoToolbox"
        else
            warn "${name}: no VideoToolbox mention in the decode log"
        fi
        if grep -q "videotoolbox_vld" "${log}"; then
            pass "${name}: decoder negotiated the VideoToolbox hardware pixel format"
        else
            warn "${name}: no videotoolbox_vld pixel format in the decode log"
        fi
        if cmp -s "${soft}" "${hard}"; then
            pass "${name}: hardware output matches software decoding"
        else
            warn "${name}: hardware output differs from software decoding (check YUV/format differences)"
        fi
    }

    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264; then
        make_clip "${WORK}/h264-720p.mp4" -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 \
            -c:v libx264 -pix_fmt yuv420p
        make_clip "${WORK}/h264-64.mp4" -f lavfi -i testsrc2=size=64x64:rate=30 -t 2 \
            -c:v libx264 -pix_fmt yuv420p
        decode_case "h264-720p" "${WORK}/h264-720p.mp4"
        decode_case "h264-64x64-known-adverse" "${WORK}/h264-64.mp4"
    else
        warn "ffmpeg has no libx264 encoder; H.264 test skipped"
    fi
    if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
        make_clip "${WORK}/hevc-720p.mp4" -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 \
            -c:v libx265 -pix_fmt yuv420p
        decode_case "hevc-720p" "${WORK}/hevc-720p.mp4"
    else
        warn "ffmpeg has no libx265 encoder; HEVC test skipped"
    fi
fi

echo
echo "== post-decode VDA markers =="
ioreg -l -w0 -r -n IGPU 2>/dev/null | grep -oE '"NRed[A-Za-z0-9]+"[[:space:]]*=[[:space:]]*[0-9]+' | sort -u >"${WORK}/markers-after.txt" || true
cat "${WORK}/markers-after.txt" 2>/dev/null || true
if grep -qE '"NRedVCNLastHardwareInit"[[:space:]]*=[[:space:]]*1' "${WORK}/markers-after.txt"; then
    pass "VCN initHardware succeeded at least once after decoding"
else
    fail "NRedVCNLastHardwareInit is not 1 after decoding (hardware engine was not initialised)"
fi
if grep -qE '"NRedVCNUserApplied"[[:space:]]*=[[:space:]]*[1-9]' "${WORK}/markers-after.txt"; then
    pass "VDA user-space pages were patched after decoding"
else
    warn "NRedVCNUserApplied did not increase after decoding"
fi

if command -v ffmpeg >/dev/null 2>&1; then

    echo
    echo "== hardware encode (VideoProc acceptance) =="
    for encoder in h264_videotoolbox hevc_videotoolbox; do
        out="${WORK}/encode-${encoder}.mp4"
        if run_timeout 120 ffmpeg -hide_banner -loglevel error -y -f lavfi \
            -i testsrc2=size=1280x720:rate=30 -t 1 -c:v "${encoder}" -b:v 4M "${out}" >"${WORK}/${encoder}.log" 2>&1 \
            && [[ -s "${out}" ]] && run_timeout 120 ffmpeg -hide_banner -loglevel error -i "${out}" -f null - \
            >/dev/null 2>&1; then
            pass "${encoder}: hardware encode produced a decodable file"
        else
            fail "${encoder}: hardware encode failed (see ${WORK}/${encoder}.log)"
        fi
    done
    log show --last 5m --style compact --info --debug \
        --predicate 'process == "VTDecoderXPCService"' 2>/dev/null \
        | grep -iE 'AppleGVA|useHighPerformanceDecode|physical accelerator' | tail -n 12 \
        >"${WORK}/vtdecoder.log" || true
    if grep -qiE 'AppleGVA|useHighPerformanceDecode|physical accelerator' "${WORK}/vtdecoder.log"; then
        pass "VTDecoderXPCService log shows the AppleGVA hardware decoder path"
    else
        warn "no AppleGVA hardware-decoder evidence in the VTDecoderXPCService log"
    fi
fi

echo
echo "== GPU reset / watchdog reports (current boot) =="
log show --last boot --style compact \
    --predicate 'eventMessage CONTAINS[c] "gpuRestart" AND process != "log"' 2>/dev/null \
    | tail -n 20 | tee "${WORK}/gpu-resets.log" || true
BOOT_SEC="$(sysctl -n kern.boottime 2>/dev/null | sed -nE 's/^\{ sec = ([0-9]+),.*/\1/p')"
NEW_REPORTS=0
for report in /Library/Logs/DiagnosticReports/*gpuRestart*; do
    [[ -e "${report}" ]] || continue
    mtime="$(stat -f %m "${report}" 2>/dev/null || echo 0)"
    if [[ -n "${BOOT_SEC}" && "${mtime}" -ge "${BOOT_SEC}" ]]; then
        echo "${report}" >>"${WORK}/gpu-resets.log"
        NEW_REPORTS=1
    fi
done
if grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}' "${WORK}/gpu-resets.log" 2>/dev/null || [[ ${NEW_REPORTS} -eq 1 ]]; then
    warn "GPU reset/watchdog reports exist for this boot; review ${WORK}/gpu-resets.log"
else
    pass "no GPU reset/watchdog reports for this boot"
fi

echo
echo "artifacts: ${WORK}"
if [[ ${FAIL} -eq 0 ]]; then
    echo "HW TEST OK (${WARN} warning(s))"
    exit 0
fi
echo "HW TEST FAILED (${FAIL} failure(s), ${WARN} warning(s))"
exit 1
