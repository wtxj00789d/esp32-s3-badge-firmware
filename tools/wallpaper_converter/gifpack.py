from __future__ import annotations

import struct
import subprocess
import tempfile
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageFilter, ImageSequence

try:
    import heatshrink2
except ImportError:  # pragma: no cover - optional at import time for GUI help screens
    heatshrink2 = None


DEFAULT_WIDTH = 412
DEFAULT_HEIGHT = 412
MAGIC = b"GIFP"
VERSION = 1
FLAG_TRANSPARENT_COLOR_KEY = 0x0001
TRANSPARENT_RGB565 = 0x0001
TRANSPARENT_REPLACEMENT_RGB565 = 0x0002
ENCODING_RLE_RGB565 = 1
ENCODING_DELTA_RLE_RGB565 = 2
ENCODING_HEATSHRINK_RGB565 = 3
ENCODING_DELTA_HEATSHRINK_RGB565 = 4
ENCODING_INDEXED_HEATSHRINK = 5
ENCODING_DELTA_INDEXED_HEATSHRINK = 6
ENCODING_INDEXED_RAW = 7
ENCODING_DELTA_INDEXED_RAW = 8
HEATSHRINK_WINDOW_BITS = 11
HEATSHRINK_LOOKAHEAD_BITS = 4
VIDEO_SUFFIXES = {".mp4", ".mov", ".m4v", ".webm", ".mkv"}
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}

HEADER_STRUCT = struct.Struct("<4sHHHHHHIIII")
FRAME_STRUCT = struct.Struct("<IIHBB")
DELTA_HEADER_STRUCT = struct.Struct("<HHHH")
INDEXED_HEADER_STRUCT = struct.Struct("<H")
ProgressCallback = Callable[[int, int, str], None]


@dataclass(frozen=True)
class Frame:
    delay_ms: int
    pixels: bytes
    has_transparency: bool = False
    indexed_palette: bytes | None = None
    indexed_indices: bytes | None = None
    indexed_colors: int | None = None
    synthetic_count: int = 0


@dataclass(frozen=True)
class Header:
    width: int
    height: int
    frame_count: int
    data_size: int
    crc32: int
    flags: int = 0


@dataclass(frozen=True)
class ParsedFrame:
    delay_ms: int
    encoding: int
    encoded: bytes
    synthetic_count: int = 0


@dataclass(frozen=True)
class ParsedGifpack:
    header: Header
    frames: list[ParsedFrame]


ENCODING_NAMES = {
    ENCODING_RLE_RGB565: "rgb565-rle",
    ENCODING_DELTA_RLE_RGB565: "rgb565-delta-rle",
    ENCODING_HEATSHRINK_RGB565: "rgb565-heatshrink",
    ENCODING_DELTA_HEATSHRINK_RGB565: "rgb565-delta-heatshrink",
    ENCODING_INDEXED_HEATSHRINK: "indexed-heatshrink",
    ENCODING_DELTA_INDEXED_HEATSHRINK: "indexed-delta-heatshrink",
    ENCODING_INDEXED_RAW: "indexed-raw",
    ENCODING_DELTA_INDEXED_RAW: "indexed-delta-raw",
}


def geometry(width: int = DEFAULT_WIDTH, height: int = DEFAULT_HEIGHT) -> tuple[int, int]:
    pixel_count = int(width) * int(height)
    return pixel_count, pixel_count * 2


def rgb888_to_rgb565_bytes(image: Image.Image) -> bytes:
    rgb = image.convert("RGB")
    out = bytearray(rgb.width * rgb.height * 2)
    idx = 0
    for r, g, b in rgb.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[idx] = value & 0xFF
        out[idx + 1] = (value >> 8) & 0xFF
        idx += 2
    return bytes(out)


def rgba_to_rgb565_bytes_with_transparency(image: Image.Image) -> tuple[bytes, bool]:
    rgba = image.convert("RGBA")
    out = bytearray(rgba.width * rgba.height * 2)
    has_transparency = False
    idx = 0
    for r, g, b, a in rgba.getdata():
        if a < 128:
            value = TRANSPARENT_RGB565
            has_transparency = True
        else:
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            if value == TRANSPARENT_RGB565:
                value = TRANSPARENT_REPLACEMENT_RGB565
        out[idx] = value & 0xFF
        out[idx + 1] = (value >> 8) & 0xFF
        idx += 2
    return bytes(out), has_transparency


def build_indexed_frame(image: Image.Image, colors: int) -> tuple[Image.Image, bytes, bytes]:
    if colors <= 0 or colors > 256:
        raise ValueError("indexed colors must be between 1 and 256")
    indexed = image.convert("RGB").quantize(colors=colors, method=Image.Quantize.MEDIANCUT)
    raw_indices = indexed.tobytes()
    used = sorted(set(raw_indices))
    remap = {old: new for new, old in enumerate(used)}
    palette_data = indexed.getpalette()
    palette = bytearray()
    remapped = bytearray(len(raw_indices))

    for old_index in used:
        base = old_index * 3
        r = palette_data[base]
        g = palette_data[base + 1]
        b = palette_data[base + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        palette.append(value & 0xFF)
        palette.append((value >> 8) & 0xFF)

    for i, old_index in enumerate(raw_indices):
        remapped[i] = remap[old_index]

    return indexed.convert("RGB"), bytes(palette), bytes(remapped)


def preprocess_image(image: Image.Image, mode: str | None) -> Image.Image:
    if mode is None:
        return image
    if mode == "unsharp_light":
        return image.filter(ImageFilter.UnsharpMask(radius=0.8, percent=70, threshold=3))
    if mode == "median3":
        return image.filter(ImageFilter.MedianFilter(size=3))
    if mode == "blur1":
        return image.filter(ImageFilter.GaussianBlur(radius=1.0))
    if mode == "blur15":
        return image.filter(ImageFilter.GaussianBlur(radius=1.5))
    raise ValueError(f"unknown preprocess mode: {mode}")


def _pixel_at(pixels: bytes, index: int) -> int:
    pos = index * 2
    return pixels[pos] | (pixels[pos + 1] << 8)


def _append_literal(out: bytearray, literal: list[int]) -> None:
    start = 0
    while start < len(literal):
        chunk = literal[start : start + 128]
        out.append(len(chunk) - 1)
        for value in chunk:
            out.append(value & 0xFF)
            out.append((value >> 8) & 0xFF)
        start += len(chunk)


def rle_encode_rgb565(pixels: bytes) -> bytes:
    if len(pixels) % 2:
        raise ValueError("RGB565 pixel data must have an even byte length")
    count = len(pixels) // 2
    out = bytearray()
    literal: list[int] = []
    i = 0
    while i < count:
        value = _pixel_at(pixels, i)
        repeat = 1
        while i + repeat < count and repeat < 128 and _pixel_at(pixels, i + repeat) == value:
            repeat += 1
        if repeat >= 3:
            if literal:
                _append_literal(out, literal)
                literal.clear()
            out.append(0x80 | (repeat - 1))
            out.append(value & 0xFF)
            out.append((value >> 8) & 0xFF)
            i += repeat
            continue
        literal.append(value)
        i += 1
        if len(literal) == 128:
            _append_literal(out, literal)
            literal.clear()
    if literal:
        _append_literal(out, literal)
    return bytes(out)


def rle_decode_rgb565(encoded: bytes, expected_pixels: int) -> bytes:
    out = bytearray(expected_pixels * 2)
    in_pos = 0
    out_pos = 0
    while in_pos < len(encoded):
        command = encoded[in_pos]
        in_pos += 1
        length = (command & 0x7F) + 1
        if command & 0x80:
            if in_pos + 2 > len(encoded):
                raise ValueError("RLE repeat run is truncated")
            lo = encoded[in_pos]
            hi = encoded[in_pos + 1]
            in_pos += 2
            for _ in range(length):
                if out_pos + 2 > len(out):
                    raise ValueError("RLE decode produced too many pixels")
                out[out_pos] = lo
                out[out_pos + 1] = hi
                out_pos += 2
        else:
            byte_len = length * 2
            if in_pos + byte_len > len(encoded):
                raise ValueError("RLE literal run is truncated")
            if out_pos + byte_len > len(out):
                raise ValueError("RLE decode produced too many pixels")
            out[out_pos : out_pos + byte_len] = encoded[in_pos : in_pos + byte_len]
            in_pos += byte_len
            out_pos += byte_len
    if out_pos != len(out):
        raise ValueError("RLE decode produced too few pixels")
    return bytes(out)


def heatshrink_encode(data: bytes) -> bytes:
    if heatshrink2 is None:
        raise ValueError("heatshrink2 is not installed")
    return heatshrink2.compress(data, window_sz2=HEATSHRINK_WINDOW_BITS, lookahead_sz2=HEATSHRINK_LOOKAHEAD_BITS)


def heatshrink_decode(encoded: bytes, expected_size: int) -> bytes:
    if heatshrink2 is None:
        raise ValueError("heatshrink2 is not installed")
    decoded = heatshrink2.decompress(encoded, window_sz2=HEATSHRINK_WINDOW_BITS, lookahead_sz2=HEATSHRINK_LOOKAHEAD_BITS)
    if len(decoded) != expected_size:
        raise ValueError("heatshrink decode produced unexpected size")
    return decoded


def default_max_duration_ms(fps_cap: int) -> int:
    if fps_cap >= 25:
        return 6000
    if fps_cap >= 20:
        return 8000
    if fps_cap >= 15:
        return 12000
    return 20000


def _delta_rect(previous: bytes, current: bytes, width: int, height: int) -> tuple[int, int, int, int]:
    _pixel_count, frame_bytes = geometry(width, height)
    if len(previous) != frame_bytes or len(current) != frame_bytes:
        raise ValueError(f"delta frames must be {frame_bytes} bytes")
    min_x = width
    min_y = height
    max_x = -1
    max_y = -1
    for y in range(height):
        row = y * width * 2
        for x in range(width):
            pos = row + x * 2
            if previous[pos] != current[pos] or previous[pos + 1] != current[pos + 1]:
                min_x = min(min_x, x)
                min_y = min(min_y, y)
                max_x = max(max_x, x)
                max_y = max(max_y, y)
    if max_x < 0:
        return 0, 0, 0, 0
    return min_x, min_y, max_x - min_x + 1, max_y - min_y + 1


def _rect_bytes(current: bytes, left: int, top: int, rect_w: int, rect_h: int, width: int) -> bytes:
    rect = bytearray(rect_w * rect_h * 2)
    out_pos = 0
    for y in range(top, top + rect_h):
        start = (y * width + left) * 2
        end = start + rect_w * 2
        rect[out_pos : out_pos + rect_w * 2] = current[start:end]
        out_pos += rect_w * 2
    return bytes(rect)


def delta_encode_rgb565(previous: bytes, current: bytes, width: int, height: int) -> bytes:
    left, top, rect_w, rect_h = _delta_rect(previous, current, width, height)
    header = DELTA_HEADER_STRUCT.pack(left, top, rect_w, rect_h)
    if rect_w == 0 or rect_h == 0:
        return header
    return header + rle_encode_rgb565(_rect_bytes(current, left, top, rect_w, rect_h, width))


def delta_encode_heatshrink_rgb565(previous: bytes, current: bytes, width: int, height: int) -> bytes:
    left, top, rect_w, rect_h = _delta_rect(previous, current, width, height)
    header = DELTA_HEADER_STRUCT.pack(left, top, rect_w, rect_h)
    if rect_w == 0 or rect_h == 0:
        return header
    return header + heatshrink_encode(_rect_bytes(current, left, top, rect_w, rect_h, width))


def encode_indexed_frame_payload(palette: bytes, indices: bytes) -> bytes:
    return INDEXED_HEADER_STRUCT.pack(len(palette) // 2) + palette + heatshrink_encode(indices)


def encode_indexed_raw_frame_payload(palette: bytes, indices: bytes) -> bytes:
    return INDEXED_HEADER_STRUCT.pack(len(palette) // 2) + palette + indices


def _delta_encode_indexed(previous: bytes, current: bytes, palette: bytes, indices: bytes, width: int, height: int) -> bytes:
    left, top, rect_w, rect_h = _delta_rect(previous, current, width, height)
    header = DELTA_HEADER_STRUCT.pack(left, top, rect_w, rect_h)
    if rect_w == 0 or rect_h == 0:
        return header + INDEXED_HEADER_STRUCT.pack(len(palette) // 2)
    rect_indices = bytearray(rect_w * rect_h)
    out_pos = 0
    for y in range(top, top + rect_h):
        start = y * width + left
        end = start + rect_w
        rect_indices[out_pos : out_pos + rect_w] = indices[start:end]
        out_pos += rect_w
    return header + INDEXED_HEADER_STRUCT.pack(len(palette) // 2) + palette + heatshrink_encode(bytes(rect_indices))


def _delta_encode_indexed_raw(previous: bytes, current: bytes, palette: bytes, indices: bytes, width: int, height: int) -> bytes:
    left, top, rect_w, rect_h = _delta_rect(previous, current, width, height)
    header = DELTA_HEADER_STRUCT.pack(left, top, rect_w, rect_h)
    if rect_w == 0 or rect_h == 0:
        return header + INDEXED_HEADER_STRUCT.pack(len(palette) // 2)
    rect_indices = bytearray(rect_w * rect_h)
    out_pos = 0
    for y in range(top, top + rect_h):
        start = y * width + left
        end = start + rect_w
        rect_indices[out_pos : out_pos + rect_w] = indices[start:end]
        out_pos += rect_w
    return header + INDEXED_HEADER_STRUCT.pack(len(palette) // 2) + palette + bytes(rect_indices)


def build_gifpack(frames: list[Frame], width: int = DEFAULT_WIDTH, height: int = DEFAULT_HEIGHT, prefer_fast_indexed: bool = False) -> bytes:
    if not frames:
        raise ValueError("gifpack requires at least one frame")
    if len(frames) > 0xFFFF:
        raise ValueError("gifpack supports at most 65535 frames")
    pixel_count, frame_bytes = geometry(width, height)
    entries = bytearray()
    data = bytearray()
    previous_pixels: bytes | None = None
    for frame in frames:
        if len(frame.pixels) != frame_bytes:
            raise ValueError(f"frame pixel data must be {frame_bytes} bytes")
        if not 1 <= frame.delay_ms <= 0xFFFF:
            raise ValueError("frame delay must fit in uint16 milliseconds")
        if not 0 <= frame.synthetic_count <= 0xFF:
            raise ValueError("synthetic frame count must fit in uint8")
        candidates = []
        if prefer_fast_indexed and frame.indexed_palette is not None and frame.indexed_indices is not None and len(frame.indexed_indices) == pixel_count:
            candidates.append((ENCODING_INDEXED_RAW, encode_indexed_raw_frame_payload(frame.indexed_palette, frame.indexed_indices)))
        candidates.append((ENCODING_RLE_RGB565, rle_encode_rgb565(frame.pixels)))
        if heatshrink2 is not None:
            candidates.append((ENCODING_HEATSHRINK_RGB565, heatshrink_encode(frame.pixels)))
            if not prefer_fast_indexed and frame.indexed_palette is not None and frame.indexed_indices is not None and len(frame.indexed_indices) == pixel_count:
                candidates.append((ENCODING_INDEXED_HEATSHRINK, encode_indexed_frame_payload(frame.indexed_palette, frame.indexed_indices)))
        if previous_pixels is not None:
            if prefer_fast_indexed and frame.indexed_palette is not None and frame.indexed_indices is not None and len(frame.indexed_indices) == pixel_count:
                candidates.append((ENCODING_DELTA_INDEXED_RAW, _delta_encode_indexed_raw(previous_pixels, frame.pixels, frame.indexed_palette, frame.indexed_indices, width, height)))
            candidates.append((ENCODING_DELTA_RLE_RGB565, delta_encode_rgb565(previous_pixels, frame.pixels, width, height)))
            if heatshrink2 is not None:
                candidates.append((ENCODING_DELTA_HEATSHRINK_RGB565, delta_encode_heatshrink_rgb565(previous_pixels, frame.pixels, width, height)))
                if not prefer_fast_indexed and frame.indexed_palette is not None and frame.indexed_indices is not None and len(frame.indexed_indices) == pixel_count:
                    candidates.append((ENCODING_DELTA_INDEXED_HEATSHRINK, _delta_encode_indexed(previous_pixels, frame.pixels, frame.indexed_palette, frame.indexed_indices, width, height)))
        if prefer_fast_indexed and frame.indexed_palette is not None and frame.indexed_indices is not None and len(frame.indexed_indices) == pixel_count:
            fast_encodings = {ENCODING_INDEXED_RAW, ENCODING_DELTA_INDEXED_RAW}
            fast_candidates = [item for item in candidates if item[0] in fast_encodings]
            encoding, encoded = min(fast_candidates, key=lambda item: len(item[1]))
        else:
            encoding, encoded = min(candidates, key=lambda item: len(item[1]))
        entries += FRAME_STRUCT.pack(len(data), len(encoded), frame.delay_ms, encoding, frame.synthetic_count)
        data += encoded
        previous_pixels = frame.pixels

    table_off = HEADER_STRUCT.size
    data_off = table_off + len(entries)
    crc = zlib.crc32(entries)
    crc = zlib.crc32(data, crc) & 0xFFFFFFFF
    flags = FLAG_TRANSPARENT_COLOR_KEY if any(frame.has_transparency for frame in frames) else 0
    header = HEADER_STRUCT.pack(MAGIC, VERSION, HEADER_STRUCT.size, width, height, len(frames), flags, table_off, data_off, len(data), crc)
    return header + entries + data


def parse_gifpack(blob: bytes) -> ParsedGifpack:
    if len(blob) < HEADER_STRUCT.size:
        raise ValueError("gifpack header is truncated")
    magic, version, header_size, width, height, frame_count, flags, table_off, data_off, data_size, expected_crc = HEADER_STRUCT.unpack_from(blob, 0)
    if magic != MAGIC:
        raise ValueError("invalid gifpack magic")
    if version != VERSION:
        raise ValueError("unsupported gifpack version")
    if header_size != HEADER_STRUCT.size:
        raise ValueError("unsupported gifpack header size")
    if width == 0 or height == 0:
        raise ValueError("gifpack dimensions are invalid")
    if frame_count == 0:
        raise ValueError("gifpack has no frames")
    table_size = frame_count * FRAME_STRUCT.size
    table_end = table_off + table_size
    data_end = data_off + data_size
    if table_off < header_size or table_end > len(blob) or data_off < table_end or data_end > len(blob):
        raise ValueError("gifpack table or data offsets are invalid")
    actual_crc = zlib.crc32(blob[table_off:table_end])
    actual_crc = zlib.crc32(blob[data_off:data_end], actual_crc) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError("CRC mismatch")
    frames: list[ParsedFrame] = []
    for i in range(frame_count):
        offset, size, delay_ms, encoding, synthetic_count = FRAME_STRUCT.unpack_from(blob, table_off + i * FRAME_STRUCT.size)
        if encoding not in ENCODING_NAMES:
            raise ValueError("unsupported frame encoding")
        if offset + size > data_size:
            raise ValueError("frame data points outside gifpack data section")
        start = data_off + offset
        frames.append(ParsedFrame(delay_ms=delay_ms, encoding=encoding, encoded=blob[start : start + size], synthetic_count=synthetic_count))
    return ParsedGifpack(Header(width, height, frame_count, data_size, expected_crc, flags), frames)


def _decode_delta_rle(previous: bytes, encoded: bytes, width: int, height: int) -> bytes:
    if len(encoded) < DELTA_HEADER_STRUCT.size:
        raise ValueError("delta frame is truncated")
    left, top, rect_w, rect_h = DELTA_HEADER_STRUCT.unpack_from(encoded, 0)
    if left > width or top > height or rect_w > width or rect_h > height or left + rect_w > width or top + rect_h > height:
        raise ValueError("delta rectangle is outside frame bounds")
    current = bytearray(previous)
    if rect_w == 0 or rect_h == 0:
        return bytes(current)
    rect = rle_decode_rgb565(encoded[DELTA_HEADER_STRUCT.size :], rect_w * rect_h)
    in_pos = 0
    for y in range(top, top + rect_h):
        start = (y * width + left) * 2
        current[start : start + rect_w * 2] = rect[in_pos : in_pos + rect_w * 2]
        in_pos += rect_w * 2
    return bytes(current)


def _decode_delta_heatshrink(previous: bytes, encoded: bytes, width: int, height: int) -> bytes:
    if len(encoded) < DELTA_HEADER_STRUCT.size:
        raise ValueError("delta frame is truncated")
    left, top, rect_w, rect_h = DELTA_HEADER_STRUCT.unpack_from(encoded, 0)
    if left > width or top > height or rect_w > width or rect_h > height or left + rect_w > width or top + rect_h > height:
        raise ValueError("delta rectangle is outside frame bounds")
    current = bytearray(previous)
    if rect_w == 0 or rect_h == 0:
        return bytes(current)
    rect = heatshrink_decode(encoded[DELTA_HEADER_STRUCT.size :], rect_w * rect_h * 2)
    in_pos = 0
    for y in range(top, top + rect_h):
        start = (y * width + left) * 2
        current[start : start + rect_w * 2] = rect[in_pos : in_pos + rect_w * 2]
        in_pos += rect_w * 2
    return bytes(current)


def _read_indexed_palette(encoded: bytes) -> tuple[list[int], int]:
    if len(encoded) < INDEXED_HEADER_STRUCT.size:
        raise ValueError("indexed frame is truncated")
    palette_count = INDEXED_HEADER_STRUCT.unpack_from(encoded, 0)[0]
    palette_bytes = palette_count * 2
    payload_off = INDEXED_HEADER_STRUCT.size
    if palette_count == 0 or palette_count > 256 or len(encoded) < payload_off + palette_bytes:
        raise ValueError("indexed frame palette is invalid")
    palette_blob = encoded[payload_off : payload_off + palette_bytes]
    palette = [palette_blob[i] | (palette_blob[i + 1] << 8) for i in range(0, palette_bytes, 2)]
    return palette, payload_off + palette_bytes


def _indices_to_rgb565(palette: list[int], indices: bytes, expected_pixels: int) -> bytes:
    if len(indices) != expected_pixels:
        raise ValueError("indexed frame has unexpected pixel count")
    out = bytearray(expected_pixels * 2)
    out_pos = 0
    for index in indices:
        if index >= len(palette):
            raise ValueError("indexed pixel is outside palette")
        value = palette[index]
        out[out_pos] = value & 0xFF
        out[out_pos + 1] = (value >> 8) & 0xFF
        out_pos += 2
    return bytes(out)


def _decode_indexed(encoded: bytes, expected_pixels: int) -> bytes:
    palette, data_off = _read_indexed_palette(encoded)
    indices = heatshrink_decode(encoded[data_off:], expected_pixels)
    return _indices_to_rgb565(palette, indices, expected_pixels)


def _decode_indexed_raw(encoded: bytes, expected_pixels: int) -> bytes:
    palette, data_off = _read_indexed_palette(encoded)
    return _indices_to_rgb565(palette, encoded[data_off:], expected_pixels)


def _decode_delta_indexed_raw(previous: bytes, encoded: bytes, width: int, height: int) -> bytes:
    if len(encoded) < DELTA_HEADER_STRUCT.size:
        raise ValueError("delta indexed frame is truncated")
    left, top, rect_w, rect_h = DELTA_HEADER_STRUCT.unpack_from(encoded, 0)
    if left > width or top > height or rect_w > width or rect_h > height or left + rect_w > width or top + rect_h > height:
        raise ValueError("delta rectangle is outside frame bounds")
    current = bytearray(previous)
    if rect_w == 0 or rect_h == 0:
        return bytes(current)
    palette, data_off = _read_indexed_palette(encoded[DELTA_HEADER_STRUCT.size :])
    indices = encoded[DELTA_HEADER_STRUCT.size + data_off :]
    if len(indices) != rect_w * rect_h:
        raise ValueError("delta indexed frame has unexpected pixel count")
    in_pos = 0
    for y in range(top, top + rect_h):
        out_pos = (y * width + left) * 2
        for index in indices[in_pos : in_pos + rect_w]:
            if index >= len(palette):
                raise ValueError("indexed pixel is outside palette")
            value = palette[index]
            current[out_pos] = value & 0xFF
            current[out_pos + 1] = (value >> 8) & 0xFF
            out_pos += 2
        in_pos += rect_w
    return bytes(current)


def decode_parsed_frame(parsed: ParsedGifpack, frame: ParsedFrame, previous: bytes | None = None) -> bytes:
    pixel_count, frame_bytes = geometry(parsed.header.width, parsed.header.height)
    if frame.encoding == ENCODING_RLE_RGB565:
        return rle_decode_rgb565(frame.encoded, pixel_count)
    if frame.encoding == ENCODING_DELTA_RLE_RGB565:
        if previous is None:
            raise ValueError("delta frame requires previous frame")
        return _decode_delta_rle(previous, frame.encoded, parsed.header.width, parsed.header.height)
    if frame.encoding == ENCODING_HEATSHRINK_RGB565:
        return heatshrink_decode(frame.encoded, frame_bytes)
    if frame.encoding == ENCODING_DELTA_HEATSHRINK_RGB565:
        if previous is None:
            raise ValueError("delta frame requires previous frame")
        return _decode_delta_heatshrink(previous, frame.encoded, parsed.header.width, parsed.header.height)
    if frame.encoding == ENCODING_INDEXED_HEATSHRINK:
        return _decode_indexed(frame.encoded, pixel_count)
    if frame.encoding == ENCODING_INDEXED_RAW:
        return _decode_indexed_raw(frame.encoded, pixel_count)
    if frame.encoding == ENCODING_DELTA_INDEXED_RAW:
        if previous is None:
            raise ValueError("delta frame requires previous frame")
        return _decode_delta_indexed_raw(previous, frame.encoded, parsed.header.width, parsed.header.height)
    raise ValueError("unsupported frame encoding")


def decode_parsed_frames(parsed: ParsedGifpack) -> list[bytes]:
    decoded: list[bytes] = []
    previous: bytes | None = None
    for frame in parsed.frames:
        pixels = decode_parsed_frame(parsed, frame, previous)
        decoded.append(pixels)
        previous = pixels
    return decoded


def summarize_encodings(frames: list[ParsedFrame]) -> str:
    counts: dict[int, int] = {}
    for frame in frames:
        counts[frame.encoding] = counts.get(frame.encoding, 0) + 1
    parts = []
    for encoding, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
        parts.append(f"{ENCODING_NAMES.get(encoding, str(encoding))} x{count}")
    return ", ".join(parts)


def iter_gif_frames(path: str) -> list[tuple[Image.Image, int]]:
    frames: list[tuple[Image.Image, int]] = []
    with Image.open(path) as image:
        for frame in ImageSequence.Iterator(image):
            duration = int(frame.info.get("duration", image.info.get("duration", 100)) or 100)
            frames.append((frame.convert("RGBA").copy(), max(duration, 20)))
    if not frames:
        raise ValueError("GIF contains no frames")
    return frames


def iter_static_image(path: str) -> list[tuple[Image.Image, int]]:
    with Image.open(path) as image:
        return [(image.convert("RGBA").copy(), 1000)]


def iter_video_frames(path: str, fps_cap: int, max_duration_ms: int, ffmpeg_path: str | None = None, crop: tuple[int, int, int, int] | None = None, output_size: tuple[int, int] | None = None) -> list[tuple[Image.Image, int]]:
    ffmpeg = ffmpeg_path or "ffmpeg"
    delay_ms = max(1, int(1000 / fps_cap))
    with tempfile.TemporaryDirectory(prefix="xiaozhi_gifp_frames_") as tmp:
        tmp_dir = Path(tmp)
        pattern = tmp_dir / "frame_%05d.png"
        filters: list[str] = []
        if crop is not None:
            left, top, right, bottom = crop
            filters.append(f"crop={right - left}:{bottom - top}:{left}:{top}")
        if output_size is not None:
            filters.append(f"scale={output_size[0]}:{output_size[1]}:flags=lanczos")
        filters.append(f"fps={fps_cap}")
        command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-t", f"{max_duration_ms / 1000:.3f}", "-i", str(path), "-vf", ",".join(filters), str(pattern)]
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


def load_media_frames(path: str, fps_cap: int = 15, max_duration_ms: int | None = None, ffmpeg_path: str | None = None, crop: tuple[int, int, int, int] | None = None, output_size: tuple[int, int] | None = None) -> list[tuple[Image.Image, int]]:
    suffix = Path(path).suffix.lower()
    if max_duration_ms is None:
        max_duration_ms = default_max_duration_ms(fps_cap)
    if suffix == ".gif":
        return iter_gif_frames(path)
    if suffix in IMAGE_SUFFIXES:
        return iter_static_image(path)
    if suffix in VIDEO_SUFFIXES:
        return iter_video_frames(path, fps_cap=fps_cap, max_duration_ms=max_duration_ms, ffmpeg_path=ffmpeg_path, crop=crop, output_size=output_size)
    raise ValueError(f"unsupported media type: {suffix or '<none>'}")


def _square_crop(size: tuple[int, int], crop: tuple[int, int, int, int] | None) -> tuple[int, int, int, int]:
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


def _image_has_transparency(image: Image.Image) -> bool:
    if "A" not in image.getbands():
        return False
    alpha = image.getchannel("A")
    return alpha.getextrema()[0] < 255


def convert_source_frames_to_frames(
    source_frames: list[tuple[Image.Image, int]],
    crop: tuple[int, int, int, int] | None = None,
    fps_cap: int = 15,
    max_duration_ms: int | None = None,
    colors: int | None = None,
    preprocess: str | None = None,
    width: int = DEFAULT_WIDTH,
    height: int = DEFAULT_HEIGHT,
    progress: ProgressCallback | None = None,
    progress_base: int = 0,
    progress_total: int = 1,
    frame_delay_ms: int | None = None,
    synthetic_count: int = 0,
) -> list[Frame]:
    if fps_cap <= 0:
        raise ValueError("fps_cap must be positive")
    if not 0 <= synthetic_count <= 0xFF:
        raise ValueError("synthetic_count must fit in uint8")
    if not source_frames:
        raise ValueError("media contains no frames")
    crop = _square_crop(source_frames[0][0].size, crop)
    min_delay = max(1, int(1000 / fps_cap))
    output_delay = min_delay if frame_delay_ms is None else max(1, int(frame_delay_ms))
    if max_duration_ms is None:
        max_duration_ms = default_max_duration_ms(fps_cap)
    elapsed = 0
    next_emit = 0
    output: list[Frame] = []
    for source_index, (image, duration) in enumerate(source_frames):
        if elapsed >= max_duration_ms:
            break
        if elapsed >= next_emit:
            cropped = image.crop(crop).resize((width, height), Image.Resampling.LANCZOS)
            cropped = preprocess_image(cropped, preprocess)
            indexed_palette = None
            indexed_indices = None
            if colors is not None:
                cropped, indexed_palette, indexed_indices = build_indexed_frame(cropped, colors)
            pixels, has_transparency = rgba_to_rgb565_bytes_with_transparency(cropped)
            output.append(Frame(delay_ms=output_delay, pixels=pixels, has_transparency=has_transparency, indexed_palette=indexed_palette, indexed_indices=indexed_indices, indexed_colors=(len(indexed_palette) // 2) if indexed_palette is not None else None, synthetic_count=synthetic_count))
            next_emit += min_delay
            if next_emit <= elapsed:
                next_emit = elapsed + min_delay
        elapsed += duration
        if progress is not None and (source_index % 4 == 0 or source_index == len(source_frames) - 1):
            progress(min(progress_base + source_index + 1, progress_total), progress_total, "Converting frames")
    if not output:
        image = source_frames[0][0].crop(crop).resize((width, height), Image.Resampling.LANCZOS)
        pixels, has_transparency = rgba_to_rgb565_bytes_with_transparency(preprocess_image(image, preprocess))
        output.append(Frame(delay_ms=output_delay, pixels=pixels, has_transparency=has_transparency, synthetic_count=synthetic_count))
    return output


def convert_gif_to_frames(path: str, crop: tuple[int, int, int, int] | None = None, fps_cap: int = 15, max_duration_ms: int | None = None, width: int = DEFAULT_WIDTH, height: int = DEFAULT_HEIGHT) -> list[Frame]:
    return convert_source_frames_to_frames(iter_gif_frames(path), crop, fps_cap=fps_cap, max_duration_ms=max_duration_ms, width=width, height=height)


def convert_media_to_gifpack_with_details(
    path: str,
    crop: tuple[int, int, int, int] | None = None,
    max_size: int | None = None,
    fps_cap: int = 15,
    max_duration_ms: int | None = None,
    width: int = DEFAULT_WIDTH,
    height: int = DEFAULT_HEIGHT,
    ffmpeg_path: str | None = None,
    progress: ProgressCallback | None = None,
    smooth: bool = False,
    smooth_real_fps: int = 6,
    force_rgb565: bool = False,
    pre_sharpen: bool = False,
) -> tuple[bytes, list[Frame]]:
    visual_fps = fps_cap
    if smooth:
        if smooth_real_fps <= 0:
            raise ValueError("smooth_real_fps must be positive")
        if visual_fps < smooth_real_fps:
            raise ValueError("smooth visual fps must be greater than or equal to smooth_real_fps")
        synthetic_count = max(0, round(visual_fps / smooth_real_fps) - 1)
        source_fps_cap = smooth_real_fps
        frame_delay_ms = max(1, int(1000 / visual_fps))
        duration_fps = source_fps_cap
    else:
        synthetic_count = 0
        source_fps_cap = fps_cap
        frame_delay_ms = None
        duration_fps = fps_cap
    if max_duration_ms is None:
        max_duration_ms = default_max_duration_ms(duration_fps)
    suffix = Path(path).suffix.lower()
    if suffix in VIDEO_SUFFIXES:
        source_frames = load_media_frames(path, fps_cap=source_fps_cap, max_duration_ms=max_duration_ms, ffmpeg_path=ffmpeg_path, crop=crop, output_size=(width, height))
        work_crop = (0, 0, width, height)
    else:
        source_frames = load_media_frames(path, fps_cap=source_fps_cap, max_duration_ms=max_duration_ms, ffmpeg_path=ffmpeg_path)
        work_crop = crop
    source_has_transparency = any(_image_has_transparency(image) for image, _duration in source_frames)
    prefer_fast_indexed = max_size is None and width > 64 and height > 64 and not source_has_transparency and not force_rgb565
    attempts: list[tuple[int | None, str | None]]
    if source_has_transparency or force_rgb565:
        attempts = [(None, "unsharp_light" if pre_sharpen and not source_has_transparency else None)]
    elif prefer_fast_indexed:
        attempts = [(256, None)]
    elif max_size is None:
        attempts = [(None, None), (256, None), (192, None), (128, None), (96, None), (64, None)]
    else:
        attempts = [
            (256, None), (224, None), (192, None), (160, None),
            (128, None), (96, None), (64, None), (48, None), (32, None),
            (256, "median3"), (192, "median3"), (128, "median3"), (96, "median3"), (64, "median3"),
        ]
    progress_total = max(1, len(attempts) * len(source_frames))
    best_blob: bytes | None = None
    best_frames: list[Frame] | None = None
    for attempt_index, (colors, preprocess) in enumerate(attempts):
        progress_base = attempt_index * len(source_frames)
        if progress is not None:
            progress(progress_base, progress_total, "Trying compression")
        frames = convert_source_frames_to_frames(source_frames, work_crop, fps_cap=source_fps_cap, max_duration_ms=max_duration_ms, colors=colors, preprocess=preprocess, width=width, height=height, progress=progress, progress_base=progress_base, progress_total=progress_total, frame_delay_ms=frame_delay_ms, synthetic_count=synthetic_count)
        blob = build_gifpack(frames, width=width, height=height, prefer_fast_indexed=prefer_fast_indexed)
        if best_blob is None or len(blob) < len(best_blob):
            best_blob = blob
            best_frames = frames
        if max_size is None or max_size <= 0 or len(blob) <= max_size:
            return blob, frames
    return best_blob if best_blob is not None else blob, best_frames if best_frames is not None else frames


def convert_media_to_gifpack(path: str, crop: tuple[int, int, int, int] | None = None, max_size: int | None = None, fps_cap: int = 15, max_duration_ms: int | None = None, width: int = DEFAULT_WIDTH, height: int = DEFAULT_HEIGHT, ffmpeg_path: str | None = None, progress: ProgressCallback | None = None, smooth: bool = False, smooth_real_fps: int = 6, force_rgb565: bool = False, pre_sharpen: bool = False) -> bytes:
    blob, _frames = convert_media_to_gifpack_with_details(path, crop, max_size=max_size, fps_cap=fps_cap, max_duration_ms=max_duration_ms, width=width, height=height, ffmpeg_path=ffmpeg_path, progress=progress, smooth=smooth, smooth_real_fps=smooth_real_fps, force_rgb565=force_rgb565, pre_sharpen=pre_sharpen)
    return blob


def convert_gif_to_gifpack(path: str, crop: tuple[int, int, int, int] | None = None, fps_cap: int = 15, max_duration_ms: int | None = None, width: int = DEFAULT_WIDTH, height: int = DEFAULT_HEIGHT, max_size: int | None = None) -> bytes:
    return convert_media_to_gifpack(path, crop, max_size=max_size, fps_cap=fps_cap, max_duration_ms=max_duration_ms, width=width, height=height)
