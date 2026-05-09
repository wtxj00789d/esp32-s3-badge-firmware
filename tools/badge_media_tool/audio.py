from __future__ import annotations

import shutil
import tempfile
import wave
from pathlib import Path

import miniaudio


SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2


def clamp_sample_range(sample_count: int, start_seconds: float | None, end_seconds: float | None) -> tuple[int, int]:
    start = max(0, int(round((start_seconds or 0.0) * SAMPLE_RATE)))
    end = sample_count if end_seconds is None else max(0, int(round(end_seconds * SAMPLE_RATE)))
    end = min(end, sample_count)
    if end <= start:
        raise ValueError("end time must be greater than start time")
    return start, end


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


def audio_duration_seconds(input_path: str) -> float:
    samples = decode_audio_to_samples(input_path)
    if not samples:
        raise ValueError("audio produced no samples")
    return len(samples) / SAMPLE_RATE


def write_wav(output_path: str, samples: list[int]) -> Path:
    target = Path(output_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(target), "wb") as wav:
        wav.setnchannels(CHANNELS)
        wav.setsampwidth(SAMPLE_WIDTH)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(b"".join(int(sample).to_bytes(2, "little", signed=True) for sample in samples))
    return target


def convert_audio_to_badge_wav(
    input_path: str,
    output_path: str,
    start_seconds: float | None = None,
    end_seconds: float | None = None,
) -> Path:
    samples = decode_audio_to_samples(input_path)
    if not samples:
        raise ValueError("audio produced no samples")
    start, end = clamp_sample_range(len(samples), start_seconds, end_seconds)
    return write_wav(output_path, samples[start:end])
