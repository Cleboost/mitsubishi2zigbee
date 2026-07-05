#!/usr/bin/env python3
"""Add or refresh a release entry in the Zigbee2MQTT OTA index."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

MANUFACTURER_CODE = 0x1337
IMAGE_TYPE = 0x0001
MODEL_ID = "MZ-Zigbee-C6"


def semver_to_file_version(version: str) -> int:
    major, minor, patch = map(int, version.split("."))
    return (major << 16) | (minor << 8) | patch


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="Semver without v prefix, e.g. 1.0.1")
    parser.add_argument("url", help="HTTP(S) URL to the .ota asset")
    parser.add_argument(
        "index",
        nargs="?",
        default="zigbee2mqtt_converter/ota_index.json",
        help="Path to ota_index.json",
    )
    args = parser.parse_args()

    index_path = Path(args.index)
    file_version = semver_to_file_version(args.version)

    entry = {
        "fileName": f"{MODEL_ID}_{args.version}.ota",
        "fileVersion": file_version,
        "url": args.url,
        "imageType": IMAGE_TYPE,
        "manufacturerCode": MANUFACTURER_CODE,
        "modelId": MODEL_ID,
    }

    index: list[dict] = []
    if index_path.exists():
        index = json.loads(index_path.read_text(encoding="utf-8"))

    index = [e for e in index if e.get("fileVersion") != file_version]
    index.append(entry)
    index.sort(key=lambda e: e["fileVersion"], reverse=True)

    index_path.parent.mkdir(parents=True, exist_ok=True)
    index_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    print(f"Updated {index_path} with {entry['fileName']} (fileVersion={file_version})")


if __name__ == "__main__":
    main()
