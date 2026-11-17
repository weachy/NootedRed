#!/bin/bash
# Bounded hardware-required AVC/HEVC test. VDA is enabled by default.
# Use a Python environment containing PyAV and NumPy:
#   VDA_TEST_PYTHON=/path/to/venv/bin/python tools/vda_hwtest.sh
# Exit 77 means skipped/incomplete, never a hardware pass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "${VDA_TEST_PYTHON:-python3}" "${ROOT}/tools/vda_validate.py" --require-manifest "$@"
