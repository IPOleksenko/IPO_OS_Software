

# IPO_OS_Software

_> Userland ecosystem, libraries, utilities, and runtime environment for **[IPO_OS](https://github.com/IPOleksenko/IPO_OS)**._

The `IPO_OS_Software` repository contains a complete collection of user applications, system libraries, compilers and interpreters, as well as tools for working with virtual disks of the **IPO_FS** file system.

---

## ⚙️ Installing Dependencies

Building libraries, applications, and compilers requires a 32-bit GCC/GCC++ toolchain, NASM, and Python 3:

```bash
./install-dependencies.sh
```

---

## 🔨 Building

### Build all components (`lib`, `drivers`, `applications`, `toolchain`, `font-databases`):
```bash
make
# or
make all
```

### Build individual components:

- **Libraries only** (`build/lib/` — `libc.a`, `libm.a`, `libstdc++.a`, `libapp.a`, `libapp_api.a`, `libbearssl.a`, CRT objects, `libtcc1.a`, `libgcc.a`):
  ```bash
  make lib
  ```
- **Loadable kernel drivers only** (`build/drivers/` — executable `.kmd` wrappers and modules: `touchpad`, `gamepad`, `rtl8139` [RTL8139/E1000/VirtIO-Net + TCP/IP stack], `custom_proto`):
  ```bash
  make drivers
  ```
- **Applications only** (`build/applications/` — user `.bin` executables including `links`, `tcc`, `doom`, `SENSE-The-Game`, `gl_cube`, `gl_gears`, `gl_teapot`, `gpu_info`, `sdk_3d_audio_demo`, `quake`, `wolf3d`, `binfmt`, `elf_loader`):
  ```bash
  make applications
  ```
- **In-OS toolchain only** (`build/toolchain/` — `gcc.elf`, `g++.elf`, binutils `.elf` [`ld`, `as`, `ar`, `nm`, `objcopy`, `objdump`, `readelf`, `ranlib`, `strip`], `lua`/`luac`, `nasm.elf`, `python.elf`):
  ```bash
  make toolchain
  ```
- **Font databases only** (`build/fonts/` — `fixed.fnt`, `terminus.fnt`, `bold.fnt`):
  ```bash
  make font-databases
  ```
- **Clean build artifacts:**
  ```bash
  make clean      # removes build/ and in-tree toolchain intermediates
  make clean-all  # also deletes stray *.o and __pycache__ directories
  ```
  
---

## 💾 Loading Applications onto a Virtual Disk

### Method 1: Using the `load_to_disk.sh` script

The script accepts the disk image path as an argument:

```bash
./load_to_disk.sh /path/to/disk.img
```

Additional options:

```bash
./load_to_disk.sh --help
./load_to_disk.sh /path/to/new_disk.img --format  # Automatically format the disk if necessary
```

### Method 2: Using Makefile disk targets

The disk image path is passed through `DISK=` (or environment variable `DISK` / `IPO_DISK`):

```bash
make create-disk DISK=/path/to/disk.img   # Create 512 MiB raw image if absent
make format-disk DISK=/path/to/disk.img   # Format IPO_FS partition at START_LBA (default: 2048)
make load-apps DISK=/path/to/disk.img     # Automatically passes --format and loads apps/libs/fonts/headers
```

Alternatively, use an environment variable:

```bash
export DISK=/path/to/disk.img
make load-apps
```

### Method 3: Directly using the Python CLI (`tools/load_applications.py`)

```bash
python3 tools/load_applications.py --disk /path/to/disk.img
# or automatically create/format the disk if it does not exist:
python3 tools/load_applications.py --disk /path/to/disk.img --format
```

### Method 4: Python API

The `tools.load_applications` module exports `load_applications_to_disk`:

```python
from tools.load_applications import load_applications_to_disk

# Load applications, toolchain, headers, libraries, and fonts onto the specified disk image
status = load_applications_to_disk(
    image_path="/path/to/disk.img",
    start_lba=2048,
    auto_format=True
)
print(f"Result: {status}")
```

---

## 🛠️ Disk Utilities

- **`tools/disk_editor.py`** — CLI editor for the `IPO_FS` filesystem (supports `format`, `ls`, `cat`, `mkdir`, `touch`, `put`, `rm`, 56-bit inodes; defaults: `--image build/disks/disk.img`, `--start-lba 2048`):
  ```bash
  python3 tools/disk_editor.py --image build/disks/disk.img ls /
  python3 tools/disk_editor.py --image build/disks/disk.img put local_file.txt /dest.txt
  python3 tools/disk_editor.py --image build/disks/disk.img cat /remote.txt > local.txt
  python3 tools/disk_editor.py --image build/disks/disk.img mkdir /mydir
  python3 tools/disk_editor.py --image build/disks/disk.img rm /dest.txt
  python3 tools/disk_editor.py --image build/disks/disk.img format
  ```
- **`tools/build_font_db.py`** — Generates `IFNT` binary font databases in `build/fonts/` (`fixed.fnt`, `terminus.fnt`, `bold.fnt`) covering ASCII, Cyrillic, and Chinese Hanzi glyphs (invoked by `make font-databases`).
- **`tools/format_disk_via_os.py`** — Injects MBR partition 1 (`LBA 2048`) and boots `IPO_OS` (`--os-image IPO_OS.bin` or `--boot`/`--kernel`) in headless `qemu-system-x86_64` to format and mount the `IPO_FS` filesystem on the target disk image (default `--disk build/disks/disk.img`, `--size-mb 128`).
- **`tools/create_live_image.py`** — Builds a unified bootable raw Live USB disk image (`build/IPO_OS.img` by default, 512 MiB or matching `--disk`), combining `IPO_OS.bin` (`--os-image` or `--boot` + `--kernel`), MBR partition table, `IPO_FS` superblocks (`2030`/`2039`), zeroed WAL journal, and `--disk` partition data (invoked by `make live-image`).

---

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (Owner) — Developer and creator of the project idea.

# 📜 License

This project is licensed under the [MIT License](./LICENSE).
