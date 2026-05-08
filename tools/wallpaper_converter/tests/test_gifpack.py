import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

import gifpack


class GifpackTests(unittest.TestCase):
    def make_pixels(self, values):
        return b"".join(int(v).to_bytes(2, "little") for v in values)

    def test_rgb888_to_rgb565_little_endian(self):
        image = Image.new("RGB", (2, 1))
        image.putdata([(255, 0, 0), (0, 255, 0)])
        self.assertEqual(gifpack.rgb888_to_rgb565_bytes(image), b"\x00\xf8\xe0\x07")

    def test_rgba_transparency_uses_color_key(self):
        image = Image.new("RGBA", (2, 1))
        image.putdata([(255, 0, 0, 255), (0, 0, 0, 0)])
        pixels, has_transparency = gifpack.rgba_to_rgb565_bytes_with_transparency(image)
        self.assertTrue(has_transparency)
        self.assertEqual(pixels, b"\x00\xf8\x01\x00")

    def test_build_parse_412(self):
        width = 412
        height = 412
        frame = gifpack.Frame(delay_ms=100, pixels=self.make_pixels([0x001F] * width * height))
        blob = gifpack.build_gifpack([frame], width=width, height=height)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.header.width, width)
        self.assertEqual(parsed.header.height, height)
        self.assertEqual(gifpack.decode_parsed_frames(parsed), [frame.pixels])

    def test_build_parse_64_for_default_ui(self):
        width = 64
        height = 64
        frame = gifpack.Frame(delay_ms=120, pixels=self.make_pixels([0x07E0] * width * height))
        blob = gifpack.build_gifpack([frame], width=width, height=height)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.header.width, width)
        self.assertEqual(parsed.header.height, height)
        self.assertEqual(gifpack.decode_parsed_frames(parsed), [frame.pixels])

    def test_synthetic_count_round_trip(self):
        width = 4
        height = 4
        frame = gifpack.Frame(delay_ms=50, pixels=self.make_pixels([0xFFFF] * width * height), synthetic_count=3)
        blob = gifpack.build_gifpack([frame], width=width, height=height)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.frames[0].delay_ms, 50)
        self.assertEqual(parsed.frames[0].synthetic_count, 3)

    def test_smooth_conversion_uses_low_fps_keyframes_with_visual_delay(self):
        source_frames = []
        for index in range(10):
            image = Image.new("RGB", (8, 8), (index * 20, 0, 0))
            source_frames.append((image, 50))
        frames = gifpack.convert_source_frames_to_frames(
            source_frames,
            fps_cap=5,
            max_duration_ms=1000,
            width=8,
            height=8,
            frame_delay_ms=50,
            synthetic_count=3,
        )
        self.assertEqual([frame.delay_ms for frame in frames], [50, 50, 50])
        self.assertEqual([frame.synthetic_count for frame in frames], [3, 3, 3])

    def test_fast_indexed_raw_round_trip(self):
        width = 8
        height = 8
        palette = self.make_pixels([0x001F, 0x07E0, 0xF800])
        a_values = [0x001F] * width * height
        b_values = list(a_values)
        b_values[width * 3 + 4] = 0xF800
        a_indices = bytes([0] * width * height)
        b_indices = bytearray(a_indices)
        b_indices[width * 3 + 4] = 2
        b_indices = bytes(b_indices)
        a_pixels = self.make_pixels(a_values)
        b_pixels = self.make_pixels(b_values)
        frames = [
            gifpack.Frame(delay_ms=100, pixels=a_pixels, indexed_palette=palette, indexed_indices=a_indices),
            gifpack.Frame(delay_ms=100, pixels=b_pixels, indexed_palette=palette, indexed_indices=b_indices),
        ]
        blob = gifpack.build_gifpack(frames, width=width, height=height, prefer_fast_indexed=True)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.frames[0].encoding, gifpack.ENCODING_INDEXED_RAW)
        self.assertEqual(parsed.frames[1].encoding, gifpack.ENCODING_DELTA_INDEXED_RAW)
        self.assertEqual(gifpack.decode_parsed_frames(parsed), [a_pixels, b_pixels])

    def test_crc_rejects_corruption(self):
        frame = gifpack.Frame(delay_ms=100, pixels=self.make_pixels([0xF800] * 64 * 64))
        blob = bytearray(gifpack.build_gifpack([frame], width=64, height=64))
        blob[-1] ^= 0x55
        with self.assertRaisesRegex(ValueError, "CRC"):
            gifpack.parse_gifpack(bytes(blob))

    def test_convert_default_ui_gif_to_64(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "idle.gif"
            a = Image.new("RGB", (64, 64), "black")
            b = Image.new("RGB", (64, 64), "cyan")
            a.save(path, save_all=True, append_images=[b], duration=[100, 100], loop=0)
            frames = gifpack.convert_gif_to_frames(str(path), fps_cap=10, max_duration_ms=1000, width=64, height=64)
        self.assertEqual(len(frames), 2)
        self.assertEqual(len(frames[0].pixels), 64 * 64 * 2)

    def test_convert_transparent_gif_sets_header_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "transparent.gif"
            a = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            b = Image.new("RGBA", (64, 64), (255, 0, 0, 255))
            a.save(path, save_all=True, append_images=[b], duration=[100, 100], loop=0)
            blob = gifpack.convert_gif_to_gifpack(str(path), fps_cap=10, max_duration_ms=1000, width=64, height=64)
        parsed = gifpack.parse_gifpack(blob)
        self.assertTrue(parsed.header.flags & gifpack.FLAG_TRANSPARENT_COLOR_KEY)


if __name__ == "__main__":
    unittest.main()
