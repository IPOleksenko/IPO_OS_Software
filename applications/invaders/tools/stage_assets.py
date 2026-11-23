#!/usr/bin/env python3
"""Stage Space Invaders WAV sound effects into data/invaders/wav using relative paths."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC_WAV = ROOT / "third_party" / "invaders" / "wav"
DST_WAV = ROOT / "data" / "invaders" / "wav"


def main() -> None:
    DST_WAV.mkdir(parents=True, exist_ok=True)
    for wav in sorted(SRC_WAV.glob("*.wav")):
        dst = DST_WAV / wav.name
        subprocess.run(
            [
                "ffmpeg", "-y", "-loglevel", "error",
                "-i", str(wav),
                "-ac", "1", "-ar", "22050", "-acodec", "pcm_s16le",
                str(dst),
            ],
            check=True,
        )
    print(f"Staged Space Invaders WAVs into {DST_WAV.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
