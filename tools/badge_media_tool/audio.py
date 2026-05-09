from __future__ import annotations

import shutil
import tempfile
import wave
from pathlib import Path

import miniaudio


SAMPLE_RATE = 24000
CHANNELS = 1
SAMPLE_WIDTH = 2


def decode_audio_to_samples(input_path: str) -> list[int]:
    source = Path(input_path)
    if not source.exists():
        raise FileNotFoundError(source)
    with tempfile.TemporaryDirectory(prefix="badge_audio_") as tmp:
        temp_source = Path(tmp) / f"source{source.suffix.lower() or '.audio'}"
        shutil.copyfile(source, temp_source)
        decoded = miniaudio.decode_file(
            str(temp_source),
            output_format=miniaudio.SampleFormat.SIGNED16,
            nchannels=CHANNELS,
            sample_rate=SAMPLE_RATE,
        )
        return list(decoded.samples)


def write_wav(output_path: str, samples: list[int]) -> Path:
    target = Path(output_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(target), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(b"".join(int(sample).to_bytes(2, "little", signed=True) for sample in samples))
    return target


def convert_audio_to_badge_wav(input_path: str, output_path: str) -> Path:
    samples = decode_audio_to_samples(input_path)
    if not samples:
        raise ValueError("audio produced no samples")
    return write_wav(output_path, samples)
