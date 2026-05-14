from __future__ import annotations

import struct
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path

from PIL import Image, ImageSequence


WIDTH = 240
HEIGHT = 240
FRAME_BYTES = WIDTH * HEIGHT * 2
MAX_FPS = 20
DEFAULT_FPS = 15
DEFAULT_MAX_FRAMES = 60
MAGIC = b"BWP1"
HEADER_STRUCT = struct.Struct("<4sHHHHI")
VIDEO_SUFFIXES = {".mp4", ".mov", ".m4v", ".webm", ".mkv"}
ANIMATED_IMAGE_SUFFIXES = {".gif", ".webp"}
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}
ProgressCallback = Callable[[int, int, str], None]


def frames_duration_ms(frames: list[tuple[Image.Image, int]]) -> int:
    return sum(duration for _image, duration in frames)


def default_max_duration_ms(fps: int, max_frames: int = DEFAULT_MAX_FRAMES) -> int:
    fps = max(1, min(MAX_FPS, int(fps)))
    return int(max_frames * 1000 / fps)


def rgb565_bytes(image: Image.Image) -> bytes:
    rgb = image.convert("RGB")
    out = bytearray(rgb.width * rgb.height * 2)
    idx = 0
    for r, g, b in rgb.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[idx] = value & 0xFF
        out[idx + 1] = (value >> 8) & 0xFF
        idx += 2
    return bytes(out)


def iter_image_sequence_frames(path: str) -> list[tuple[Image.Image, int]]:
    frames = []
    with Image.open(path) as image:
        for frame in ImageSequence.Iterator(image):
            duration = int(frame.info.get("duration", image.info.get("duration", 100)) or 100)
            frames.append((frame.convert("RGBA").copy(), max(duration, 20)))
    if not frames:
        raise ValueError("image sequence contains no frames")
    return frames


def iter_static_image(path: str) -> list[tuple[Image.Image, int]]:
    with Image.open(path) as image:
        return [(image.convert("RGBA").copy(), 1000)]


def iter_video_frames(
    path: str,
    fps: int,
    max_duration_ms: int,
    ffmpeg_path: str | None,
    crop: tuple[int, int, int, int] | None,
    output_size: tuple[int, int] | None,
) -> list[tuple[Image.Image, int]]:
    ffmpeg = ffmpeg_path or "ffmpeg"
    delay_ms = max(1, int(1000 / fps))
    with tempfile.TemporaryDirectory(prefix="badge_bwp_frames_") as tmp:
        tmp_dir = Path(tmp)
        pattern = tmp_dir / "frame_%05d.png"
        filters = []
        if crop is not None:
            left, top, right, bottom = crop
            filters.append(f"crop={right - left}:{bottom - top}:{left}:{top}")
        if output_size is not None:
            filters.append(f"scale={output_size[0]}:{output_size[1]}:flags=lanczos")
        filters.append(f"fps={fps}")
        command = [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-t",
            f"{max_duration_ms / 1000:.3f}",
            "-i",
            str(path),
            "-vf",
            ",".join(filters),
            str(pattern),
        ]
        try:
            subprocess.run(command, check=True, capture_output=True, text=True)
        except FileNotFoundError as exc:
            raise ValueError("ffmpeg.exe was not found") from exc
        except subprocess.CalledProcessError as exc:
            detail = (exc.stderr or exc.stdout or str(exc)).strip()
            raise ValueError(f"ffmpeg failed: {detail}") from exc
        frames = []
        for frame_path in sorted(tmp_dir.glob("frame_*.png")):
            with Image.open(frame_path) as image:
                frames.append((image.convert("RGBA").copy(), delay_ms))
    if not frames:
        raise ValueError("video produced no frames")
    return frames


def load_media_frames(
    path: str,
    fps: int,
    max_duration_ms: int,
    ffmpeg_path: str | None = None,
    crop: tuple[int, int, int, int] | None = None,
    output_size: tuple[int, int] | None = None,
) -> list[tuple[Image.Image, int]]:
    suffix = Path(path).suffix.lower()
    if suffix in ANIMATED_IMAGE_SUFFIXES:
        return iter_image_sequence_frames(path)
    if suffix in IMAGE_SUFFIXES:
        return iter_static_image(path)
    if suffix in VIDEO_SUFFIXES:
        return iter_video_frames(path, fps, max_duration_ms, ffmpeg_path, crop, output_size)
    raise ValueError(f"unsupported media type: {suffix or '<none>'}")


def square_crop(size: tuple[int, int], crop: tuple[int, int, int, int] | None) -> tuple[int, int, int, int]:
    src_w, src_h = size
    if crop is None:
        side = min(src_w, src_h)
        left = (src_w - side) // 2
        top = (src_h - side) // 2
        return left, top, left + side, top + side
    left, top, right, bottom = [int(v) for v in crop]
    if left < 0 or top < 0 or right > src_w or bottom > src_h or right <= left or bottom <= top:
        raise ValueError("crop rectangle is outside source bounds or empty")
    if right - left != bottom - top:
        raise ValueError("crop rectangle must be square")
    return left, top, right, bottom


def time_range_ms(start_seconds: float | None, end_seconds: float | None) -> tuple[int, int | None]:
    start_ms = max(0, int(round((start_seconds or 0.0) * 1000)))
    end_ms = None if end_seconds is None else max(0, int(round(end_seconds * 1000)))
    if end_ms is not None and end_ms <= start_ms:
        raise ValueError("end time must be greater than start time")
    return start_ms, end_ms


def trim_frames_by_time(
    source_frames: list[tuple[Image.Image, int]],
    start_ms: int,
    end_ms: int | None,
) -> list[tuple[Image.Image, int]]:
    if not source_frames:
        return []
    elapsed = 0
    trimmed = []
    for image, duration in source_frames:
        frame_start = elapsed
        frame_end = elapsed + duration
        elapsed = frame_end
        if frame_end <= start_ms:
            continue
        if end_ms is not None and frame_start >= end_ms:
            break
        clipped_start = max(frame_start, start_ms)
        clipped_end = min(frame_end, end_ms) if end_ms is not None else frame_end
        clipped_duration = max(1, clipped_end - clipped_start)
        trimmed.append((image, clipped_duration))
    if not trimmed:
        raise ValueError("selected time range contains no frames")
    return trimmed


def convert_frames_to_rgb565(
    source_frames: list[tuple[Image.Image, int]],
    fps: int,
    max_duration_ms: int,
    max_frames: int,
    crop: tuple[int, int, int, int] | None,
    progress: ProgressCallback | None = None,
) -> list[bytes]:
    if not source_frames:
        raise ValueError("media contains no frames")
    fps = max(1, min(MAX_FPS, int(fps)))
    crop = square_crop(source_frames[0][0].size, crop)
    min_delay = max(1, int(1000 / fps))
    elapsed = 0
    next_emit = 0
    output = []
    total = len(source_frames)
    for source_index, (image, duration) in enumerate(source_frames):
        if elapsed >= max_duration_ms or len(output) >= max_frames:
            break
        if elapsed >= next_emit:
            cropped = image.crop(crop).resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
            output.append(rgb565_bytes(cropped))
            next_emit += min_delay
            if next_emit <= elapsed:
                next_emit = elapsed + min_delay
        elapsed += duration
        if progress and (source_index % 4 == 0 or source_index == total - 1):
            progress(min(source_index + 1, total), total, "Converting frames")
    if not output:
        cropped = source_frames[0][0].crop(crop).resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        output.append(rgb565_bytes(cropped))
    return output


def build_bwp(frames: list[bytes], fps: int) -> bytes:
    if not frames:
        raise ValueError("BWP requires at least one frame")
    if len(frames) > 0xFFFF:
        raise ValueError("BWP supports at most 65535 frames")
    for frame in frames:
        if len(frame) != FRAME_BYTES:
            raise ValueError(f"BWP frame must be {FRAME_BYTES} bytes")
    header = HEADER_STRUCT.pack(MAGIC, WIDTH, HEIGHT, int(fps), len(frames), FRAME_BYTES)
    return header + b"".join(frames)


def convert_media_to_bwp(
    input_path: str,
    output_path: str,
    fps: int = DEFAULT_FPS,
    max_frames: int = DEFAULT_MAX_FRAMES,
    crop: tuple[int, int, int, int] | None = None,
    start_seconds: float | None = None,
    end_seconds: float | None = None,
    ffmpeg_path: str | None = None,
    progress: ProgressCallback | None = None,
) -> Path:
    source = Path(input_path)
    if not source.exists():
        raise FileNotFoundError(source)
    fps = max(1, min(MAX_FPS, int(fps)))
    max_frames = max(1, int(max_frames))
    start_ms, end_ms = time_range_ms(start_seconds, end_seconds)
    max_output_duration_ms = default_max_duration_ms(fps, max_frames)
    load_duration_ms = end_ms if end_ms is not None else start_ms + max_output_duration_ms
    suffix = source.suffix.lower()
    if suffix in VIDEO_SUFFIXES:
        source_frames = load_media_frames(str(source), fps, load_duration_ms, ffmpeg_path, crop, (WIDTH, HEIGHT))
        work_crop = (0, 0, WIDTH, HEIGHT)
    else:
        source_frames = load_media_frames(str(source), fps, load_duration_ms, ffmpeg_path)
        work_crop = crop
    selected_frames = trim_frames_by_time(source_frames, start_ms, end_ms)
    selected_duration_ms = min(frames_duration_ms(selected_frames), max_output_duration_ms)
    frames = convert_frames_to_rgb565(selected_frames, fps, selected_duration_ms, max_frames, work_crop, progress)
    blob = build_bwp(frames, fps)
    target = Path(output_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(blob)
    return target
