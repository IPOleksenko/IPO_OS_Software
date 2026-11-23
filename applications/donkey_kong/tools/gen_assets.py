#!/usr/bin/env python3
"""Stage Donkey Kong arcade assets under IPO_OS_Software/data/donkey_kong/assets/.

Sprites are sourced directly (via relative repository paths) from
third_party/gi_zeppieri_donkey_kong/assets/sprites/.
"""

import math
import shutil
import struct
import wave
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SRC_SPRITES = REPO_ROOT / "third_party" / "gi_zeppieri_donkey_kong" / "assets" / "sprites"
OUT_DIR = REPO_ROOT / "data" / "donkey_kong" / "assets"


def stage_sprites():
    dst_dir = OUT_DIR / "sprites"
    dst_dir.mkdir(parents=True, exist_ok=True)
    for name in ("jump_man.png", "world.png", "kong.png", "title.png"):
        src = SRC_SPRITES / name
        if not src.exists():
            raise FileNotFoundError(f"Missing upstream sprite sheet: {src}")
        shutil.copyfile(src, dst_dir / name)


def make_wav(path: Path, freq_a: float, freq_b: float, dur_s: float):
    sr = 22050
    n = max(1, int(sr * dur_s))
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        frames = bytearray()
        for i in range(n):
            t = i / n
            f = freq_a + (freq_b - freq_a) * t
            val = 0.25 if math.sin(2.0 * math.pi * f * (i / sr)) >= 0 else -0.25
            env = 1.0 - t * 0.6
            sample = int(max(-32767, min(32767, val * env * 12000)))
            frames += struct.pack("<h", sample)
        wf.writeframes(bytes(frames))


def main():
    for sub in ("sprites", "fonts", "sounds"):
        (OUT_DIR / sub).mkdir(parents=True, exist_ok=True)

    stage_sprites()

    sound_specs = {
        "music_1.wav": (220, 330, 0.45),
        "music_2.wav": (262, 392, 0.45),
        "music_hammer.wav": (440, 587, 0.35),
        "death.wav": (400, 110, 0.45),
        "smash.wav": (180, 90, 0.18),
        "how_high.wav": (330, 660, 0.40),
        "intro.wav": (262, 523, 0.50),
        "jump.wav": (320, 680, 0.14),
        "score.wav": (523, 784, 0.15),
        "run.wav": (160, 200, 0.08),
        "fall.wav": (350, 140, 0.25),
        "win_1.wav": (392, 784, 0.40),
        "win_2.wav": (440, 880, 0.40),
        "win_level.wav": (330, 880, 0.50),
        "spring_bounce.wav": (280, 620, 0.16),
        "spring_fall.wav": (500, 200, 0.20),
        "oil_light.wav": (240, 480, 0.18),
        "boot.wav": (440, 880, 0.20),
        "countdown.wav": (660, 440, 0.12),
    }
    for name, (fa, fb, dur) in sound_specs.items():
        make_wav(OUT_DIR / "sounds" / name, fa, fb, dur)

    ttf_candidates = [
        REPO_ROOT / "assets" / "fonts" / "UbuntuMono-R.ttf",
        Path("/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"),
    ]
    dst_ttf = OUT_DIR / "fonts" / "PressStart2P-Regular.ttf"
    src_ttf = next((p for p in ttf_candidates if p.exists()), None)
    if src_ttf is not None:
        shutil.copyfile(src_ttf, dst_ttf)
    elif not dst_ttf.exists():
        dst_ttf.write_bytes(b"TTF0")

    (OUT_DIR / "dk_config.ini").write_text(
        "[core]\n"
        "target_fps_hz = 60\n"
        "window_width_px = 448\n"
        "window_height_px = 512\n"
        "control_scheme = arrows\n"
        "debug_mode = 0\n"
        "invulnerable_mode = 0\n"
        "classic_mode = 1\n"
        "fullscreen = 0\n",
        encoding="utf-8",
    )
    (OUT_DIR / "hiscores.ini").write_text(
        "NAME0 = IPO\nSCORE0 = 5000\n"
        "NAME1 = DEV\nSCORE1 = 3000\n"
        "NAME2 = ---\nSCORE2 = 0\n"
        "NAME3 = ---\nSCORE3 = 0\n"
        "NAME4 = ---\nSCORE4 = 0\n",
        encoding="utf-8",
    )
    print(f"Generated Donkey Kong assets from gi_zeppieri/donkey_kong sprites under {OUT_DIR}")


if __name__ == "__main__":
    main()
