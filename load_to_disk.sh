#!/usr/bin/env bash
# ==============================================================================
# load_to_disk.sh - Helper script to load applications onto an IPO_OS disk image
#
# Usage:
#   ./load_to_disk.sh <path/to/disk.img> [optional extra flags]
#
# Examples:
#   ./load_to_disk.sh /path/to/disk.img
#   ./load_to_disk.sh /tmp/my_disk.img --format
#   ./load_to_disk.sh --disk /path/to/disk.img
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -eq 0 ] && [ -z "${DISK:-}" ] && [ -z "${IPO_DISK:-}" ]; then
    echo "================================================================="
    echo " ОШИБКА: Путь к образу диска не указан!"
    echo " Укажите путь к целевому диску явно:"
    echo "   ./load_to_disk.sh /path/to/disk.img"
    echo "   ./load_to_disk.sh /path/to/disk.img --format"
    echo "   ./load_to_disk.sh --disk /path/to/disk.img"
    echo "================================================================="
    exit 1
fi

if [ $# -ge 1 ] && [[ "$1" != -* ]]; then
    DISK_TARGET="$1"
    shift
    python3 "${SCRIPT_DIR}/tools/load_applications.py" --disk "${DISK_TARGET}" "$@"
else
    python3 "${SCRIPT_DIR}/tools/load_applications.py" "$@"
fi
