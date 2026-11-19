# ==============================================================================
# IPO_OS_Software - Disk Operations & Application Deployment
# ==============================================================================

.PHONY: load-apps format-disk create-disk font-databases live-image require-disk

# Check that target disk path is explicitly provided by the user
require-disk:
	@if [ -z "$(strip $(DISK))" ]; then \
		echo "================================================================="; \
		echo " ERROR: Target disk image path was not specified."; \
		echo " Specify the target disk explicitly:"; \
		echo "   make load-apps DISK=/path/to/disk.img"; \
		echo "   ./load_to_disk.sh /path/to/disk.img"; \
		echo "================================================================="; \
		exit 1; \
	fi

# ------------------------------------------------------------------------------
# Build / extract font database
# ------------------------------------------------------------------------------
font-databases:
	@mkdir -p $(BUILD)/fonts
	@$(PYTHON) $(TOOLS_DIR)/build_font_db.py || { \
		echo "[fonts] build_font_db.py finished with warnings; ensuring fallback fonts..."; \
		cp -f $(ASSETS_DIR)/fonts/*.fnt $(BUILD)/fonts/ 2>/dev/null || true; \
	}
	@cp -n $(ASSETS_DIR)/fonts/*.fnt $(BUILD)/fonts/ 2>/dev/null || true

# ------------------------------------------------------------------------------
# Create raw virtual disk image if not exists
# ------------------------------------------------------------------------------
create-disk: require-disk
	@if [ ! -f "$(DISK)" ]; then \
		echo "[disk] Creating 512MB raw disk image: $(DISK)"; \
		mkdir -p "$$(dirname "$(DISK)")"; \
		dd if=/dev/zero of="$(DISK)" bs=1M count=512 status=none; \
	else \
		echo "[disk] Disk image already exists: $(DISK)"; \
	fi

# ------------------------------------------------------------------------------
# Format disk with IPO_FS partition
# ------------------------------------------------------------------------------
format-disk: require-disk create-disk
	@echo "[disk] Formatting partition at LBA $(START_LBA) on $(DISK) with IPO_FS..."
	@$(PYTHON) $(TOOLS_DIR)/disk_editor.py --image "$(DISK)" --start-lba $(START_LBA) format

# ------------------------------------------------------------------------------
# Load all applications, libraries, headers and assets onto disk
# Must explicitly specify disk path:
#   make load-apps DISK=/path/to/disk.img
# ------------------------------------------------------------------------------
load-apps: require-disk font-databases
	@echo "[disk] Loading applications onto target disk: $(DISK)"
	@$(PYTHON) $(TOOLS_DIR)/load_applications.py --disk "$(DISK)" --start-lba $(START_LBA) --format

# ------------------------------------------------------------------------------
# Create hybrid live image (if needed)
# ------------------------------------------------------------------------------
live-image:
	@if [ -f "$(TOOLS_DIR)/create_live_image.py" ]; then \
		$(PYTHON) $(TOOLS_DIR)/create_live_image.py; \
	fi
