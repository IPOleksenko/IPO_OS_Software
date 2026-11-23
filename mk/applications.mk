# ==============================================================================
# IPO_OS_Software - Userland Applications
# ==============================================================================

.PHONY: applications clean-applications

APPS_SRCS_RAW    := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
GENERIC_APP_SRCS := $(filter-out $(APPS_DIR)/tcc/% $(APPS_DIR)/lua/% $(APPS_DIR)/micropython/% $(APPS_DIR)/nasm/% $(APPS_DIR)/doom/% $(APPS_DIR)/wolf3d/% $(APPS_DIR)/quake/% $(APPS_DIR)/links/% $(APPS_DIR)/git/% $(APPS_DIR)/pacman/% $(APPS_DIR)/donkey_kong/% $(APPS_DIR)/pokered/% $(APPS_DIR)/tetris/% $(APPS_DIR)/mario/% $(APPS_DIR)/zelda/% $(APPS_DIR)/invaders/% $(APPS_DIR)/spacewar/% $(APPS_DIR)/binfmt/% $(APPS_DIR)/elf_loader/% $(APPS_DIR)/SENSE-The-Game/%, $(APPS_SRCS_RAW))
SENSE_PORT_DIR   := $(APPS_DIR)/SENSE-The-Game
SENSE_UPSTREAM   := $(THIRD_PARTY_DIR)/SENSE-The-Game/app/src/main
SENSE_CPP_DIR    := $(SENSE_UPSTREAM)/cpp
SENSE_ASSETS_DIR := $(SENSE_UPSTREAM)/assets
SENSE_BUILD_DIR  := $(APPS_BUILD)/SENSE-The-Game
SENSE_BIN        := $(SENSE_BUILD_DIR)/SENSE: The Game.bin
SENSE_STAMP      := $(SENSE_BUILD_DIR)/SENSE-The-Game.stamp
SENSE_HAVE_UPSTREAM := $(and $(wildcard $(SENSE_CPP_DIR)/main.cpp),$(wildcard $(SENSE_ASSETS_DIR)/icon.bmp))

APPS_BINS        := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(GENERIC_APP_SRCS)) \
                    $(APPS_BUILD)/links/links.bin \
                    $(APPS_BUILD)/git/git.bin \
                    $(APPS_BUILD)/pacman/pacman.bin \
                    $(APPS_BUILD)/donkey_kong/donkey_kong.bin \
                    $(APPS_BUILD)/pokered/pokered.bin \
                    $(APPS_BUILD)/tetris/tetris.bin \
                    $(APPS_BUILD)/mario/mario.bin \
                    $(APPS_BUILD)/zelda/zelda.bin \
                    $(APPS_BUILD)/invaders/invaders.bin \
                    $(APPS_BUILD)/spacewar/spacewar.bin \
                    $(APPS_BUILD)/tcc/tcc.bin \
                    $(APPS_BUILD)/doom/doom.bin \
                    $(APPS_BUILD)/wolf3d/wolf3d.bin \
                    $(APPS_BUILD)/quake/quake.bin \
                    $(if $(SENSE_HAVE_UPSTREAM),$(SENSE_STAMP)) \
                    $(APPS_BUILD)/binfmt/binfmt.bin \
                    $(APPS_BUILD)/elf_loader/elf_loader.bin

APP_ENTRY_OBJ    := $(APPS_BUILD)/entry.o

applications: lib $(APPS_BINS)

.PHONY: links links-force git git-force pacman pacman-force donkey_kong donkey_kong-force pokered pokered-force tetris tetris-force mario mario-force zelda zelda-force invaders invaders-force spacewar spacewar-force wolf3d wolf3d-force quake quake-force
links: lib $(APPS_BUILD)/links/links.bin

links-force:

$(APPS_BUILD)/links/links.bin: links-force $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) $(BEARSSL_LIB_A)
	$(MAKE) -C $(APPS_DIR)/links links

git: lib $(APPS_BUILD)/git/git.bin

git-force:

$(APPS_BUILD)/git/git.bin: git-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) $(BEARSSL_LIB_A)
	$(MAKE) -C $(APPS_DIR)/git git

pacman: lib $(APPS_BUILD)/pacman/pacman.bin

pacman-force:

$(APPS_BUILD)/pacman/pacman.bin: pacman-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/pacman pacman

mario: lib $(APPS_BUILD)/mario/mario.bin

mario-force:

$(APPS_BUILD)/mario/mario.bin: mario-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/mario mario

zelda: lib $(APPS_BUILD)/zelda/zelda.bin

zelda-force:

$(APPS_BUILD)/zelda/zelda.bin: zelda-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/zelda zelda

invaders: lib $(APPS_BUILD)/invaders/invaders.bin

invaders-force:

$(APPS_BUILD)/invaders/invaders.bin: invaders-force $(USER_CRT0) $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/invaders invaders

spacewar: lib $(APPS_BUILD)/spacewar/spacewar.bin

spacewar-force:

$(APPS_BUILD)/spacewar/spacewar.bin: spacewar-force $(USER_CRT0) $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/spacewar spacewar

donkey_kong: lib $(APPS_BUILD)/donkey_kong/donkey_kong.bin

donkey_kong-force:

$(APPS_BUILD)/donkey_kong/donkey_kong.bin: donkey_kong-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/donkey_kong donkey_kong

pokered: lib $(APPS_BUILD)/pokered/pokered.bin

pokered-force:

$(APPS_BUILD)/pokered/pokered.bin: pokered-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/pokered pokered

tetris: lib $(APPS_BUILD)/tetris/tetris.bin

tetris-force:

$(APPS_BUILD)/tetris/tetris.bin: tetris-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/tetris tetris

wolf3d: lib $(APPS_BUILD)/wolf3d/wolf3d.bin

wolf3d-force:

$(APPS_BUILD)/wolf3d/wolf3d.bin: wolf3d-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/wolf3d wolf3d

quake: lib $(APPS_BUILD)/quake/quake.bin

quake-force:

$(APPS_BUILD)/quake/quake.bin: quake-force $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A)
	$(MAKE) -C $(APPS_DIR)/quake quake

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
# DOOM
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

DOOM_OBJS := $(patsubst $(DOOM_DIR)/%.c, $(APPS_BUILD)/doom/%.o, $(DOOM_SRCS))

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

$(APPS_BUILD)/doom/%.o: $(DOOM_DIR)/%.c
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
# SENSE: The Game External Port (C++17 + Compat SDL2/IMG/Mixer/TTF)
# Compiled only when upstream sources exist in $(SENSE_CPP_DIR).
# ------------------------------------------------------------------------------
.PHONY: SENSE-The-Game

ifneq ($(SENSE_HAVE_UPSTREAM),)

SENSE_CPP_SRCS := \
	$(SENSE_CPP_DIR)/main.cpp \
	$(SENSE_CPP_DIR)/application/audio.cpp \
	$(SENSE_CPP_DIR)/application/game.cpp \
	$(SENSE_CPP_DIR)/application/renderer.cpp \
	$(SENSE_CPP_DIR)/application/window.cpp \
	$(SENSE_CPP_DIR)/assets/assets.cpp \
	$(SENSE_CPP_DIR)/assets/checkpoints.cpp \
	$(SENSE_CPP_DIR)/objects/background.cpp \
	$(SENSE_CPP_DIR)/objects/decor.cpp \
	$(SENSE_CPP_DIR)/objects/end.cpp \
	$(SENSE_CPP_DIR)/objects/player.cpp \
	$(SENSE_CPP_DIR)/objects/road.cpp \
	$(SENSE_CPP_DIR)/objects/scale.cpp \
	$(SENSE_CPP_DIR)/objects/text.cpp \
	$(SENSE_CPP_DIR)/utils/icon.cpp \
	$(SENSE_CPP_DIR)/utils/modding.cpp \
	$(SENSE_CPP_DIR)/utils/music.cpp \
	$(SENSE_CPP_DIR)/utils/sfx.cpp \
	$(SENSE_CPP_DIR)/utils/text_manager.cpp \
	$(SENSE_CPP_DIR)/utils/texture.cpp

SENSE_CPP_OBJS := $(patsubst $(SENSE_CPP_DIR)/%.cpp,$(SENSE_BUILD_DIR)/upstream/%.o,$(SENSE_CPP_SRCS))
SENSE_SDL_OBJS := $(SENSE_BUILD_DIR)/compat_sdl/sense_sdl.o \
	$(SENSE_BUILD_DIR)/compat_sdl/sense_ttf.o \
	$(SENSE_BUILD_DIR)/compat_sdl/sense_mem.o \
	$(SENSE_BUILD_DIR)/wuffs/wuffs-v0.4.o \
	$(SENSE_BUILD_DIR)/wuffs/image_wuffs.o

SENSE_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++17 -nostdinc++ -fexceptions -fno-rtti \
	-fno-threadsafe-statics -MMD -MP -D_STDBOOL_H -D__bool_true_false_are_defined=1 \
	-I$(SENSE_PORT_DIR)/compat_stl \
	-I$(SENSE_PORT_DIR)/compat_sdl \
	-I$(SENSE_CPP_DIR) \
	-I$(SENSE_CPP_DIR)/application \
	-I$(SENSE_CPP_DIR)/assets \
	-I$(SENSE_CPP_DIR)/objects \
	-I$(SENSE_CPP_DIR)/utils

SENSE_SDL_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -MMD -MP \
	-I$(SENSE_PORT_DIR)/compat_sdl \
	-I$(THIRD_PARTY_DIR)/northstar_media

SENSE-The-Game: lib $(SENSE_STAMP)

-include $(SENSE_CPP_OBJS:.o=.d) $(SENSE_SDL_OBJS:.o=.d)

$(SENSE_BUILD_DIR)/upstream/assets/assets.o: $(SENSE_CPP_DIR)/assets/assets.cpp | check-submodules
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(SENSE_CXXFLAGS) -Wa,-I$(SENSE_ASSETS_DIR)

$(SENSE_BUILD_DIR)/upstream/%.o: $(SENSE_CPP_DIR)/%.cpp | check-submodules
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(SENSE_CXXFLAGS)

$(SENSE_BUILD_DIR)/sense_cxx_rt.o: $(SENSE_PORT_DIR)/sense_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(SENSE_CXXFLAGS)

$(SENSE_BUILD_DIR)/compat_sdl/%.o: $(SENSE_PORT_DIR)/compat_sdl/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(SENSE_SDL_CFLAGS)

$(SENSE_BUILD_DIR)/wuffs/wuffs-v0.4.o: $(THIRD_PARTY_DIR)/wuffs/wuffs-v0.4.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -DWUFFS_IMPLEMENTATION -Wno-unused-function -Wno-unused-parameter

$(SENSE_BUILD_DIR)/wuffs/image_wuffs.o: $(THIRD_PARTY_DIR)/northstar_media/image_wuffs.c $(APPS_DIR)/links/port/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(APPS_DIR)/links/port/media -iquote $(THIRD_PARTY_DIR)/northstar_media -iquote $(THIRD_PARTY_DIR)/wuffs -Wno-unused-function

$(SENSE_STAMP): $(SENSE_CPP_OBJS) $(SENSE_SDL_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building SENSE: The Game port: $(SENSE_BIN)"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(SENSE_CPP_OBJS) $(SENSE_SDL_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf "$(SENSE_BIN)"
	@rm -f $@.elf
	@touch $@
	@SIZE=$$(stat -c%s "$(SENSE_BIN)" 2>/dev/null || stat -f%z "$(SENSE_BIN)" 2>/dev/null); \
	echo "[applications] ✓ Created: $(SENSE_BIN) ($$SIZE bytes)"

else

SENSE-The-Game:
	@echo "[applications] Skipping SENSE: The Game: sources not found in $(SENSE_UPSTREAM)"

endif

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
GENERIC_APP_OBJS := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.o, $(GENERIC_APP_SRCS))
-include $(GENERIC_APP_OBJS:.o=.d)

$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 -g -MMD -MP $(APPS_CFLAGS)

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
