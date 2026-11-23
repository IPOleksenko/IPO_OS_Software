#!/usr/bin/env python3
"""Stage Cpp-Zelda-2019 assets into data/zelda/Assets using relative paths."""

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC_ASSETS = ROOT / "third_party" / "Cpp-Zelda-2019" / "Assets"
DST_ASSETS = ROOT / "data" / "zelda" / "Assets"


def convert_ogg_to_wav(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "ffmpeg",
        "-y",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-ac",
        "1",
        "-ar",
        "22050",
        "-acodec",
        "pcm_s16le",
        str(dst),
    ]
    subprocess.run(cmd, check=True)


def main() -> None:
    if not SRC_ASSETS.is_dir():
        raise FileNotFoundError(f"Missing upstream assets directory: {SRC_ASSETS}")

    DST_ASSETS.mkdir(parents=True, exist_ok=True)

    # Copy PNG spritesheets / UI icons cleanly
    for png in sorted(SRC_ASSETS.glob("*.png")):
        shutil.copy2(png, DST_ASSETS / png.name)

    # Normalize tilemap.txt CRLF -> LF so POSIX std::getline parses empty lines accurately
    tilemap_src = SRC_ASSETS / "tilemap.txt"
    if tilemap_src.is_file():
        text = tilemap_src.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
        (DST_ASSETS / "tilemap.txt").write_text(text, encoding="utf-8")

    # Convert OGG audio files (top-level and Audio/) into 22050 Hz mono PCM16 WAVs
    for ogg in sorted(SRC_ASSETS.rglob("*.ogg")):
        rel = ogg.relative_to(SRC_ASSETS).with_suffix(".wav")
        convert_ogg_to_wav(ogg, DST_ASSETS / rel)

    print(f"Staged Zelda assets into {DST_ASSETS.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
