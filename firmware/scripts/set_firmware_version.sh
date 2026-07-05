#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <semver>   e.g. 1.0.1" >&2
    exit 1
fi

VERSION="$1"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid semver: $VERSION (expected MAJOR.MINOR.PATCH)" >&2
    exit 1
fi

IFS='.' read -r MAJOR MINOR PATCH <<< "$VERSION"
FILE_VERSION=$(printf "0x%08X" $(( (MAJOR << 16) | (MINOR << 8) | PATCH )))
LEN=${#VERSION}
HEX_LEN=$(printf '\\x%02x' "$LEN")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HEADER="${SCRIPT_DIR}/../src/ota_zigbee.h"

sed -i "s/^#define OTA_FILE_VERSION.*/#define OTA_FILE_VERSION         ${FILE_VERSION}/" "$HEADER"
sed -i "s/^#define FW_VERSION_STRING.*/#define FW_VERSION_STRING        \"${HEX_LEN}\"\"${VERSION}\"/" "$HEADER"

echo "Set firmware version ${VERSION} (${FILE_VERSION}) in ${HEADER}"
