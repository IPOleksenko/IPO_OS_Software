ROOT := ../..
include $(ROOT)/mk/config.mk

POKERED_BUILD    := $(APPS_BUILD)/pokered
POKERED_BIN      := $(POKERED_BUILD)/pokered.bin
POKERED_RUST_DIR := rust
POKERED_TARGET   := i586-ipo-linux-gnu
POKERED_RUST_LIB := $(POKERED_RUST_DIR)/target/$(POKERED_TARGET)/release/libpokered_ipo.a

POKERED_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2

POKERED_OBJS := \
	$(POKERED_BUILD)/ipo_pokered.o \
	$(POKERED_BUILD)/ipo_rust_compat.o

.PHONY: pokered clean

pokered: $(POKERED_BIN)

$(POKERED_RUST_LIB): $(POKERED_RUST_DIR)/src/lib.rs $(POKERED_RUST_DIR)/Cargo.toml $(POKERED_RUST_DIR)/$(POKERED_TARGET).json
	@if [ ! -f ../../third_party/open-pokered/gfx/tilesets/overworld.png ]; then \
		bash ../../third_party/open-pokered/scripts/fetch-gfx.sh; \
	fi
	cd $(POKERED_RUST_DIR) && RUSTC_BOOTSTRAP=1 RUSTFLAGS="-C panic=abort" \
		cargo build --release --target $(POKERED_TARGET).json -Z json-target-spec -Z build-std=std,panic_abort

$(POKERED_BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(POKERED_CFLAGS)

$(POKERED_BIN): $(POKERED_OBJS) $(POKERED_RUST_LIB) $(APPS_BUILD)/entry.o $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building open-pokered → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APPS_BUILD)/entry.o $(POKERED_OBJS) \
		-Wl,--start-group $(POKERED_RUST_LIB) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(POKERED_BUILD)
