

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

### Build all components:
```bash
make
# or
make all
```

### Build individual components:

- **Libraries only:**
  ```bash
  make lib
  ```
- **Applications only:**
  ```bash
  make applications
  ```
- **In-OS toolchain only:**
  ```bash
  make toolchain
  ```
- **Clean build artifacts:**
  ```bash
  make clean
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

### Method 2: Using `make load-apps` / `make make-apps` / `make install-disk`

The disk image path is passed through the `DISK=` variable:

```bash
make load-apps DISK=/path/to/disk.img
# or
make make-apps DISK=/path/to/disk.img
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

The `tools.load_applications` module exports a ready-to-use function, `load_applications_to_disk`:

```python
from tools.load_applications import load_applications_to_disk

# Load applications, libraries, headers, and fonts onto the specified disk image
status = load_applications_to_disk(
    image_path="/path/to/disk.img",
    start_lba=2048,
    auto_format=True
)
print(f"Result: {status}")
```

---

## 🛠️ Disk Utilities

- **`tools/disk_editor.py`** — A fully featured interface for the IPO_FS file system:
  ```bash
  python3 tools/disk_editor.py --image build/disk.img ls /
  python3 tools/disk_editor.py --image build/disk.img put local_file.txt /dest.txt
  python3 tools/disk_editor.py --image build/disk.img get /remote.txt local.txt
  python3 tools/disk_editor.py --image build/disk.img format
  ```
- **`tools/build_font_db.py`** — Generates IFNT binary font tables (supports ASCII, Cyrillic, and Chinese characters).
- **`tools/format_disk_via_os.py`** — Partitions and formats the system disk.
- **`tools/create_live_image.py`** — Builds a bootable Live CD / Hybrid ISO.

---

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (Owner) — Developer and creator of the project idea.

# 📜 License

This project is licensed under the [MIT License](./LICENSE).

