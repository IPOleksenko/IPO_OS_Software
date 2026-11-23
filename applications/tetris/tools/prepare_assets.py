#!/usr/bin/env python3
"""Prepare Tetris-game assets inside data/tetris/src/ using relative paths."""

import shutil
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
UPSTREAM_SRC = REPO_ROOT / "third_party" / "Tetris-game" / "src"
OUT_SRC = REPO_ROOT / "data" / "tetris" / "src"


def copy_tree(src: Path, dst: Path, suffix_filter: set[str] | None = None):
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True, exist_ok=True)
    for item in sorted(src.rglob("*")):
        rel = item.relative_to(src)
        target = dst / rel
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            if suffix_filter is not None and item.suffix.lower() not in suffix_filter:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(item, target)


def main():
    if not UPSTREAM_SRC.exists():
        raise SystemExit(f"Missing upstream Tetris-game sources: {UPSTREAM_SRC}")

    copy_tree(UPSTREAM_SRC / "Media", OUT_SRC / "Media", {".png", ".jpg"})
    copy_tree(UPSTREAM_SRC / "fonts", OUT_SRC / "fonts", {".ttf"})
    copy_tree(UPSTREAM_SRC / "logs", OUT_SRC / "logs", {".txt"})
    copy_tree(UPSTREAM_SRC / "Sounds" / "Effects", OUT_SRC / "Sounds" / "Effects", {".wav"})

    # Keep logs clean with LF line endings and empty songs history
    kb = (OUT_SRC / "logs" / "keybinds.txt")
    if kb.exists():
        kb.write_text(kb.read_text(encoding="utf-8").replace("\r\n", "\n"), encoding="utf-8")
    (OUT_SRC / "logs" / "songs.txt").write_text("", encoding="utf-8")

    # Convert 5 representative music tracks to compact 22050 Hz mono PCM WAV (8-bit/16-bit)
    music_out = OUT_SRC / "Sounds" / "Music"
    if music_out.exists():
        shutil.rmtree(music_out)
    music_out.mkdir(parents=True, exist_ok=True)

    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        for idx in range(1, 6):
            mp3_path = UPSTREAM_SRC / "Sounds" / "Music" / f"{idx:02d}.mp3"
            wav_path = music_out / f"{idx:02d}.wav"
            if mp3_path.exists():
                subprocess.run(
                    [
                        ffmpeg, "-y", "-loglevel", "error",
                        "-i", str(mp3_path),
                        "-t", "30", "-ac", "1", "-ar", "22050", "-acodec", "pcm_s16le",
                        str(wav_path),
                    ],
                    check=True,
                )

    print(f"Prepared Tetris assets under {OUT_SRC}")


if __name__ == "__main__":
    main()
