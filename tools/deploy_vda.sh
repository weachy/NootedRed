#!/bin/bash
# Install the current NootedRed release kext on an OpenCore EFI partition.
# Usage:
#   tools/deploy_vda.sh --esp /Volumes/EFI            # install (keeps a timestamped backup)
#   tools/deploy_vda.sh --esp /Volumes/EFI --kext /path/to/NootedRed.kext
#   tools/deploy_vda.sh --esp /Volumes/EFI --restore  # restore the newest timestamped backup
#   tools/deploy_vda.sh --list-backups --esp /Volumes/EFI
#
# This script never reboots and never edits the OpenCore config/boot-args. Media is default-on (see
# docs/VDA-Deployment.md). Run it from the repository root; it needs sudo for the ESP writes.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP_DIR="${ROOT}/build/efi-backup"
ESP=""
ACTION="install"
SRC=""

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --esp) ESP="${2:-}"; shift 2 ;;
        --kext)
            [[ $# -ge 2 && -n "$2" ]] || { echo "missing --kext path" >&2; exit 1; }
            SRC="$2"; shift 2 ;;
        --restore) ACTION="restore"; shift ;;
        --list-backups) ACTION="list"; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

if [[ -z "${ESP}" ]]; then
    echo "missing --esp /Volumes/EFI" >&2
    exit 1
fi
if [[ ! -d "${ESP}/EFI/OC/Kexts" ]]; then
    echo "not an OpenCore ESP: ${ESP}/EFI/OC/Kexts not found" >&2
    exit 1
fi

DST="${ESP}/EFI/OC/Kexts/NootedRed.kext"
mkdir -p "${BACKUP_DIR}"

# The ESP may be mounted with noowners and be writable by the current user; only use sudo when needed.
SUDO=""
if [[ ! -w "${ESP}/EFI/OC/Kexts" ]]; then
    SUDO="sudo"
fi

case "${ACTION}" in
    list)
        ls -1dt "${BACKUP_DIR}"/NootedRed-*.kext 2>/dev/null || echo "no backups"
        ;;
    restore)
        BACKUP="$(ls -1dt "${BACKUP_DIR}"/NootedRed-*.kext 2>/dev/null | head -n1 || true)"
        if [[ -z "${BACKUP}" ]]; then
            echo "no backup found in ${BACKUP_DIR}" >&2
            exit 1
        fi
        echo "restoring ${BACKUP} -> ${DST}"
        ${SUDO} rm -rf "${DST}"
        ${SUDO} ditto "${BACKUP}" "${DST}"
        echo "restored; reboot to apply"
        ;;
    install)
        if [[ -z "${SRC}" ]]; then
            SRC="$(sh "${ROOT}/tools/release_kext_path.sh")"
        fi
        if [[ ! -s "${SRC}/Contents/MacOS/NootedRed" ]]; then
            echo "missing selected build artifact: ${SRC}" >&2
            echo "build with the same NRED_BUILD_BRANCH, or pass --kext explicitly" >&2
            exit 1
        fi
        # Resolve symlinks before replacing the destination. An explicit
        # source inside that destination would otherwise be deleted first.
        SRC="$(cd "${SRC}" && pwd -P)"
        DST="$(cd "${ESP}/EFI/OC/Kexts" && pwd -P)/NootedRed.kext"
        if [[ "${SRC}" == "${DST}" || "${SRC}" == "${DST}/"* ]]; then
            echo "source kext must be outside the install destination: ${SRC}" >&2
            exit 1
        fi
        NEW_HASH="$(shasum -a 256 "${SRC}/Contents/MacOS/NootedRed" | awk '{print $1}')"
        echo "source kext: ${SRC}"
        echo "new kext SHA-256: ${NEW_HASH}"
        if [[ -d "${DST}" ]]; then
            OLD_HASH="$(shasum -a 256 "${DST}/Contents/MacOS/NootedRed" | awk '{print $1}')"
            STAMP="$(date +%Y%m%d-%H%M%S)-$$"
            BACKUP="${BACKUP_DIR}/NootedRed-${STAMP}.kext"
            if [[ -e "${BACKUP}" ]]; then
                echo "backup already exists; refusing to overwrite it: ${BACKUP}" >&2
                exit 1
            fi
            echo "backing up current kext (${OLD_HASH}) -> ${BACKUP}"
            ${SUDO} ditto "${DST}" "${BACKUP}"
        fi
        echo "installing -> ${DST}"
        ${SUDO} rm -rf "${DST}"
        ${SUDO} ditto "${SRC}" "${DST}"
        INSTALLED_HASH="$(shasum -a 256 "${DST}/Contents/MacOS/NootedRed" 2>/dev/null | awk '{print $1}')"
        if [[ "${INSTALLED_HASH}" != "${NEW_HASH}" ]]; then
            echo "installed kext hash ${INSTALLED_HASH} != ${NEW_HASH}" >&2
            if [[ -n "${BACKUP:-}" && -d "${BACKUP}" ]]; then
                echo "restoring ${BACKUP}" >&2
                ${SUDO} rm -rf "${DST}"
                ${SUDO} ditto "${BACKUP}" "${DST}"
            fi
            exit 1
        fi
        echo "verified installed SHA-256: ${INSTALLED_HASH}"
        echo "installed; reboot to apply. Media is enabled by default on the validated build/device."
        echo "To disable media, use -NRedNoVDA. Run tools/vda_hwtest.sh after boot."
        ;;
esac
