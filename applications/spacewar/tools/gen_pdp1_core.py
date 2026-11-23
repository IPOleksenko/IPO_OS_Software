#!/usr/bin/env python3
"""Extract assembled 18-bit PDP-1 memory image from third_party/Spacewar/spacewar.asm."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ASM = ROOT / "third_party" / "Spacewar" / "spacewar.asm"

VALID_HEAD_RE = re.compile(
    r"^(sza|szm|spq|clc|law|lac|lio|dap|dip|dio|dac|dzm|add|sub|idx|isp|"
    r"sad|sas|mus|dis|mul|div|jmp|jsp|jda|cal|iot|opr|shf|skp|and|ior|xor|"
    r"xct|dpy|tyi|lsm|ioh|ral|rar|rcl|rcr|sal|sar|scl|scr|ril|rir|sil|sir|"
    r"cla|cli|cma|lat|stf|clf|hlt|szo|spi|spa|sma|szs|szf|nop|[0-7]+|-?[0-7]+)\b"
)


def parse_spacewar_asm(asm_path: Path) -> tuple[list[int], int]:
    mem = [0] * 4096
    start_addr = 4
    lines = asm_path.read_text(encoding="utf-8", errors="replace").splitlines()
    count = 0

    for ln in lines:
        m = re.match(r"^([ +])([0-7]{6})\s+([0-7]{6})\s+(.*)$", ln)
        if not m:
            m_start = re.search(r"\bstart\s+([0-7]+)", ln)
            if m_start:
                start_addr = int(m_start.group(1), 8)
            continue
        code = m.group(4).split("/")[0].strip()
        if not code:
            continue
        # Skip intermediate macro assignment lines (e.g. ZZ2623=ZZ2623+ZZ2623)
        if "=" in code and not VALID_HEAD_RE.match(code):
            continue
        addr = int(m.group(2), 8)
        val = int(m.group(3), 8)
        if 0 <= addr < 4096:
            mem[addr] = val & 0o777777
            count += 1

    return mem, start_addr


def main() -> None:
    asm_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ASM
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("spacewar_rom.h")
    mem, start_addr = parse_spacewar_asm(asm_path)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        f.write("/* Generated from third_party/Spacewar/spacewar.asm by tools/gen_pdp1_core.py */\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define SPACEWAR_START_ADDR 0x{start_addr:03X}u\n\n")
        f.write("static const uint32_t spacewar_init_mem[4096] = {\n")
        for i in range(0, 4096, 8):
            chunk = ", ".join(f"0x{w:05X}u" for w in mem[i:i + 8])
            comma = "," if i + 8 < 4096 else ""
            f.write(f"    {chunk}{comma}\n")
        f.write("};\n")
    print(f"Generated {out_path.name} (start=0{start_addr:o} octal)")


if __name__ == "__main__":
    main()
