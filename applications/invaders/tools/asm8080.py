#!/usr/bin/env python3
"""Minimal 2-pass Intel 8080 assembler for Computer Archeology invaders.asm."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ASM = ROOT / "applications" / "invaders" / "invaders.asm"

REGS = {"B": 0, "C": 1, "D": 2, "E": 3, "H": 4, "L": 5, "M": 6, "A": 7}
RPS = {"B": 0, "BC": 0, "D": 1, "DE": 1, "H": 2, "HL": 2, "SP": 3}
PUSHPOP = {"B": 0, "BC": 0, "D": 1, "DE": 1, "H": 2, "HL": 2, "PSW": 3}

ZERO_OP = {
    "NOP": 0x00, "RLC": 0x07, "RRC": 0x0F, "RAL": 0x17, "RAR": 0x1F,
    "DAA": 0x27, "CMA": 0x2F, "STC": 0x37, "CMC": 0x3F, "HLT": 0x76,
    "RNZ": 0xC0, "RZ": 0xC8, "RET": 0xC9, "RNC": 0xD0, "RC": 0xD8,
    "RPO": 0xE0, "XTHL": 0xE3, "RPE": 0xE8, "PCHL": 0xE9, "XCHG": 0xEB,
    "RP": 0xF0, "DI": 0xF3, "RM": 0xF8, "SPHL": 0xF9, "EI": 0xFB,
}

ALU_REG = {
    "ADD": 0x80, "ADC": 0x88, "SUB": 0x90, "SBB": 0x98,
    "ANA": 0xA0, "XRA": 0xA8, "ORA": 0xB0, "CMP": 0xB8,
}

IMM8_OP = {
    "ADI": 0xC6, "ACI": 0xCE, "SUI": 0xD6, "SBI": 0xDE,
    "ANI": 0xE6, "XRI": 0xEE, "ORI": 0xF6, "CPI": 0xFE,
    "OUT": 0xD3, "IN": 0xDB,
}

IMM16_OP = {
    "SHLD": 0x22, "LHLD": 0x2A, "STA": 0x32, "LDA": 0x3A,
    "JNZ": 0xC2, "JMP": 0xC3, "CNZ": 0xC4, "JZ": 0xCA, "CZ": 0xCC, "CALL": 0xCD,
    "JNC": 0xD2, "CNC": 0xD4, "JC": 0xDA, "CC": 0xDC,
    "JPO": 0xE2, "CPO": 0xE4, "JPE": 0xEA, "CPE": 0xEC,
    "JP": 0xF2, "CP": 0xF4, "JM": 0xFA, "CM": 0xFC,
}


def strip_comment(line: str) -> str:
    in_sq = False
    in_dq = False
    for idx, ch in enumerate(line):
        if ch == "'" and not in_dq:
            in_sq = not in_sq
        elif ch == '"' and not in_sq:
            in_dq = not in_dq
        elif ch == ";" and not in_sq and not in_dq:
            return line[:idx]
    return line


def split_args(s: str) -> list[str]:
    args, cur, in_sq, in_dq = [], [], False, False
    for ch in s:
        if ch == "'" and not in_dq:
            in_sq = not in_sq
            cur.append(ch)
        elif ch == '"' and not in_sq:
            in_dq = not in_dq
            cur.append(ch)
        elif ch == "," and not in_sq and not in_dq:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def eval_expr(expr: str, sym: dict[str, int], pc: int) -> int:
    s = expr.strip()
    def repl_tok(m):
        tok = m.group(0)
        up = tok.upper()
        if re.fullmatch(r"0[xX][0-9A-Fa-f]+", tok):
            return str(int(tok, 16))
        if re.fullmatch(r"[0-9][0-9A-Fa-f]*[Hh]", tok):
            return str(int(tok[:-1], 16))
        if re.fullmatch(r"[01]+[Bb]", tok):
            return str(int(tok[:-1], 2))
        if re.fullmatch(r"[0-9]+", tok):
            return tok
        if tok == "$":
            return str(pc)
        if tok in sym:
            return str(sym[tok])
        raise KeyError(f"Undefined symbol {tok!r} in expr {expr!r}")

    py_expr = re.sub(r"0[xX][0-9A-Fa-f]+|[0-9][0-9A-Fa-f]*[Hh]|[A-Za-z_.$][A-Za-z0-9_.$]*|[0-9]+", repl_tok, s)
    return int(eval(py_expr, {"__builtins__": {}}, {}))


def assemble_pass(lines: list[str], sym: dict[str, int], pass_no: int) -> tuple[bytearray, int]:
    rom = bytearray([0xFF] * 0x2000)
    pc = 0
    max_addr = 0

    for lineno, raw in enumerate(lines, 1):
        code = strip_comment(raw).strip()
        if not code:
            continue
        # Handle optional label:
        while ":" in code:
            idx = code.index(":")
            # Check not inside quotes before ':'
            lbl = code[:idx].strip()
            if lbl and re.fullmatch(r"[A-Za-z_.][A-Za-z0-9_.]*", lbl):
                sym[lbl] = pc
                code = code[idx + 1:].strip()
            else:
                break
        if not code:
            continue

        parts = code.split(None, 1)
        op = parts[0].upper()
        rest = parts[1].strip() if len(parts) > 1 else ""

        if op in ("CPU", "END"):
            if op == "END":
                break
            continue
        if op == "ORG":
            pc = eval_expr(rest, sym, pc)
            continue
        if op == "DB":
            for arg in split_args(rest):
                if len(arg) >= 2 and ((arg[0] == "'" and arg[-1] == "'") or (arg[0] == '"' and arg[-1] == '"')):
                    for ch in arg[1:-1]:
                        if pass_no == 2:
                            rom[pc] = ord(ch) & 0xFF
                        pc += 1
                else:
                    if pass_no == 2:
                        rom[pc] = eval_expr(arg, sym, pc) & 0xFF
                    pc += 1
            if pc > max_addr:
                max_addr = pc
            continue
        if op == "DW":
            for arg in split_args(rest):
                if pass_no == 2:
                    v = eval_expr(arg, sym, pc) & 0xFFFF
                    rom[pc] = v & 0xFF
                    rom[pc + 1] = (v >> 8) & 0xFF
                pc += 2
            if pc > max_addr:
                max_addr = pc
            continue

        args = split_args(rest)
        out = []
        if op in ZERO_OP:
            out = [ZERO_OP[op]]
        elif op == "MOV":
            dst, src = REGS[args[0].upper()], REGS[args[1].upper()]
            out = [0x40 | (dst << 3) | src]
        elif op == "MVI":
            dst = REGS[args[0].upper()]
            v = (eval_expr(args[1], sym, pc) & 0xFF) if pass_no == 2 else 0
            out = [0x06 | (dst << 3), v]
        elif op == "LXI":
            rp = RPS[args[0].upper()]
            v = (eval_expr(args[1], sym, pc) & 0xFFFF) if pass_no == 2 else 0
            out = [0x01 | (rp << 4), v & 0xFF, (v >> 8) & 0xFF]
        elif op == "LDAX":
            out = [0x0A | (RPS[args[0].upper()] << 4)]
        elif op == "STAX":
            out = [0x02 | (RPS[args[0].upper()] << 4)]
        elif op == "INX":
            out = [0x03 | (RPS[args[0].upper()] << 4)]
        elif op == "DCX":
            out = [0x0B | (RPS[args[0].upper()] << 4)]
        elif op == "DAD":
            out = [0x09 | (RPS[args[0].upper()] << 4)]
        elif op == "INR":
            out = [0x04 | (REGS[args[0].upper()] << 3)]
        elif op == "DCR":
            out = [0x05 | (REGS[args[0].upper()] << 3)]
        elif op == "PUSH":
            out = [0xC5 | (PUSHPOP[args[0].upper()] << 4)]
        elif op == "POP":
            out = [0xC1 | (PUSHPOP[args[0].upper()] << 4)]
        elif op in ALU_REG:
            out = [ALU_REG[op] | REGS[args[0].upper()]]
        elif op in IMM8_OP:
            v = (eval_expr(args[0], sym, pc) & 0xFF) if pass_no == 2 else 0
            out = [IMM8_OP[op], v]
        elif op in IMM16_OP:
            v = (eval_expr(args[0], sym, pc) & 0xFFFF) if pass_no == 2 else 0
            out = [IMM16_OP[op], v & 0xFF, (v >> 8) & 0xFF]
        elif op == "RST":
            n = eval_expr(args[0], sym, pc)
            out = [0xC7 | ((n & 7) << 3)]
        else:
            raise ValueError(f"Line {lineno}: unknown opcode {op!r} ({raw!r})")

        if pass_no == 2:
            for i, b in enumerate(out):
                rom[pc + i] = b
        pc += len(out)
        if pc > max_addr:
            max_addr = pc

    return rom[:max_addr], max_addr


def main() -> None:
    asm_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ASM
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("invaders_rom.h")
    lines = asm_path.read_text(encoding="utf-8", errors="replace").splitlines()
    sym: dict[str, int] = {}
    assemble_pass(lines, sym, 1)
    rom, size = assemble_pass(lines, sym, 2)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        f.write("/* Generated from invaders.asm by tools/asm8080.py */\n")
        f.write(f"static const unsigned int invaders_rom_len = {size}u;\n")
        f.write("static const unsigned char invaders_rom[] = {\n")
        for i in range(0, len(rom), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in rom[i:i + 16])
            comma = "," if i + 16 < len(rom) else ""
            f.write(f"    {chunk}{comma}\n")
        f.write("};\n")
    print(f"Assembled {asm_path.name} -> {out_path.name} ({size} bytes, {len(sym)} symbols)")


if __name__ == "__main__":
    main()
