ROOT := ../..
include $(ROOT)/mk/config.mk

INV_BUILD := $(APPS_BUILD)/invaders
INV_BIN   := $(INV_BUILD)/invaders.bin
INV_8080  := $(THIRD_PARTY_DIR)/invaders/8080/src

INV_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 -Wall -Wextra \
	-I$(INV_BUILD) -I$(INV_8080) \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare

.PHONY: invaders clean

invaders: $(INV_BIN)

$(INV_BUILD)/invaders_rom.h: invaders.asm tools/asm8080.py
	@mkdir -p $(dir $@)
	python3 tools/asm8080.py invaders.asm $@

$(INV_BUILD)/main.o: main.c $(INV_BUILD)/invaders_rom.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(INV_CFLAGS)

$(INV_BUILD)/emu-8080.o: $(INV_8080)/emu-8080.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(INV_CFLAGS)

$(INV_BIN): $(INV_BUILD)/main.o $(INV_BUILD)/emu-8080.o $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Space Invaders (1978 8080 invaders.asm) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(INV_BUILD)/main.o $(INV_BUILD)/emu-8080.o \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(INV_BUILD)
