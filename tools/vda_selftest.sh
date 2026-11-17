#!/bin/bash
# VDA hardware decode self-test (read-only).
# Media is enabled by default; -NRedNoVDA disables it. Diagnostics only, not a hardware PASS.
set -u

echo "== system =="
sw_vers
uname -r
echo "boot-args: $(sysctl -n kern.bootargs 2>/dev/null || true)"

echo
echo "== iGPU VDA/VCN properties =="
ioreg -l -w0 -r -n IGPU 2>/dev/null | grep -oE '"NRed[A-Za-z0-9]+"[[:space:]]*=[[:space:]]*[0-9]+' | sort -u || true

echo
echo "== NootedRed / VCN / UserPatch logs (current boot) =="
log show --last boot --style compact --info --debug \
  --predicate 'eventMessage CONTAINS "VCN" OR eventMessage CONTAINS "VDA" \
               OR eventMessage CONTAINS "UserPatch"' 2>/dev/null \
  | tail -n 120 || true

echo
echo "== loaded NootedRed =="
kextstat 2>/dev/null | grep -i nootedred || true

echo
echo "== GPU reset / watchdog reports (current boot) =="
log show --last boot --style compact \
  --predicate 'eventMessage CONTAINS[c] "gpuRestart" OR eventMessage CONTAINS[c] "watchdog"' 2>/dev/null \
  | tail -n 40 || true

echo
echo "== VideoToolbox hardware decode support =="
if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -hide_banner -hwaccels 2>/dev/null | sed -n '1,20p'
  echo
  echo "Availability alone does not prove hardware selection. Run tools/vda_hwtest.sh for strict validation."
else
  echo "ffmpeg not installed; use a VideoToolbox client or install ffmpeg for a quick check"
fi

echo
echo "== VTDecoderXPCService =="
pgrep -lf VTDecoderXPCService || echo "no decoder process running (start a decode to create one)"
