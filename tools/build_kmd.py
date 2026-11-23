#!/usr/bin/env python3
"""Package an i386 ET_EXEC --emit-relocs ELF into a pure headerless bare-metal IPO_OS kernel module."""

import argparse
import re
import struct
import sys
from pathlib import Path

KMOD_RELOC_ABS32 = 1
KMOD_RELOC_REL32 = 2

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9

SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1

R_386_NONE = 0
R_386_32 = 1
R_386_PC32 = 2

CRT_TABLE_SYMS = ("_kmd_reloc_tbl", "_kmd_ext_tbl", "_kmd_meta_tbl")


class Elf32:
    def __init__(self, data: bytes):
        self.data = data
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("Input must be 32-bit little-endian ELF")
        eh = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
        self.e_shoff = eh[6]
        self.e_shentsize = eh[11]
        self.e_shnum = eh[12]
        self.e_shstrndx = eh[13]

        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            sh = struct.unpack_from("<10I", data, off)
            self.sections.append({
                "index": i,
                "sh_name": sh[0],
                "sh_type": sh[1],
                "sh_flags": sh[2],
                "sh_addr": sh[3],
                "sh_offset": sh[4],
                "sh_size": sh[5],
                "sh_link": sh[6],
                "sh_info": sh[7],
                "sh_addralign": sh[8],
                "sh_entsize": sh[9],
                "name": "",
            })

        shstr = self.sections[self.e_shstrndx]
        shstr_bytes = data[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]]
        for sec in self.sections:
            sec["name"] = self._cstr(shstr_bytes, sec["sh_name"])

        self.syms = []
        self.sym_by_name = {}
        for sec in self.sections:
            if sec["sh_type"] == SHT_SYMTAB:
                strsec = self.sections[sec["sh_link"]]
                strtab = data[strsec["sh_offset"]:strsec["sh_offset"] + strsec["sh_size"]]
                entsz = sec["sh_entsize"] or 16
                cnt = sec["sh_size"] // entsz
                for i in range(cnt):
                    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                        "<IIIBBH", data, sec["sh_offset"] + i * entsz
                    )
                    name = self._cstr(strtab, st_name)
                    sym = {
                        "index": i,
                        "name": name,
                        "st_value": st_value,
                        "st_size": st_size,
                        "st_info": st_info,
                        "st_shndx": st_shndx,
                    }
                    self.syms.append(sym)
                    if name:
                        self.sym_by_name[name] = sym
                break

    @staticmethod
    def _cstr(buf: bytes, off: int) -> str:
        end = buf.find(b"\x00", off)
        if end < 0:
            end = len(buf)
        return buf[off:end].decode("utf-8", errors="replace")

    def section_bytes(self, name: str) -> bytes:
        for sec in self.sections:
            if sec["name"] == name:
                if sec["sh_type"] == SHT_NOBITS:
                    return b""
                return self.data[sec["sh_offset"]:sec["sh_offset"] + sec["sh_size"]]
        return b""

    def collect_relocs(self):
        """Return (internal_self_reloc_offsets, external_relocs[(offset, sym_name, kmod_type)])."""
        internal_offsets = []
        external = []

        for sec in self.sections:
            if sec["sh_type"] != SHT_REL:
                continue
            target_idx = sec["sh_info"]
            if target_idx >= len(self.sections):
                continue
            tsec = self.sections[target_idx]
            if not (tsec["sh_flags"] & SHF_ALLOC) or tsec["sh_type"] != SHT_PROGBITS:
                continue

            entsz = sec["sh_entsize"] or 8
            cnt = sec["sh_size"] // entsz
            for i in range(cnt):
                r_offset, r_info = struct.unpack_from("<II", self.data, sec["sh_offset"] + i * entsz)
                r_type = r_info & 0xFF
                r_sym = r_info >> 8
                if r_type == R_386_NONE:
                    continue
                if r_sym >= len(self.syms):
                    raise ValueError(f"Relocation sym index out of range: {r_sym}")
                sym = self.syms[r_sym]

                if r_sym == 0 or sym["st_shndx"] == SHN_ABS:
                    if r_type == R_386_PC32:
                        raise ValueError(f"PC32 relocation against non-relocatable symbol '{sym['name']}'")
                    continue

                if sym["st_shndx"] == SHN_UNDEF:
                    if not sym["name"]:
                        raise ValueError(f"Relocation against unnamed undefined symbol #{r_sym}")
                    if r_type == R_386_32:
                        external.append((r_offset, sym["name"], KMOD_RELOC_ABS32))
                    elif r_type == R_386_PC32:
                        external.append((r_offset, sym["name"], KMOD_RELOC_REL32))
                    else:
                        raise ValueError(f"Unsupported external i386 reloc type {r_type} for {sym['name']}")
                else:
                    if r_type == R_386_32:
                        if sym["name"] in CRT_TABLE_SYMS:
                            continue
                        internal_offsets.append(r_offset)
                    elif r_type == R_386_PC32:
                        pass
                    else:
                        raise ValueError(f"Unsupported internal i386 reloc type {r_type} for {sym['name']}")

        return sorted(set(internal_offsets)), sorted(external)


def load_ksyms(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    return set(re.findall(r'\{\s*"([^"]+)"\s*,', text))


def validate_external_ksyms(ksyms_arg: str, external):
    """Validate unresolved KMD symbols against kernel_syms_autogen.c when available."""
    if not ksyms_arg:
        return
    path = Path(ksyms_arg)
    try:
        known = load_ksyms(path)
    except OSError as exc:
        print(f"[build_kmd] WARNING: unable to read ksyms '{path}' ({exc}); skipping symbol check",
              file=sys.stderr)
        return
    if not known:
        print(f"[build_kmd] WARNING: no symbols parsed from '{path}'; skipping symbol check",
              file=sys.stderr)
        return
    missing = sorted({sym for _, sym, _ in external if sym not in known})
    if missing:
        raise ValueError(f"Unresolved kernel symbol(s) not in {ksyms_arg}: {', '.join(missing)}")


def asm_escape(s: str) -> str:
    return (
        s.replace("\\", "\\\\")
         .replace('"', '\\"')
         .replace("\n", "\\n")
         .replace("\r", "\\r")
         .replace("\t", "\\t")
    )


def emit_self_relocs_s(elf: Elf32, args, out_path: Path):
    internal_offsets, external = elf.collect_relocs()
    validate_external_ksyms(args.ksyms, external)

    cleanup_sym = elf.sym_by_name.get("kmd_main_cleanup")
    cleanup_off = cleanup_sym["st_value"] if cleanup_sym else 0xFFFFFFFF

    sym_relocs = {}
    for off, sym_name, rtype in external:
        if off >= 0x80000000:
            raise ValueError(f"Relocation offset exceeds 2GiB: 0x{off:08x}")
        encoded = off | (0x80000000 if rtype == KMOD_RELOC_REL32 else 0)
        sym_relocs.setdefault(sym_name, []).append(encoded)
    ordered_syms = list(sym_relocs.keys())

    lines = [
        "/* Autogenerated by tools/build_kmd.py */",
        '.section .kmd_self_relocs, "a", @progbits',
        ".global _kmd_reloc_tbl",
        ".global _kmd_ext_tbl",
        ".global _kmd_meta_tbl",
        "_kmd_reloc_tbl:",
        f"    .long {len(internal_offsets)}",
    ]
    for off in internal_offsets:
        lines.append(f"    .long 0x{off:08x}")

    lines.append("_kmd_ext_tbl:")
    lines.append(f"    .long {len(ordered_syms)}")
    for idx, sname in enumerate(ordered_syms):
        offs = sym_relocs[sname]
        lines.append(f"    .long .Lkmd_ext_str_{idx} - _kmd_ext_tbl")
        lines.append(f"    .long {len(offs)}")
        for val in offs:
            lines.append(f"    .long 0x{val:08x}")

    lines.append("_kmd_meta_tbl:")
    lines.append("    .long .Lkmd_meta_name - _kmd_meta_tbl" if args.name else "    .long 0")
    lines.append("    .long .Lkmd_meta_author - _kmd_meta_tbl" if args.author else "    .long 0")
    lines.append("    .long .Lkmd_meta_desc - _kmd_meta_tbl" if args.description else "    .long 0")
    lines.append(f"    .long 0x{cleanup_off:08x}")

    for idx, sname in enumerate(ordered_syms):
        lines.append(f'.Lkmd_ext_str_{idx}: .asciz "{asm_escape(sname)}"')
    if args.name:
        lines.append(f'.Lkmd_meta_name: .asciz "{asm_escape(args.name)}"')
    if args.author:
        lines.append(f'.Lkmd_meta_author: .asciz "{asm_escape(args.author)}"')
    if args.description:
        lines.append(f'.Lkmd_meta_desc: .asciz "{asm_escape(args.description)}"')

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_baremetal(elf: Elf32, args) -> bytes:
    internal_offsets, external = elf.collect_relocs()
    validate_external_ksyms(args.ksyms, external)

    init_sym = elf.sym_by_name.get("kmd_init_entry")
    if not init_sym or init_sym["st_value"] != 0:
        raise ValueError("Bare-metal module requires kmd_init_entry at VMA 0x00000000")

    data_sec = next((s for s in elf.sections if s["name"] == ".data"), None)
    if not data_sec:
        raise ValueError("Missing .data section")
    data_bytes = elf.section_bytes(".data")

    # Verify self-reloc table baked in Pass 2 matches Pass 2 internal relocs exactly
    tbl_sym = elf.sym_by_name.get("_kmd_reloc_tbl")
    if tbl_sym is None:
        raise ValueError("Missing _kmd_reloc_tbl symbol in Pass 2 ELF")
    rel_tbl_in_data = tbl_sym["st_value"] - data_sec["sh_addr"]
    tbl_count = struct.unpack_from("<I", data_bytes, rel_tbl_in_data)[0]
    if tbl_count != len(internal_offsets):
        raise ValueError(f"Self-reloc count mismatch: Pass1 table={tbl_count}, Pass2={len(internal_offsets)}")
    if tbl_count > 0:
        tbl_entries = list(struct.unpack_from(f"<{tbl_count}I", data_bytes, rel_tbl_in_data + 4))
        if tbl_entries != internal_offsets:
            raise ValueError("Pass 2 internal relocation offsets shifted unexpectedly!")

    # Verify external symbol table baked in Pass 2 matches Pass 2 external relocs
    ext_sym = elf.sym_by_name.get("_kmd_ext_tbl")
    if ext_sym is None:
        raise ValueError("Missing _kmd_ext_tbl symbol in Pass 2 ELF")
    ext_in_data = ext_sym["st_value"] - data_sec["sh_addr"]
    ext_sym_cnt = struct.unpack_from("<I", data_bytes, ext_in_data)[0]
    cursor = ext_in_data + 4
    reconstructed_ext = []
    for _ in range(ext_sym_cnt):
        name_delta, rel_cnt = struct.unpack_from("<II", data_bytes, cursor)
        cursor += 8
        str_off = ext_in_data + name_delta
        end_off = data_bytes.find(b"\x00", str_off)
        sname = data_bytes[str_off:end_off].decode("utf-8", errors="replace")
        for _ in range(rel_cnt):
            (val,) = struct.unpack_from("<I", data_bytes, cursor)
            cursor += 4
            rtype = KMOD_RELOC_REL32 if (val & 0x80000000) else KMOD_RELOC_ABS32
            reconstructed_ext.append((val & 0x7FFFFFFF, sname, rtype))
    if sorted(reconstructed_ext) != external:
        raise ValueError("Pass 2 external relocation table mismatch against ELF relocs!")

    data_start = elf.sym_by_name.get("_kmd_data_start")
    bss_end = elf.sym_by_name.get("_kmd_bss_end")
    if not data_start or not bss_end:
        raise ValueError("Missing linker boundary symbols (_kmd_data_start, _kmd_bss_end)")

    raw_text = elf.section_bytes(".text")
    data_off = data_start["st_value"]
    total_size = bss_end["st_value"]
    if len(raw_text) > data_off or data_off + len(data_bytes) > total_size:
        raise ValueError("Section sizes exceed linker layout boundaries")

    image = bytearray(total_size)
    image[0:len(raw_text)] = raw_text
    image[data_off:data_off + len(data_bytes)] = data_bytes

    if image[:4] in (b"\x7fELF", b"IKMD") or image[:2] == b"#!":
        raise ValueError("Bare-metal output must not begin with container header bytes")

    return bytes(image)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("input_elf", type=Path)
    p.add_argument("--emit-self-relocs-s", type=Path, default=None)
    p.add_argument("-o", "--output", type=Path, default=None)
    p.add_argument("--name", default="module")
    p.add_argument("--author", default="")
    p.add_argument("--description", default="IPO_OS Bare-Metal Driver")
    p.add_argument("--ksyms", type=str, default=None)
    args = p.parse_args()

    elf = Elf32(args.input_elf.read_bytes())

    if args.emit_self_relocs_s:
        args.emit_self_relocs_s.parent.mkdir(parents=True, exist_ok=True)
        emit_self_relocs_s(elf, args, args.emit_self_relocs_s)
        return 0

    if not args.output:
        p.error("--output is required unless --emit-self-relocs-s is specified")

    flat = build_baremetal(elf, args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(flat)
    print(f"[build_kmd] Wrote pure bare-metal {args.output} ({len(flat)} bytes, headerless)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
