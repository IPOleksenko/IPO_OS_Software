ROOT := ../..
include $(ROOT)/mk/config.mk

SW_BUILD := $(APPS_BUILD)/spacewar
SW_BIN   := $(SW_BUILD)/spacewar.bin
SW_ASM   := $(THIRD_PARTY_DIR)/Spacewar/spacewar.asm

SW_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 -Wall -Wextra \
	-I$(SW_BUILD) \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare

.PHONY: spacewar clean

spacewar: $(SW_BIN)

$(SW_BUILD)/spacewar_rom.h: $(SW_ASM) tools/gen_pdp1_core.py
	@mkdir -p $(dir $@)
	python3 tools/gen_pdp1_core.py $(SW_ASM) $@

$(SW_BUILD)/main.o: main.c $(SW_BUILD)/spacewar_rom.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(SW_CFLAGS)

$(SW_BIN): $(SW_BUILD)/main.o $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Spacewar! (PDP-1 1962 spacewar.asm) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(SW_BUILD)/main.o \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(SW_BUILD)
