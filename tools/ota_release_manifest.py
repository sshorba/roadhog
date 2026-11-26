#!/usr/bin/env python3
"""Generate the canonical manifest from an ESP32-S3 Road Hög app binary."""

import argparse
import hashlib
import re
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("sequence", type=int)
    parser.add_argument("notes_url")
    args = parser.parse_args()
    sequence = args.sequence
    if not 1 <= sequence <= 0xFFFFFFFF:
        parser.error("sequence must fit a nonzero uint32")
    if args.image.name != f"roadhog-v{sequence}.bin":
        parser.error(f"image must be named roadhog-v{sequence}.bin")
    if not re.fullmatch(r"https://[a-z0-9.-]+/[!-~]*", args.notes_url) or any(
        ch in args.notes_url for ch in "#\\"
    ):
        parser.error("expected an absolute HTTPS notes URL")
    if not 288 <= args.image.stat().st_size <= 0x400000:
        parser.error("image must fit in one 4 MiB OTA slot")
    image = args.image.read_bytes()
    descriptor = image[32:288]
    if (
        image[0] != 0xE9
        or image[3] >> 4 != 4
        or struct.unpack_from("<H", image, 12)[0] != 9
        or struct.unpack_from("<I", descriptor)[0] != 0xABCD5432
        or descriptor[16:48].split(b"\0", 1)[0] != str(sequence).encode("ascii")
        or descriptor[48:80].split(b"\0", 1)[0] != b"roadhog"
    ):
        parser.error("image header, project or embedded version does not match the release")
    manifest = (
        "roadhog-release-v1\n"
        "target=esp32s3-n16r8-ota-ab-v1\n"
        f"sequence={sequence}\n"
        f"size={len(image)}\n"
        f"sha256={hashlib.sha256(image).hexdigest()}\n"
        f"notes={args.notes_url}\n"
    ).encode("ascii")
    if len(manifest) > 1024:
        parser.error("manifest exceeds device size limit")
    with args.image.with_suffix(".manifest").open("xb") as output:
        output.write(manifest)


if __name__ == "__main__":
    main()
