# Compatibility data collector

`collect_compat_data.py` gathers the machine data needed to extend NootedRed's two hardware fixes to
another macOS build or AMD APU:

- the Chromium/Electron GL/Metal dual-source fix — user-space driver images and candidate patch sites;
- the VCN/VDA hardware codec support — kernel ABI data, user-space VA/AppleGVA patch data and media
  capabilities.

It is read-only, does not install anything and does not need root. It replaces the older
`collect_platform_data.py` for current work (that script is kept for the historical datasets).

## Requirements

- Run on the target macOS install, or use `--sys-root` pointing to a mounted offline macOS disk volume.
- **Prefer a pristine boot** for data collected from the live system that will be used to generate new patch sets: boot with `-NRedOff`
  (or without NootedRed) so the kext has not patched the file-backed page cache. The collector detects
  the patched state and, for the already-committed pages, reconstructs the vanilla pages from
  `NootedRed/UserPatches.cpp`.
  > **Tip**: When collecting from an offline volume via `--sys-root` (e.g. an external SSD), files on disk are completely unaffected by live kext memory patches, naturally satisfying the pristine requirements.
- `python3`, `nm`, `ioreg`, `system_profiler`, `kmutil`/`ffmpeg` are used when present; missing tools only
  produce warnings.

## Usage

### 1. Live system collection (running on target OS)

The script automatically detects the running macOS version (e.g. `14.8.9`) and stores output into a directory named `nred-compat-<macos-version>`:

```sh
# Collect to /tmp/nred-compat-<macos-version>
python3 tools/collect_compat_data.py --out /tmp

# Collect and generate /tmp/nred-compat-<macos-version>.zip
python3 tools/collect_compat_data.py --out /tmp --zip

# Collect to current directory (./nred-compat-<macos-version>) with full KC and zip
python3 tools/collect_compat_data.py --include-kc --zip
```

### 2. External / offline disk collection (without rebooting)

If you have an external SSD or another partition with macOS installed (e.g. macOS 13.x, 14.x, 15.x, or 26.x mounted at `/Volumes/<VolumeName>`), you can collect its complete compatibility dataset directly without rebooting into it:

```sh
# Collect from mounted external disk volume
python3 tools/collect_compat_data.py --sys-root "/Volumes/Macintosh HD" --include-kc --zip

# Collect to a specific output path (e.g. into DEV/AnalysisData)
python3 tools/collect_compat_data.py --sys-root "/Volumes/Ventura" --out ../AnalysisData --include-kc --zip
```

When `--sys-root` is supplied:
- The OS version is automatically read from `<sys-root>/System/Library/CoreServices/SystemVersion.plist`, generating the proper directory name (e.g. `nred-compat-13.6.4`).
- `AMDRadeonX5000GLDriver`, `SystemKernelExtensions.kc`, firmware, and dyld shared cache are extracted directly from the specified volume.
- Current host GPU and display metadata are recorded alongside offline system information.

### Options

| Option | Meaning |
| --- | --- |
| `--sys-root DIR` | system root directory (default: `/`; pass e.g. `/Volumes/<VolumeName>` for an attached external disk) |
| `--out DIR` | destination parent directory (default: current directory, generating `nred-compat-<macos-version>`; supports `/path` and `/path/`) |
| `--zip` | also create `<out>.zip` archive alongside the output folder |
| `--cache-dir DIR` | override the dyld shared cache directory |
| `--include-kc` | copy `SystemKernelExtensions.kc` (hundreds of MB); needed to re-derive VDA data for a new macOS build |
| `--no-generators` | skip running the VCN/VA/GVA generators (faster, metadata/Chromium data only) |

## Output layout

```
manifest.json                 schema/tool version, OS/GPU identity, generator results, warnings, file hashes
SHA256SUMS
metadata/
  sw_vers.txt uname.txt boot-args.txt csrutil.txt nvram-filtered.txt
  hardware.json displays.json gpu-identity.json      (serial/UUID fields redacted)
  kexts-kmutil.txt kexts-kextstat.txt
  ioreg-gpu.txt ioreg-media.txt
  videotoolbox-ffmpeg.txt coremedia-defaults.txt
chromium/
  GLDriver.bin / GLDriver.stock.bin                  (stock copy exists when the live page cache was patched)
  GLDriver-nm.txt
  MTL-image.bin / MTL-image.stock.bin
  MTL-info.json MTL-symbols-all.txt
  sites.json                                          every hit of the known patch patterns, with page hashes
  pages/<sha>.bin                                     unique 4 KiB pages containing the hits
  spec-hints.json                                     how to turn the hits into a gen_patch_data.py spec
vda/
  kernel-info.json                                    KC + X5000/X6000 UUIDs, versions and segment hashes
  kernel-abi.json                                     guards / call-site tables (when derivable)
  generated/                                          VCNKernelData.hpp, VDAVAData.hpp, VDAGVAData.hpp
  generator-results.json                              exit codes, stdout/stderr; --check fallback verdicts
  firmware/                                           on-disk AMD firmware blobs, when available
```

`chromium/sites.json` is the important file for a new macOS build: it lists every hit of the validated
patterns (`c7477804000000`, `83e0fe`, `750b`, `0f94c0`) with the file/VA offset, page offset, 32-byte
context and a reference to the unique 4 KiB page image. Pick the correct sites in a disassembler, then
write a spec for `tools/gen_patch_data.py` (format in `tools/README-patchgen.md`).

## VDA data and cross-version compatibility

`vda/generated/` contains the three headers as generated on the target build, or the committed headers
when the generators can verify them against the system (`--check` fallback). `vda/generator-results.json`
records both outcomes. If generation fails on a new macOS major/minor build: keep the dataset, pass
`--include-kc`, and provide the archive so the missing signatures or struct offsets can be added.

`vda/kernel-info.json` is collected on all builds: it contains the kernel collection UUID and
the X5000/X6000 kext UUIDs, versions and segment hashes. NootedRed uses this identity evidence to
validate and drive dynamic patching across macOS 13 (Ventura), macOS 14 (Sonoma), macOS 15 (Sequoia),
and macOS 26 (Tahoe).

## Privacy and redistribution

The archive contains Apple binaries and hardware details, not system files beyond what is needed. The
hardware JSON has serial numbers and hardware UUIDs redacted, and only the relevant `nvram` keys are
kept. **Do not publish the archive**; share it privately with the maintainer.

## Adding a dataset to the repository

1. Collect the archive and keep it outside git (`.gitignore` covers `platform-data/` archives).
2. Record it under `platform-data/<date>_<os>_<cpu>_<device-id>/` if it should be indexed.
3. Add an entry to `docs/Platform-Data-Index.md` with the OS build, Darwin version, APU/device ID and
   archive hash.
4. From the collected files, add the data to `NootedRed/UserPatches.cpp` / `DeviceProfile` using the
   generators, and record the hardware validation status honestly.
