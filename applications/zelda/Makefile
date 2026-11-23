ROOT := ../..
include $(ROOT)/mk/config.mk

ZELDA_PORT_DIR := .
ZELDA_UPSTREAM := $(THIRD_PARTY_DIR)/Cpp-Zelda-2019/Claudius
ZELDA_BUILD    := $(APPS_BUILD)/zelda
ZELDA_BIN      := $(ZELDA_BUILD)/zelda.bin
SENSE_PORT     := $(APPS_DIR)/SENSE-The-Game

ZELDA_SRCS := $(wildcard $(ZELDA_UPSTREAM)/*.cpp)
ZELDA_OBJS := $(patsubst $(ZELDA_UPSTREAM)/%.cpp,$(ZELDA_BUILD)/upstream/%.o,$(ZELDA_SRCS))

ZELDA_EXTRA_OBJS := \
	$(ZELDA_BUILD)/ipo_sdl2.o \
	$(ZELDA_BUILD)/ipo_zelda_fs.o \
	$(ZELDA_BUILD)/wuffs/wuffs-v0.4.o \
	$(ZELDA_BUILD)/wuffs/image_wuffs.o

ZELDA_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 -Wall -Wextra \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare

ZELDA_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++17 -O2 -fwrapv -ftrivial-auto-var-init=zero \
	-nostdinc++ -fexceptions -frtti -fno-threadsafe-statics \
	-D_STDBOOL_H -D__bool_true_false_are_defined=1 -DNULL=0L \
	-D__int64="long long" \
	-include typeinfo -include utility -include stdexcept \
	-I$(ZELDA_PORT_DIR)/compat_sdl2 \
	-I$(ZELDA_PORT_DIR)/compat_stl \
	-I$(SENSE_PORT)/compat_stl \
	-I$(APPS_DIR)/donkey_kong/compat_stl \
	-I$(ZELDA_UPSTREAM) \
	-I$(ZELDA_PORT_DIR)/compat_case \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-reorder \
	-Wno-switch -Wno-write-strings -Wno-unknown-pragmas

.PHONY: zelda clean

zelda: $(ZELDA_BIN)

$(ZELDA_BUILD)/upstream/%.o: $(ZELDA_UPSTREAM)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(ZELDA_CXXFLAGS)

$(ZELDA_BUILD)/ipo_sdl2.o: ipo_sdl2.c compat_sdl2/SDL.h compat_sdl2/SDL_image.h compat_sdl2/SDL_mixer.h compat_sdl2/SDL_ttf.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(ZELDA_CFLAGS) -I$(ZELDA_PORT_DIR)/compat_sdl2

$(ZELDA_BUILD)/ipo_zelda_fs.o: ipo_zelda_fs.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(ZELDA_CFLAGS)

$(ZELDA_BUILD)/zelda_cxx_rt.o: zelda_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(ZELDA_CXXFLAGS)

$(ZELDA_BUILD)/wuffs/wuffs-v0.4.o: $(THIRD_PARTY_DIR)/wuffs/wuffs-v0.4.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -DWUFFS_IMPLEMENTATION -Wno-unused-function -Wno-unused-parameter

$(ZELDA_BUILD)/wuffs/image_wuffs.o: $(THIRD_PARTY_DIR)/northstar_media/image_wuffs.c $(APPS_DIR)/links/port/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(APPS_DIR)/links/port/media -iquote $(THIRD_PARTY_DIR)/northstar_media -iquote $(THIRD_PARTY_DIR)/wuffs -Wno-unused-function

$(ZELDA_BIN): $(ZELDA_OBJS) $(ZELDA_EXTRA_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building The Legend of Zelda (Cpp-Zelda-2019) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		-Wl,--wrap=fopen \
		$(USER_CRT0) $(ZELDA_OBJS) $(ZELDA_EXTRA_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(ZELDA_BUILD)
