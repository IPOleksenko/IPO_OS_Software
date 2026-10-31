# ==============================================================================
# IPO_Applications - Userland Applications
# ==============================================================================

.PHONY: applications apps clean-applications clean-apps

APPS_SRCS_RAW    := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
GENERIC_APP_SRCS := $(filter-out $(APPS_DIR)/tcc/% $(APPS_DIR)/lua/% $(APPS_DIR)/micropython/% $(APPS_DIR)/nasm/% $(APPS_DIR)/doom/% $(APPS_DIR)/lib/% $(APPS_DIR)/dltest/plugin_sample.c $(APPS_DIR)/binfmt/% $(APPS_DIR)/elf_loader/%, $(APPS_SRCS_RAW))
APPS_BINS        := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(GENERIC_APP_SRCS)) \
                    $(APPS_BUILD)/tcc/tcc.bin \
                    $(APPS_BUILD)/doom/doom.bin \
                    $(APPS_BUILD)/binfmt/binfmt.bin \
                    $(APPS_BUILD)/elf_loader/elf_loader.bin

APP_ENTRY_OBJ    := $(APPS_BUILD)/entry.o

applications: lib $(APPS_BINS)
apps: applications

$(APP_ENTRY_OBJ): $(APPS_DIR)/entry.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS)

# ------------------------------------------------------------------------------
# TinyCC In-OS C Compiler
# ------------------------------------------------------------------------------
$(APPS_BUILD)/tcc/setjmp.o: $(APPS_DIR)/tcc/setjmp.s
	@mkdir -p $(dir $@)
	$(ASM) -f elf32 $< -o $@

$(APPS_BUILD)/tcc/tcc.o: $(APPS_DIR)/tcc/tcc.c $(APPS_DIR)/tcc/libtcc.c $(APPS_DIR)/tcc/tcctools.c $(APPS_DIR)/tcc/tccpp.c $(APPS_DIR)/tcc/tccgen.c $(APPS_DIR)/tcc/tccelf.c $(APPS_DIR)/tcc/tccasm.c $(APPS_DIR)/tcc/tccrun.c $(APPS_DIR)/tcc/tccdbg.c $(APPS_DIR)/tcc/i386-gen.c $(APPS_DIR)/tcc/i386-link.c $(APPS_DIR)/tcc/i386-asm.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(TCC_CFLAGS)

$(APPS_BUILD)/tcc/tcc.bin: $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/setjmp.o $(APP_ENTRY_OBJ) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building official TinyCC: $(APPS_DIR)/tcc/tcc.c → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APP_ENTRY_OBJ) $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/setjmp.o \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# ------------------------------------------------------------------------------
# Official DOOM (linuxdoom-1.10)
# ------------------------------------------------------------------------------
DOOM_DIR      := $(APPS_DIR)/doom
DOOM_CORE_DIR := $(DOOM_DIR)/linuxdoom
DOOM_BUILD    := $(APPS_BUILD)/doom

DOOM_SRCS := \
	$(DOOM_CORE_DIR)/am_map.c \
	$(DOOM_CORE_DIR)/doomdef.c \
	$(DOOM_CORE_DIR)/doomstat.c \
	$(DOOM_CORE_DIR)/dstrings.c \
	$(DOOM_CORE_DIR)/d_items.c \
	$(DOOM_CORE_DIR)/d_main.c \
	$(DOOM_CORE_DIR)/d_net.c \
	$(DOOM_CORE_DIR)/f_finale.c \
	$(DOOM_CORE_DIR)/f_wipe.c \
	$(DOOM_CORE_DIR)/g_game.c \
	$(DOOM_CORE_DIR)/hu_lib.c \
	$(DOOM_CORE_DIR)/hu_stuff.c \
	$(DOOM_CORE_DIR)/info.c \
	$(DOOM_CORE_DIR)/m_argv.c \
	$(DOOM_CORE_DIR)/m_bbox.c \
	$(DOOM_CORE_DIR)/m_cheat.c \
	$(DOOM_CORE_DIR)/m_fixed.c \
	$(DOOM_CORE_DIR)/m_menu.c \
	$(DOOM_CORE_DIR)/m_misc.c \
	$(DOOM_CORE_DIR)/m_random.c \
	$(DOOM_CORE_DIR)/m_swap.c \
	$(DOOM_CORE_DIR)/p_ceilng.c \
	$(DOOM_CORE_DIR)/p_doors.c \
	$(DOOM_CORE_DIR)/p_enemy.c \
	$(DOOM_CORE_DIR)/p_floor.c \
	$(DOOM_CORE_DIR)/p_inter.c \
	$(DOOM_CORE_DIR)/p_lights.c \
	$(DOOM_CORE_DIR)/p_map.c \
	$(DOOM_CORE_DIR)/p_maputl.c \
	$(DOOM_CORE_DIR)/p_mobj.c \
	$(DOOM_CORE_DIR)/p_plats.c \
	$(DOOM_CORE_DIR)/p_pspr.c \
	$(DOOM_CORE_DIR)/p_saveg.c \
	$(DOOM_CORE_DIR)/p_setup.c \
	$(DOOM_CORE_DIR)/p_sight.c \
	$(DOOM_CORE_DIR)/p_spec.c \
	$(DOOM_CORE_DIR)/p_switch.c \
	$(DOOM_CORE_DIR)/p_telept.c \
	$(DOOM_CORE_DIR)/p_tick.c \
	$(DOOM_CORE_DIR)/p_user.c \
	$(DOOM_CORE_DIR)/r_bsp.c \
	$(DOOM_CORE_DIR)/r_data.c \
	$(DOOM_CORE_DIR)/r_draw.c \
	$(DOOM_CORE_DIR)/r_main.c \
	$(DOOM_CORE_DIR)/r_plane.c \
	$(DOOM_CORE_DIR)/r_segs.c \
	$(DOOM_CORE_DIR)/r_sky.c \
	$(DOOM_CORE_DIR)/r_things.c \
	$(DOOM_CORE_DIR)/sounds.c \
	$(DOOM_CORE_DIR)/s_sound.c \
	$(DOOM_CORE_DIR)/st_lib.c \
	$(DOOM_CORE_DIR)/st_stuff.c \
	$(DOOM_CORE_DIR)/tables.c \
	$(DOOM_CORE_DIR)/v_video.c \
	$(DOOM_CORE_DIR)/wi_stuff.c \
	$(DOOM_CORE_DIR)/w_wad.c \
	$(DOOM_CORE_DIR)/z_zone.c \
	$(DOOM_DIR)/i_video.c \
	$(DOOM_DIR)/i_system.c \
	$(DOOM_DIR)/i_sound.c \
	$(DOOM_DIR)/i_net.c \
	$(DOOM_DIR)/i_main.c \
	$(DOOM_DIR)/ipo_libc_compat.c

DOOM_OBJS := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.o, $(DOOM_SRCS))

DOOM_CFLAGS := -m32 -O2 \
	-std=gnu89 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DNORMALUNIX \
	-I$(INCLUDE_DIR) \
	-I$(APPS_DIR)/include \
	-I$(DOOM_DIR)/include \
	-I$(DOOM_CORE_DIR) \
	-I$(DOOM_DIR) \
	-include $(DOOM_DIR)/doom_compat.h \
	-Wno-unused-variable \
	-Wno-unused-function \
	-Wno-implicit-function-declaration \
	-Wno-implicit-int

$(APPS_BUILD)/doom/%.o: $(APPS_DIR)/doom/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(DOOM_CFLAGS)

$(APPS_BUILD)/doom/doom.bin: $(DOOM_OBJS) $(APP_ENTRY_OBJ) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building DOOM: $(DOOM_DIR) → $@"
	@$(CC) $(DOOM_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APP_ENTRY_OBJ) $(DOOM_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# ------------------------------------------------------------------------------
# Dynamic Binary Format Manager (binfmt)
# ------------------------------------------------------------------------------
$(APPS_BUILD)/binfmt/binfmt_entry.o: $(APPS_DIR)/binfmt/binfmt_entry.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pic -fno-pie -c $< -o $@

$(APPS_BUILD)/binfmt/binfmt.o: $(APPS_DIR)/binfmt/binfmt.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 -g $(APPS_CFLAGS)

$(APPS_BUILD)/binfmt/binfmt.bin: $(APPS_BUILD)/binfmt/binfmt.o $(APPS_BUILD)/binfmt/binfmt_entry.o $(APPS_DIR)/binfmt/binfmt.ld $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building binfmt: $< → $@"
	@$(CC) $(APPS_CFLAGS) -pie -Wl,--no-dynamic-linker -Wl,-z,notext -Wl,-T,$(APPS_DIR)/binfmt/binfmt.ld \
		$(APPS_BUILD)/binfmt/binfmt_entry.o $< \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation\|RWX\|build-id" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .rel.dyn -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# ------------------------------------------------------------------------------
# Position-Independent Bare-Metal ELF Runner (elf_loader)
# ------------------------------------------------------------------------------
$(APPS_BUILD)/elf_loader/loader_entry.o: $(APPS_DIR)/elf_loader/loader_entry.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@ -fno-pic -fno-pie

$(APPS_BUILD)/elf_loader/elf_loader.o: $(APPS_DIR)/elf_loader/elf_loader.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -O2 -fno-pic -fno-pie -fno-jump-tables -fno-stack-protector -ffreestanding -fno-builtin -nostdlib -nostdinc -I$(INCLUDE_DIR) -c $< -o $@

$(APPS_BUILD)/elf_loader/elf_loader.bin: $(APPS_BUILD)/elf_loader/loader_entry.o $(APPS_BUILD)/elf_loader/elf_loader.o $(APPS_DIR)/elf_loader/loader.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building elf_loader: $< → $@"
	@$(LD) -m elf_i386 -T $(APPS_DIR)/elf_loader/loader.ld $(APPS_BUILD)/elf_loader/loader_entry.o $(APPS_BUILD)/elf_loader/elf_loader.o $(LIBGCC_A) -o $@.elf
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# ------------------------------------------------------------------------------
# Generic Userland Applications (ELF32)
# ------------------------------------------------------------------------------
$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 -g $(APPS_CFLAGS)

$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(APP_ENTRY_OBJ) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building: $*.c → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(APPS_DIR)/app.ld -Wl,-q \
		$(APP_ENTRY_OBJ) $< \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean-applications:
	@echo "[applications] Cleaning built applications..."
	@rm -rf $(APPS_BUILD)

clean-apps: clean-applications

