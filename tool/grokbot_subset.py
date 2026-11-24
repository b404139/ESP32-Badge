#!/usr/bin/env python3
import hashlib
import struct
import urllib.request
import zlib
from pathlib import Path

COMMIT = "9d917f8dcbe48396c74145341a55c8f7b8492081"
SOURCE_URL = (
    "https://raw.githubusercontent.com/Ocean-lhy/Stopwatch-GrokBot/"
    f"{COMMIT}/main/grokbot.atlas"
)
SOURCE_SHA256 = "df3c750980927732a0b55f6d2d4481f36cc37c10e3d7819f402511333a347996"
OUTPUT = Path(__file__).resolve().parents[1] / "media" / "pet" / "grokbot.atlas"
STATES = [
    "idle", "listening", "thinking", "waiting", "working", "finished",
    "happy", "playful", "sad", "surprised", "scared", "sleeping",
    "drowsy", "curious", "confused", "angry", "excited", "proud",
]
GLOBAL_HEADER = struct.Struct("<4sHHHBBI")
CLIP_HEADER = struct.Struct("<16sHHIII")
FRAME_INDEX = struct.Struct("<II")
FRAME_HEADER = struct.Struct("<IB3x")


def parse_atlas(data):
    if len(data) < GLOBAL_HEADER.size:
        raise ValueError("atlas header truncated")
    magic, version, width, height, fps, clip_count, reserved = GLOBAL_HEADER.unpack_from(data)
    if (magic, version, width, height, fps) != (b"GBZ1", 1, 400, 400, 30):
        raise ValueError(f"unexpected atlas metadata: {magic} {version} {width}x{height} {fps}fps")
    if reserved != 0 or not 0 < clip_count <= 64:
        raise ValueError("invalid atlas clip count or reserved field")
    table_end = GLOBAL_HEADER.size + clip_count * CLIP_HEADER.size
    if table_end > len(data):
        raise ValueError("clip table truncated")

    clips = []
    for i in range(clip_count):
        fields = CLIP_HEADER.unpack_from(data, GLOBAL_HEADER.size + i * CLIP_HEADER.size)
        name, frames, start_frame, offset, size, packed_size = fields
        name = name.split(b"\0", 1)[0].decode("ascii")
        if not name or frames <= start_frame or packed_size != 80000:
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
        previous = None
        digest = hashlib.sha256()
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
                if flags != 0:
                    raise ValueError(f"invalid frame flags: {clip['name']} frame {frame_index}")
                packed = raw
            digest.update(packed)
            previous = packed
        if clip["name"] in seen:
            raise ValueError(f"duplicate clip: {clip['name']}")
        seen.add(clip["name"])
        clip["pixels_sha256"] = digest.hexdigest()
        clip["frame_count"] = clip["frames"]
    return magic, version, width, height, fps, clips


def build_subset(data, clips):
    by_name = {clip["name"]: clip for clip in clips}
    missing = [name for name in STATES if name not in by_name]
    if missing:
        raise ValueError(f"missing states: {', '.join(missing)}")
    selected = [by_name[name] for name in STATES]
    offset = GLOBAL_HEADER.size + len(selected) * CLIP_HEADER.size
    headers = []
    blocks = []
    for clip in selected:
        headers.append(CLIP_HEADER.pack(
            clip["name"].encode("ascii"),
            clip["frames"],
            clip["start"],
            offset,
            clip["size"],
            clip["packed_size"],
        ))
        blocks.append(data[clip["offset"]:clip["offset"] + clip["size"]])
        offset += clip["size"]
    output = bytearray(GLOBAL_HEADER.pack(b"GBZ1", 1, 400, 400, 30, len(selected), 0))
    output.extend(b"".join(headers))
    output.extend(b"".join(blocks))
    return bytes(output), selected


def main():
    request = urllib.request.Request(SOURCE_URL, headers={"User-Agent": "badge-grokbot-subset/1"})
    with urllib.request.urlopen(request, timeout=120) as response:
        source = response.read()
    digest = hashlib.sha256(source).hexdigest()
    if len(source) != 8799421 or digest != SOURCE_SHA256:
        raise ValueError(f"source atlas mismatch: {len(source)} bytes, sha256={digest}")
    _, _, _, _, _, source_clips = parse_atlas(source)
    subset, selected = build_subset(source, source_clips)
    _, _, _, _, _, subset_clips = parse_atlas(subset)
    for original, rebuilt in zip(selected, subset_clips):
        if original["pixels_sha256"] != rebuilt["pixels_sha256"]:
            raise ValueError(f"pixel mismatch after rebuild: {original['name']}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(subset)
    print(f"source {SOURCE_URL}")
    print(f"source sha256 {digest}")
    print(f"wrote {OUTPUT} ({len(subset)} bytes, {len(subset) / 1024 / 1024:.2f} MiB)")
    for clip in subset_clips:
        print(f"{clip['name']:12} {clip['frames']:3} frames start={clip['start']} pixels={clip['pixels_sha256'][:16]}")
    print("Personal/local use only; upstream role artwork and atlas are not licensed for redistribution.")


if __name__ == "__main__":
    main()
