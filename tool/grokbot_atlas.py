#!/usr/bin/env python3
import hashlib
import struct
import urllib.request
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

COMMIT = "9d917f8dcbe48396c74145341a55c8f7b8492081"
SOURCE_URL = (
    "https://raw.githubusercontent.com/Ocean-lhy/Stopwatch-GrokBot/"
    f"{COMMIT}/main/grokbot.atlas"
)
SOURCE_SHA256 = "df3c750980927732a0b55f6d2d4481f36cc37c10e3d7819f402511333a347996"
OUTPUT = Path(__file__).resolve().parents[1] / "media" / "pet" / "grokbot.atlas"
SOURCE_SIZE = 400
CANVAS_SIZE = 440
GLOBAL_HEADER = struct.Struct("<4sHHHBBI")
CLIP_HEADER = struct.Struct("<16sHHIII")
FRAME_INDEX = struct.Struct("<II")
FRAME_HEADER = struct.Struct("<IB3x")


def decode_frames(data, clip):
    previous = None
    for frame_index in range(clip["frames"]):
        index_pos = clip["offset"] + frame_index * FRAME_INDEX.size
        record_offset, size_flags = FRAME_INDEX.unpack_from(data, index_pos)
        compressed_size = size_flags & 0x7FFFFFFF
        flags = size_flags >> 31
        record_pos = clip["offset"] + record_offset
        if record_pos > clip["offset"] + clip["size"] or FRAME_HEADER.size > clip["offset"] + clip["size"] - record_pos:
            raise ValueError(f"frame record outside clip: {clip['name']} frame {frame_index}")
        stored_size, stored_flags = FRAME_HEADER.unpack_from(data, record_pos)
        if stored_size != compressed_size or stored_flags != flags:
            raise ValueError(f"frame header mismatch: {clip['name']} frame {frame_index}")
        payload_pos = record_pos + FRAME_HEADER.size
        if compressed_size > clip["offset"] + clip["size"] - payload_pos:
            raise ValueError(f"frame payload outside clip: {clip['name']} frame {frame_index}")
        raw = zlib.decompress(data[payload_pos:payload_pos + compressed_size])
        if len(raw) != clip["packed_size"]:
            raise ValueError(f"inflated size mismatch: {clip['name']} frame {frame_index}")
        if flags:
            if previous is None:
                raise ValueError(f"first frame is delta: {clip['name']}")
            packed = bytes(a ^ b for a, b in zip(previous, raw))
        else:
            packed = raw
        yield packed
        previous = packed


def parse_atlas(data, expected_size=SOURCE_SIZE, expected_packed_size=SOURCE_SIZE * SOURCE_SIZE // 2):
    if len(data) < GLOBAL_HEADER.size:
        raise ValueError("atlas header truncated")
    magic, version, width, height, fps, clip_count, reserved = GLOBAL_HEADER.unpack_from(data)
    if (magic, version, width, height, fps) != (b"GBZ1", 1, expected_size, expected_size, 30):
        raise ValueError(f"unexpected atlas metadata: {magic} {version} {width}x{height} {fps}fps")
    if reserved != 0 or clip_count != 53:
        raise ValueError(f"unexpected atlas clip count or reserved field: {clip_count}, {reserved}")
    table_end = GLOBAL_HEADER.size + clip_count * CLIP_HEADER.size
    if table_end > len(data):
        raise ValueError("clip table truncated")

    clips = []
    for i in range(clip_count):
        fields = CLIP_HEADER.unpack_from(data, GLOBAL_HEADER.size + i * CLIP_HEADER.size)
        name, frames, start_frame, offset, size, packed_size = fields
        name = name.split(b"\0", 1)[0].decode("ascii")
        if not name or frames <= start_frame or packed_size != expected_packed_size:
            raise ValueError(f"invalid clip metadata: {name}")
        if offset < table_end or offset > len(data) or size > len(data) - offset:
            raise ValueError(f"clip outside atlas: {name}")
        if frames * FRAME_INDEX.size > size:
            raise ValueError(f"frame index exceeds clip: {name}")
        clips.append({
            "name": name,
            "frames": frames,
            "start": start_frame,
            "offset": offset,
            "size": size,
            "packed_size": packed_size,
        })

    seen = set()
    for clip in clips:
        digest = hashlib.sha256()
        frame_count = 0
        for packed in decode_frames(data, clip):
            digest.update(packed)
            frame_count += 1
        if frame_count != clip["frames"]:
            raise ValueError(f"frame count mismatch: {clip['name']}")
        if clip["name"] in seen:
            raise ValueError(f"duplicate clip: {clip['name']}")
        seen.add(clip["name"])
        clip["pixels_sha256"] = digest.hexdigest()
    return clips


def resize_packed(packed):
    source = np.frombuffer(packed, dtype=np.uint8).reshape(SOURCE_SIZE, SOURCE_SIZE // 2)
    gray = np.empty((SOURCE_SIZE, SOURCE_SIZE), dtype=np.uint8)
    gray[:, 0::2] = (source >> 4) * 17
    gray[:, 1::2] = (source & 0x0f) * 17
    image = Image.fromarray(gray, mode="L").resize((CANVAS_SIZE, CANVAS_SIZE), Image.Resampling.BILINEAR)
    quantized = np.clip((np.asarray(image, dtype=np.uint16) + 8) // 17, 0, 15).astype(np.uint8)
    return (quantized[:, 0::2] << 4) | quantized[:, 1::2]


def build_clip(source, source_clip):
    index = bytearray(source_clip["frames"] * FRAME_INDEX.size)
    records = bytearray()
    previous = None
    digest = hashlib.sha256()
    for frame_index, source_packed in enumerate(decode_frames(source, source_clip)):
        packed = resize_packed(source_packed).tobytes()
        digest.update(packed)
        delta = previous is not None
        payload = bytes(a ^ b for a, b in zip(previous, packed)) if delta else packed
        compressed = zlib.compress(payload, 9)
        record_offset = len(index) + len(records)
        FRAME_INDEX.pack_into(index, frame_index * FRAME_INDEX.size, record_offset,
                              len(compressed) | (int(delta) << 31))
        records.extend(FRAME_HEADER.pack(len(compressed), int(delta)))
        records.extend(compressed)
        previous = packed
    return bytes(index + records), digest.hexdigest()


def build_atlas(source, source_clips):
    blocks = []
    digests = []
    for clip in source_clips:
        block, digest = build_clip(source, clip)
        blocks.append(block)
        digests.append(digest)

    offset = GLOBAL_HEADER.size + len(source_clips) * CLIP_HEADER.size
    headers = []
    for clip, block in zip(source_clips, blocks):
        headers.append(CLIP_HEADER.pack(
            clip["name"].encode("ascii"),
            clip["frames"],
            clip["start"],
            offset,
            len(block),
            CANVAS_SIZE * CANVAS_SIZE // 2,
        ))
        offset += len(block)
    output = bytearray(GLOBAL_HEADER.pack(b"GBZ1", 1, CANVAS_SIZE, CANVAS_SIZE, 30, len(source_clips), 0))
    output.extend(b"".join(headers))
    output.extend(b"".join(blocks))
    return bytes(output), digests


def main():
    request = urllib.request.Request(SOURCE_URL, headers={"User-Agent": "badge-grokbot-atlas/2"})
    with urllib.request.urlopen(request, timeout=120) as response:
        source = response.read()
    source_digest = hashlib.sha256(source).hexdigest()
    if len(source) != 8799421 or source_digest != SOURCE_SHA256:
        raise ValueError(f"source atlas mismatch: {len(source)} bytes, sha256={source_digest}")
    source_clips = parse_atlas(source)
    output, expected_digests = build_atlas(source, source_clips)
    output_clips = parse_atlas(output, CANVAS_SIZE, CANVAS_SIZE * CANVAS_SIZE // 2)
    for source_clip, output_clip, expected_digest in zip(source_clips, output_clips, expected_digests):
        if expected_digest != output_clip["pixels_sha256"]:
            raise ValueError(f"pixel mismatch after rebuild: {source_clip['name']}")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(output)
    output_digest = hashlib.sha256(output).hexdigest()
    print(f"source {SOURCE_URL}")
    print(f"source sha256 {source_digest}")
    print(f"wrote {OUTPUT} ({len(output)} bytes, {len(output) / 1024 / 1024:.2f} MiB)")
    print(f"output sha256 {output_digest}")
    for clip in output_clips:
        print(f"{clip['name']:16} {clip['frames']:3} frames start={clip['start']} pixels={clip['pixels_sha256'][:16]}")
    print("Personal/local use only; upstream role artwork and atlas are not licensed for redistribution.")


if __name__ == "__main__":
    main()
