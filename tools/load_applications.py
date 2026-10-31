#!/usr/bin/env python3
"""
load_applications.py - Utility to populate an IPO_OS filesystem disk image
with all compiled applications, userland libraries, compiler toolchains,
headers, runtime scripts, and fonts.
"""

import argparse
import os
import sys
import subprocess
from pathlib import Path

# Add current tools directory to sys.path so disk_editor can be imported
tools_dir = Path(__file__).resolve().parent
if str(tools_dir) not in sys.path:
    sys.path.insert(0, str(tools_dir))

from disk_editor import DiskImage, DiskError


def safe_mkdir(disk, path):
    try:
        disk.mkdir(path)
    except Exception:
        pass


def safe_put(disk, src_path, dest_path):
    try:
        disk.put(str(src_path), dest_path)
    except Exception as e:
        print(f"Warning putting {src_path} -> {dest_path}: {e}")


def safe_put_executable(disk, src_path, dest_path):
    safe_put(disk, src_path, dest_path)


def load_applications_to_disk(
    image_path: str | Path,
    project_root: Path | None = None,
    apps_dir: Path | None = None,
    toolchain_dir: Path | None = None,
    fonts_dir: Path | None = None,
    start_lba: int = 2048,
    dry_run: bool = False,
    auto_format: bool = False,
) -> int:
    """
    Populates the specified disk image with applications, libraries, headers, and assets.

    :param image_path: Path to the target disk image (e.g. disk.img).
    :param project_root: Root directory of IPO_Applications (defaults to auto-detected root).
    :param apps_dir: Directory containing compiled application .bin binaries.
    :param toolchain_dir: Directory containing compiled in-OS toolchain .elf binaries.
    :param fonts_dir: Directory containing .fnt font files.
    :param start_lba: Starting LBA sector of the IPO_FS partition (default: 2048).
    :param dry_run: If True, simulate actions without writing.
    :param auto_format: If True, formats unformatted disk images automatically.
    :return: 0 on success, non-zero on error.
    """
    if project_root is None:
        project_root = Path(__file__).resolve().parent.parent

    if not image_path:
        raise ValueError("Target disk image path must be specified.")

    image = Path(image_path).resolve()
    print(f"Target disk image: {image}")

    if not image.exists():
        if dry_run:
            print(f"[Dry Run] Note: Target disk {image} does not exist yet (would be created/loaded).")
        elif auto_format:
            print(f"Notice: Disk image {image} does not exist, creating 512MB image...")
            image.parent.mkdir(parents=True, exist_ok=True)
            with open(image, "wb") as f:
                f.truncate(512 * 1024 * 1024)
        else:
            raise FileNotFoundError(
                f"Disk image '{image}' does not exist. "
                f"Specify a valid disk file path via --disk <path> or create one with 'make disk'."
            )

    # Resolve directories
    if apps_dir is None:
        apps_dir = project_root / "build" / "applications"
    else:
        apps_dir = Path(apps_dir).resolve()

    if toolchain_dir is None:
        toolchain_dir = project_root / "build" / "toolchain"
    else:
        toolchain_dir = Path(toolchain_dir).resolve()

    if fonts_dir is None:
        fonts_dir = project_root / "build" / "fonts"
        if not fonts_dir.exists() or not any(fonts_dir.glob("*.fnt")):
            fallback_fonts = project_root / "assets" / "fonts"
            if fallback_fonts.exists() and any(fallback_fonts.glob("*.fnt")):
                fonts_dir = fallback_fonts
    else:
        fonts_dir = Path(fonts_dir).resolve()

    if dry_run:
        print("[Dry Run] Would load applications from:", apps_dir)
        print("[Dry Run] Would load toolchain from:", toolchain_dir)
        print("[Dry Run] Would load fonts from:", fonts_dir)
        return 0

    # Open filesystem
    try:
        disk = DiskImage(str(image), start_lba=start_lba, require_format=False)
    except DiskError as e:
        if auto_format:
            print(f"Notice: Filesystem on {image} is unformatted ({e}). Formatting with IPO_FS...")
            disk = DiskImage(str(image), start_lba=start_lba, require_format=True)
            disk.format_disk()
        else:
            raise RuntimeError(
                f"Filesystem on {image} is not mounted/formatted ({e}). "
                f"Pass --format to auto-format, or format via disk_editor.py format."
            )

    # Mandatory OS system directories
    safe_mkdir(disk, "/applications")
    safe_mkdir(disk, "/lib")
    safe_mkdir(disk, "/fonts")
    safe_mkdir(disk, "/include")

    # Autorun file
    autorun_file = project_root / "assets" / "autorun"
    if not autorun_file.exists():
        autorun_file = project_root / "autorun"
    if autorun_file.exists():
        safe_put(disk, autorun_file, "/autorun")
    else:
        try:
            disk.cat("/autorun")
        except Exception:
            try:
                disk.write_text("/autorun", "")
            except Exception:
                pass

    # 1. Upload applications
    if apps_dir.exists():
        app_files = sorted(apps_dir.rglob("*.bin"))
        print(f"Uploading {len(app_files)} application binary(ies) to /applications...")
        for app in app_files:
            app_name = app.name[:-4] if app.name.endswith(".bin") else app.name
            safe_put_executable(disk, app, f"/applications/{app_name}")
            if app_name == "ifconfig":
                safe_put_executable(disk, app, "/applications/ipconfig")

    # Doom WAD asset
    doom_wad = project_root / "applications" / "doom" / "doom1.wad"
    if doom_wad.exists():
        print("Uploading DOOM game data (doom1.wad)...")
        safe_put(disk, doom_wad, "/applications/doom1.wad")

    # 2. Upload toolchain binaries
    tool_map = {
        "gcc": toolchain_dir / "gcc.elf",
        "g++": toolchain_dir / "g++.elf",
        "ld": toolchain_dir / "ld.elf",
        "as": toolchain_dir / "as.elf",
        "ar": toolchain_dir / "ar.elf",
        "ranlib": toolchain_dir / "ranlib.elf",
        "objcopy": toolchain_dir / "objcopy.elf",
        "objdump": toolchain_dir / "objdump.elf",
        "readelf": toolchain_dir / "readelf.elf",
        "nm": toolchain_dir / "nm.elf",
        "strip": toolchain_dir / "strip.elf",
        "nasm": toolchain_dir / "nasm.elf",
        "micropython": toolchain_dir / "python.elf",
        "python": toolchain_dir / "python.elf",
        "lua": toolchain_dir / "lua" / "lua.elf",
        "luac": toolchain_dir / "lua" / "luac.elf",
    }

    uploaded_tools = 0
    for tool_name, tool_path in tool_map.items():
        if tool_path.exists():
            safe_put_executable(disk, tool_path, f"/applications/{tool_name}")
            uploaded_tools += 1
    if uploaded_tools > 0:
        print(f"Uploaded {uploaded_tools} toolchain executables to /applications.")

    # 3. Upload fonts
    if fonts_dir.exists():
        font_files = sorted(fonts_dir.glob("*.fnt"))
        if font_files:
            print(f"Uploading {len(font_files)} system font(s) to /fonts...")
            for font in font_files:
                dest = f"/fonts/{font.name}"
                disk.put(str(font), dest)

    # 4. Upload compiler headers
    tcc_include = project_root / "applications" / "tcc" / "include"
    if tcc_include.exists():
        for h in tcc_include.glob("*.h"):
            safe_put(disk, h, f"/include/{h.name}")

    include_dir = project_root / "include"
    if not include_dir.exists():
        include_dir = project_root / "applications" / "include"
    if include_dir.exists():
        for h in include_dir.glob("*.h"):
            safe_put(disk, h, f"/include/{h.name}")

        sys_include = include_dir / "sys"
        if sys_include.exists():
            safe_mkdir(disk, "/include/sys")
            for h in sys_include.glob("*.h"):
                safe_put(disk, h, f"/include/sys/{h.name}")

        cxx_include = include_dir / "c++"
        if cxx_include.exists():
            safe_mkdir(disk, "/include/c++")
            for h in cxx_include.glob("*"):
                if h.is_file():
                    safe_put(disk, h, f"/include/c++/{h.name}")

    # 5. Upload C/C++ runtime and libraries
    lib_dir = project_root / "build" / "lib"
    lib_files = [
        "libc.a",
        "libm.a",
        "libstdc++.a",
        "libgcc.a",
        "crt0.o",
        "crti.o",
        "crtn.o",
        "crt1.o",
        "libtcc1.a",
        "libapp.a",
        "libapp_api.a",
    ]
    has_lib = any((lib_dir / lf).exists() for lf in lib_files)
    if has_lib:
        for lf in lib_files:
            p = lib_dir / lf
            if p.exists():
                safe_put(disk, p, f"/lib/{lf}")

        linker_script = project_root / "lib" / "linker.ld"
        if not linker_script.exists():
            linker_script = project_root / "applications" / "lib" / "linker.ld"
        if linker_script.exists():
            safe_put(disk, linker_script, "/lib/linker.ld")

        safe_mkdir(disk, "/lib/python")

    disk.close()
    print(f"✓ Successfully populated IPO_OS filesystem image: {image}")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Load all built applications, libraries, headers, toolchains, and tests into an IPO_OS disk image."
    )
    repo_root = Path(__file__).resolve().parent.parent

    # Allow specifying disk image via positional argument, --disk, -d, or --image
    parser.add_argument(
        "disk_pos",
        nargs="?",
        type=Path,
        default=None,
        help="Optional path to target disk image file (positional shortcut).",
    )
    parser.add_argument(
        "-d",
        "--disk",
        "--image",
        dest="disk_flag",
        type=Path,
        default=None,
        help="Path to target disk image file (e.g. build/disk.img or /path/to/IPO_OS/build/disk.img).",
    )
    parser.add_argument(
        "--project-root",
        type=Path,
        default=repo_root,
        help="Root directory of the IPO_Applications repository.",
    )
    parser.add_argument(
        "--start-lba",
        type=int,
        default=2048,
        help="Starting LBA of IPO_FS partition (default: 2048).",
    )
    parser.add_argument(
        "--apps-dir",
        type=Path,
        default=None,
        help="Directory with compiled application binaries (default: build/applications).",
    )
    parser.add_argument(
        "--toolchain-dir",
        type=Path,
        default=None,
        help="Directory with toolchain binaries (default: build/toolchain).",
    )
    parser.add_argument(
        "--fonts-dir",
        type=Path,
        default=None,
        help="Directory with .fnt font files (default: build/fonts or assets/fonts).",
    )
    parser.add_argument(
        "--format",
        action="store_true",
        dest="auto_format",
        help="Automatically format the disk with IPO_FS if it is not already formatted.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be copied without modifying the disk image.",
    )
    args = parser.parse_args()

    # Determine disk image path priority:
    # 1. --disk / -d / --image flag
    # 2. Positional disk argument
    # 3. Environment variable IPO_DISK or DISK
    # 4. ../build/disk.img (sibling IPO_OS build)
    # 5. build/disk.img
    target_disk = None
    if args.disk_flag:
        target_disk = args.disk_flag
    elif args.disk_pos:
        target_disk = args.disk_pos
    elif os.environ.get("IPO_DISK"):
        target_disk = Path(os.environ["IPO_DISK"])
    elif os.environ.get("DISK"):
        target_disk = Path(os.environ["DISK"])
    else:
        print(
            "Error: Target disk path must be specified explicitly.\n"
            "Usage:\n"
            "  python3 tools/load_applications.py --disk /path/to/disk.img\n"
            "  python3 tools/load_applications.py /path/to/disk.img\n"
            "  make load-apps DISK=/path/to/disk.img\n"
            "  ./load_to_disk.sh /path/to/disk.img",
            file=sys.stderr,
        )
        return 1

    try:
        return load_applications_to_disk(
            image_path=target_disk,
            project_root=args.project_root,
            apps_dir=args.apps_dir,
            toolchain_dir=args.toolchain_dir,
            fonts_dir=args.fonts_dir,
            start_lba=args.start_lba,
            dry_run=args.dry_run,
            auto_format=args.auto_format,
        )
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
