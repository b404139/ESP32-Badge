#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_IMAGES = {
    "0x0": "bootloader",
    "0x8000": "partition-table",
    "0x10000": "app",
    "0x210000": "storage",
}
FLASH_SIZES = {
    "1MB": 0x100000,
    "2MB": 0x200000,
    "4MB": 0x400000,
    "8MB": 0x800000,
    "16MB": 0x1000000,
    "32MB": 0x2000000,
    "64MB": 0x4000000,
    "128MB": 0x8000000,
}


def parse_int(value):
    value = value.strip().upper()
    multipliers = {"K": 1024, "M": 1024 * 1024}
    if value and value[-1] in multipliers:
        return int(value[:-1], 0) * multipliers[value[-1]]
    return int(value, 0)


def partition_sizes(path):
    sizes = {}
    with path.open(newline="", encoding="utf-8-sig") as stream:
        for row in csv.reader(stream):
            if not row or row[0].lstrip().startswith("#"):
                continue
            sizes[row[0].strip()] = parse_int(row[4].strip())
    return sizes


def make_release(build_dir, output):
    args = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    flash_files = args["flash_files"]
    if set(flash_files) != set(REQUIRED_IMAGES):
        raise ValueError(f"unexpected flash layout: {sorted(flash_files)}")

    flash_size = FLASH_SIZES[args["write_flash_args"][args["write_flash_args"].index("--flash_size") + 1]]
    if flash_size != 0x2000000:
        raise ValueError(f"this badge requires a 32MB image, got {flash_size:#x} bytes")

    sizes = partition_sizes(ROOT / "partitions.csv")
    images = []
    for offset_text, relative in sorted(flash_files.items(), key=lambda item: parse_int(item[0])):
        offset = parse_int(offset_text)
        path = build_dir / relative
        size = path.stat().st_size
        name = REQUIRED_IMAGES[offset_text]
        if name in sizes and size > sizes[name]:
            raise ValueError(f"{name} is {size} bytes, exceeds partition size {sizes[name]}")
        images.append((offset, path, size))

    for (offset, path, size), (next_offset, _, _) in zip(images, images[1:]):
        if offset + size > next_offset:
            raise ValueError(f"{path.name} overlaps the next image at {next_offset:#x}")
    if images[-1][0] + images[-1][2] > flash_size:
        raise ValueError("flash images exceed the configured flash size")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    digest = hashlib.sha256()
    try:
        with temporary.open("wb") as stream:
            position = 0
            for offset, path, size in images:
                padding = offset - position
                if padding < 0:
                    raise ValueError(f"invalid image offset {offset:#x}")
                block = b"\xff" * min(padding, 1024 * 1024)
                while padding:
                    chunk = block[:padding]
                    stream.write(chunk)
                    digest.update(chunk)
                    padding -= len(chunk)
                    position += len(chunk)
                with path.open("rb") as source:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        stream.write(chunk)
                        digest.update(chunk)
                        position += len(chunk)
            padding = flash_size - position
            while padding:
                chunk = block[:padding]
                stream.write(chunk)
                digest.update(chunk)
                padding -= len(chunk)
                position += len(chunk)
        if temporary.stat().st_size != flash_size:
            raise ValueError("merged image size mismatch")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)

    checksum = digest.hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(
        f"{checksum}  {output.name}\n", encoding="ascii"
    )
    for offset, path, size in images:
        print(f"{offset:#08x}  {size:>10,}  {path.relative_to(build_dir)}")
    print(f"output: {output}")
    print(f"size: {output.stat().st_size:,} bytes")
    print(f"sha256: {checksum}")


def main():
    parser = argparse.ArgumentParser(description="Merge ESP-IDF flash images into one 0x0 image.")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    make_release(args.build_dir.resolve(), args.output.resolve())


if __name__ == "__main__":
    main()
