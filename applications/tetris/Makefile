ROOT := ../..
include $(ROOT)/mk/config.mk

TETRIS_PORT_DIR := .
TETRIS_UPSTREAM := $(THIRD_PARTY_DIR)/Tetris-game
TETRIS_SRC_DIR  := $(TETRIS_UPSTREAM)/src
TETRIS_BUILD    := $(APPS_BUILD)/tetris
TETRIS_BIN      := $(TETRIS_BUILD)/tetris.bin
SENSE_PORT      := $(APPS_DIR)/SENSE-The-Game

TETRIS_CPP_SRCS := $(shell find $(TETRIS_SRC_DIR) -name '*.cc' | sort)
TETRIS_CPP_OBJS := $(patsubst $(TETRIS_SRC_DIR)/%.cc,$(TETRIS_BUILD)/upstream/%.o,$(TETRIS_CPP_SRCS))

TETRIS_EXTRA_OBJS := \
	$(TETRIS_BUILD)/ipo_tetris_sdl.o \
	$(TETRIS_BUILD)/ipo_tetris_fs.o \
	$(TETRIS_BUILD)/sense_ttf.o \
	$(TETRIS_BUILD)/wuffs/wuffs-v0.4.o \
	$(TETRIS_BUILD)/wuffs/image_wuffs.o

TETRIS_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++17 -O2 -MMD -MP -nostdinc++ -fexceptions -fno-rtti \
	-fno-threadsafe-statics -D_STDBOOL_H -D__bool_true_false_are_defined=1 -DNULL=0L \
	-include utility \
	-I$(TETRIS_PORT_DIR)/compat_stl \
	-I$(SENSE_PORT)/compat_stl \
	-I$(TETRIS_PORT_DIR)/compat_sdl \
	-I$(TETRIS_UPSTREAM) \
	-I$(TETRIS_SRC_DIR)/Screen \
	-I$(TETRIS_SRC_DIR)/Scenes \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-reorder -Wno-switch

TETRIS_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 \
	-I$(TETRIS_PORT_DIR)/compat_sdl \
	-I$(THIRD_PARTY_DIR)/northstar_media

.PHONY: tetris clean

tetris: $(TETRIS_BIN)

$(TETRIS_BUILD)/upstream/%.o: $(TETRIS_SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(TETRIS_CXXFLAGS)

$(TETRIS_BUILD)/tetris_cxx_rt.o: tetris_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(TETRIS_CXXFLAGS)

$(TETRIS_BUILD)/ipo_tetris_sdl.o: ipo_tetris_sdl.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(TETRIS_CFLAGS)

$(TETRIS_BUILD)/ipo_tetris_fs.o: ipo_tetris_fs.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -O2

$(TETRIS_BUILD)/sense_ttf.o: $(SENSE_PORT)/compat_sdl/sense_ttf.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(TETRIS_CFLAGS) \
		-DTTF_Init=sense_TTF_Init \
		-DTTF_Quit=sense_TTF_Quit \
		-DTTF_OpenFont=sense_TTF_OpenFont \
		-DTTF_CloseFont=sense_TTF_CloseFont

$(TETRIS_BUILD)/wuffs/wuffs-v0.4.o: $(THIRD_PARTY_DIR)/wuffs/wuffs-v0.4.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -DWUFFS_IMPLEMENTATION -Wno-unused-function -Wno-unused-parameter

$(TETRIS_BUILD)/wuffs/image_wuffs.o: $(THIRD_PARTY_DIR)/northstar_media/image_wuffs.c $(APPS_DIR)/links/port/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(APPS_DIR)/links/port/media -iquote $(THIRD_PARTY_DIR)/northstar_media -iquote $(THIRD_PARTY_DIR)/wuffs -Wno-unused-function

$(TETRIS_BIN): $(TETRIS_CPP_OBJS) $(TETRIS_EXTRA_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Tetris-game → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		-Wl,--wrap=fopen \
		$(USER_CRT0) $(TETRIS_CPP_OBJS) $(TETRIS_EXTRA_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(TETRIS_BUILD)

-include $(wildcard $(TETRIS_CPP_OBJS:.o=.d))
