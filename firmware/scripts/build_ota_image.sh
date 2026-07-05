#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_BIN="${FIRMWARE_DIR}/.pio/build/esp32-c6-zero/firmware.bin"
TOOL="${SCRIPT_DIR}/image_builder_tool.py"

MANUF_ID="0x1337"
IMAGE_TYPE="0x0001"
FILE_VERSION="${1:-0x00010001}"
OUTPUT="${2:-${FIRMWARE_DIR}/.pio/build/esp32-c6-zero/firmware-${FILE_VERSION}.ota}"

if [[ ! -f "$BUILD_BIN" ]]; then
    echo "Build firmware first: cd firmware && pio run" >&2
    exit 1
fi

if [[ ! -f "$TOOL" ]]; then
    curl -fsSL \
        "https://raw.githubusercontent.com/espressif/esp-zigbee-sdk/main/tools/image_builder_tool/image_builder_tool.py" \
        -o "$TOOL"
fi

WORK_DIR="$(dirname "$OUTPUT")"
mkdir -p "$WORK_DIR"
GENERATED_NAME="$(printf '%04X-%04X-%08X-ota-file.zigbee' \
    "$((MANUF_ID))" "$((IMAGE_TYPE))" "$((FILE_VERSION))")"
GENERATED_PATH="${WORK_DIR}/${GENERATED_NAME}"

(
    cd "$WORK_DIR"
    python3 "$TOOL" \
        --manuf-id "$MANUF_ID" \
        --image-type "$IMAGE_TYPE" \
        --file-version "$FILE_VERSION" \
        --tag 0x0000 "${BUILD_BIN}"
)

mv -f "$GENERATED_PATH" "$OUTPUT"

echo "OTA image: $OUTPUT"
ls -lh "$OUTPUT"
