ROOT := ../..
include $(ROOT)/mk/config.mk

PACMAN_UPSTREAM := $(THIRD_PARTY_DIR)/pacman.c
PACMAN_BUILD    := $(APPS_BUILD)/pacman
PACMAN_BIN      := $(PACMAN_BUILD)/pacman.bin

PACMAN_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 \
	-I$(PACMAN_UPSTREAM)/sokol \
	-Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers

PACMAN_OBJS := \
	$(PACMAN_BUILD)/pacman.o \
	$(PACMAN_BUILD)/ipo_sokol.o

.PHONY: pacman clean

pacman: $(PACMAN_BIN)

$(PACMAN_BUILD)/pacman.o: $(PACMAN_UPSTREAM)/pacman.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(PACMAN_CFLAGS)

$(PACMAN_BUILD)/ipo_sokol.o: ipo_sokol.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(PACMAN_CFLAGS)

$(PACMAN_BIN): $(PACMAN_OBJS) $(APPS_BUILD)/entry.o $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building pacman.c → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APPS_BUILD)/entry.o $(PACMAN_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(PACMAN_BUILD)
