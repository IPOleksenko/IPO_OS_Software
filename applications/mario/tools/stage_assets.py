#!/usr/bin/env python3
"""Stage Super Mario Bros (uMario_Jakowski) BMP & WAV assets into data/mario/files/.

Uses strictly relative repository paths. Filters out unused 10MB full-level
reference BMP screenshots (11.bmp..84.bmp) not loaded by uNext source code,
and resamples WAV audio to 22050 Hz mono 16-bit PCM for SB16 playback.
"""

import re
import shutil
import struct
import wave
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SRC_UNEXT = REPO_ROOT / "third_party" / "uMario_Jakowski" / "uNext"
SRC_FILES = SRC_UNEXT / "files"
OUT_FILES = REPO_ROOT / "data" / "mario" / "files"

TARGET_RATE = 22050


def collect_used_images() -> set[str]:
    src_text = "\n".join(
        p.read_text(encoding="utf-8", errors="ignore")
        for p in list(SRC_UNEXT.glob("*.cpp")) + list(SRC_UNEXT.glob("*.h"))
    )
    img_root = SRC_FILES / "images"
    used: set[str] = {"ico"}
    for p in img_root.rglob("*.bmp"):
        rel = p.relative_to(img_root).as_posix()[:-4]
        if f'"{rel}"' in src_text:
            used.add(rel)
    return used


def resample_wav_22050_mono(src: Path, dst: Path):
    with wave.open(str(src), "rb") as wf:
        chans = wf.getnchannels()
        sw = wf.getsampwidth()
        rate = wf.getframerate()
        raw = wf.readframes(wf.getnframes())

    if sw == 2:
        total = len(raw) // 2
        samples = struct.unpack(f"<{total}h", raw)
        if chans == 2:
            mono = [(samples[i] + samples[i + 1]) // 2 for i in range(0, total - 1, 2)]
        else:
            mono = list(samples)
    elif sw == 1:
        mono = [((b - 128) << 8) for b in (raw[::chans] if chans > 1 else raw)]
    else:
        raise ValueError(f"Unsupported WAV sample width {sw} in {src}")

    if rate != TARGET_RATE and len(mono) > 1:
        out_len = max(1, int(len(mono) * TARGET_RATE / rate))
        resampled = []
        ratio = len(mono) / out_len
        for i in range(out_len):
            pos = i * ratio
            idx = int(pos)
            frac = pos - idx
            s0 = mono[idx]
            s1 = mono[idx + 1] if idx + 1 < len(mono) else s0
            resampled.append(int(s0 + (s1 - s0) * frac))
        mono = resampled

    dst.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(dst), "wb") as out_wf:
        out_wf.setnchannels(1)
        out_wf.setsampwidth(2)
        out_wf.setframerate(TARGET_RATE)
        out_wf.writeframes(struct.pack(f"<{len(mono)}h", *mono))


def main():
    used_imgs = collect_used_images()
    img_src = SRC_FILES / "images"
    img_dst = OUT_FILES / "images"
    img_count = 0
    for rel in sorted(used_imgs):
        src = img_src / f"{rel}.bmp"
        if not src.exists():
            continue
        dst = img_dst / f"{rel}.bmp"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        img_count += 1

    wav_src = SRC_FILES / "sounds"
    wav_dst = OUT_FILES / "sounds"
    wav_count = 0
    for src in sorted(wav_src.glob("*.wav")):
        resample_wav_22050_mono(src, wav_dst / src.name)
        wav_count += 1

    print(f"Staged {img_count} BMP sprites and {wav_count} WAV sounds under {OUT_FILES}")


if __name__ == "__main__":
    main()
