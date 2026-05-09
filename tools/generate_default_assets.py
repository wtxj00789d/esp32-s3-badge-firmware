from __future__ import annotations

from pathlib import Path
import argparse
import bisect
import textwrap

import miniaudio
from PIL import Image


WIDTH = 240
HEIGHT = 240
FPS = 15
SOUND_SAMPLE_RATE = 24000
SOUND_MAX_SECONDS = 18


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def frame_to_1bpp(frame: Image.Image) -> bytes:
    rgba = frame.convert("RGBA")
    bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
    bg.alpha_composite(rgba)
    if bg.size != (WIDTH, HEIGHT):
        bg = bg.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    gray = bg.convert("L")
    out = bytearray()
    byte = 0
    bit = 7
    for value in gray.getdata():
        if value >= 128:
            byte |= 1 << bit
        bit -= 1
        if bit < 0:
            out.append(byte)
            byte = 0
            bit = 7
    if bit != 7:
        out.append(byte)
    return bytes(out)


def load_gif_frames(path: Path) -> tuple[list[Image.Image], list[int]]:
    image = Image.open(path)
    frames = []
    durations = []
    for i in range(getattr(image, "n_frames", 1)):
        image.seek(i)
        frames.append(image.copy())
        durations.append(max(10, int(image.info.get("duration", 30))))
    return frames, durations


def sample_gif(path: Path) -> list[bytes]:
    frames, durations = load_gif_frames(path)
    cumulative = []
    total = 0
    for duration in durations:
        total += duration
        cumulative.append(total)

    target_count = max(1, round(total * FPS / 1000))
    encoded = []
    for target_index in range(target_count):
        t_ms = target_index * 1000 / FPS
        source_index = min(bisect.bisect_right(cumulative, t_ms), len(frames) - 1)
        encoded.append(frame_to_1bpp(frames[source_index]))
    return encoded


def decode_sound(path: Path) -> list[int]:
    decoded = miniaudio.decode_file(
        str(path),
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=SOUND_SAMPLE_RATE,
    )
    samples = list(decoded.samples)
    return samples[: SOUND_SAMPLE_RATE * SOUND_MAX_SECONDS]


def bytes_literal(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return "\n".join(lines)


def int16_literal(data: list[int], indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        lines.append(indent + ", ".join(str(v) for v in chunk) + ",")
    return "\n".join(lines)


def uint32_literal(data: list[int], indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 8):
        chunk = data[i : i + 8]
        lines.append(indent + ", ".join(str(v) for v in chunk) + ",")
    return "\n".join(lines)


def write_assets(header: Path, source: Path, rle_frames: list[bytes], sound: list[int]) -> None:
    offsets = [0]
    wallpaper_blob = bytearray()
    for frame in rle_frames:
        wallpaper_blob.extend(frame)
        offsets.append(len(wallpaper_blob))

    header.write_text(
        textwrap.dedent(
            f"""\
            #pragma once

            #include <cstddef>
            #include <cstdint>

            namespace badge_default_assets {{
            constexpr int kWidth = {WIDTH};
            constexpr int kHeight = {HEIGHT};
            constexpr int kFps = {FPS};
            constexpr int kFrameCount = {len(rle_frames)};
            constexpr int kSoundSampleRate = {SOUND_SAMPLE_RATE};

            bool DecodeWallpaperFrame(int frame_index, uint16_t* dest, size_t pixel_count);
            const int16_t* SoundPcm();
            size_t SoundSampleCount();
            }}
            """
        ),
        encoding="utf-8",
    )

    source.write_text(
        textwrap.dedent(
            f"""\
            #include "badge_default_assets.h"

            namespace {{
            const uint32_t kWallpaperOffsets[] = {{
            {uint32_literal(offsets)}
            }};

            const uint8_t kWallpaperRle[] = {{
            {bytes_literal(bytes(wallpaper_blob))}
            }};

            const int16_t kSoundPcm[] = {{
            {int16_literal(sound)}
            }};
            }}

            namespace badge_default_assets {{
            bool DecodeWallpaperFrame(int frame_index, uint16_t* dest, size_t pixel_count)
            {{
                if (dest == nullptr || pixel_count < static_cast<size_t>(kWidth * kHeight) ||
                    frame_index < 0 || frame_index >= kFrameCount) {{
                    return false;
                }}

                const uint8_t* ptr = kWallpaperRle + kWallpaperOffsets[frame_index];
                const uint8_t* end = kWallpaperRle + kWallpaperOffsets[frame_index + 1];
                size_t out = 0;
                while (ptr < end && out < static_cast<size_t>(kWidth * kHeight)) {{
                    uint8_t bits = *ptr++;
                    for (int bit = 7; bit >= 0 && out < static_cast<size_t>(kWidth * kHeight); --bit) {{
                        dest[out++] = (bits & (1 << bit)) ? 0xFFFF : 0x0000;
                    }}
                }}
                return out == static_cast<size_t>(kWidth * kHeight);
            }}

            const int16_t* SoundPcm()
            {{
                return kSoundPcm;
            }}

            size_t SoundSampleCount()
            {{
                return sizeof(kSoundPcm) / sizeof(kSoundPcm[0]);
            }}
            }}
            """
        ),
        encoding="utf-8",
    )

    print(f"frames={len(rle_frames)} wallpaper_1bpp_bytes={len(wallpaper_blob)} sound_samples={len(sound)} sound_bytes={len(sound) * 2}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gif", required=True, type=Path)
    parser.add_argument("--mp3", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    rle_frames = sample_gif(args.gif)
    sound = decode_sound(args.mp3)
    write_assets(args.header, args.source, rle_frames, sound)


if __name__ == "__main__":
    main()
