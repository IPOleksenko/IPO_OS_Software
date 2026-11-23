ROOT := ../..
include $(ROOT)/mk/config.mk

DK_PORT_DIR   := .
DK_UPSTREAM   := $(THIRD_PARTY_DIR)/donkey_kong_arcade
DK_SRC_DIR    := $(DK_UPSTREAM)/src
DK_BUILD      := $(APPS_BUILD)/donkey_kong
DK_BIN        := $(DK_BUILD)/donkey_kong.bin
SENSE_PORT    := $(APPS_DIR)/SENSE-The-Game

DK_CPP_SRCS := \
	$(DK_SRC_DIR)/core/main.cc \
	$(DK_SRC_DIR)/core/log.cc \
	$(DK_SRC_DIR)/core/input.cc \
	$(DK_SRC_DIR)/core/rand.cc \
	$(DK_SRC_DIR)/core/animation.cc \
	$(DK_SRC_DIR)/core/cutscene.cc \
	$(DK_SRC_DIR)/assets/sprites.cc \
	$(DK_SRC_DIR)/assets/cutscenes.cc \
	$(DK_SRC_DIR)/assets/fonts.cc \
	$(DK_SRC_DIR)/assets/sounds.cc \
	$(DK_SRC_DIR)/game/play.cc \
	$(DK_SRC_DIR)/game/game.cc \
	$(DK_SRC_DIR)/game/intermission.cc \
	$(DK_SRC_DIR)/game/world.cc \
	$(DK_SRC_DIR)/game/constants.cc \
	$(DK_SRC_DIR)/game/stages/classic_barrels.cc \
	$(DK_SRC_DIR)/game/stages/classic_factory.cc \
	$(DK_SRC_DIR)/game/stages/classic_elevators.cc \
	$(DK_SRC_DIR)/game/stages/classic_rivets.cc \
	$(DK_SRC_DIR)/hud/hud.cc \
	$(DK_SRC_DIR)/hiscores/hiscores.cc \
	$(DK_SRC_DIR)/menu/menu.cc \
	$(DK_SRC_DIR)/title/title.cc

DK_CPP_OBJS := $(patsubst $(DK_SRC_DIR)/%.cc,$(DK_BUILD)/upstream/%.o,$(DK_CPP_SRCS))

DK_EXTRA_OBJS := \
	$(DK_BUILD)/ipo_allegro.o \
	$(DK_BUILD)/wuffs/wuffs-v0.4.o \
	$(DK_BUILD)/wuffs/image_wuffs.o

DK_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++20 -O2 -nostdinc++ -fexceptions -fno-rtti \
	-fno-threadsafe-statics -D_STDBOOL_H -D__bool_true_false_are_defined=1 \
	-include cassert -include utility \
	-I$(DK_PORT_DIR)/compat_stl \
	-I$(SENSE_PORT)/compat_stl \
	-I$(DK_PORT_DIR)/compat_allegro5 \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-reorder

DK_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 \
	-I$(DK_PORT_DIR)/compat_allegro5 \
	-I$(THIRD_PARTY_DIR)/northstar_media

.PHONY: donkey_kong clean

donkey_kong: $(DK_BIN)

$(DK_BUILD)/upstream/%.o: $(DK_SRC_DIR)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(DK_CXXFLAGS)

$(DK_BUILD)/sense_cxx_rt.o: $(SENSE_PORT)/sense_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(DK_CXXFLAGS)

$(DK_BUILD)/ipo_allegro.o: ipo_allegro.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(DK_CFLAGS)

$(DK_BUILD)/wuffs/wuffs-v0.4.o: $(THIRD_PARTY_DIR)/wuffs/wuffs-v0.4.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -DWUFFS_IMPLEMENTATION -Wno-unused-function -Wno-unused-parameter

$(DK_BUILD)/wuffs/image_wuffs.o: $(THIRD_PARTY_DIR)/northstar_media/image_wuffs.c $(APPS_DIR)/links/port/media/glib.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS) -std=gnu11 -I$(APPS_DIR)/links/port/media -iquote $(THIRD_PARTY_DIR)/northstar_media -iquote $(THIRD_PARTY_DIR)/wuffs -Wno-unused-function

$(DK_BIN): $(DK_CPP_OBJS) $(DK_EXTRA_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building donkey_kong → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(DK_CPP_OBJS) $(DK_EXTRA_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(DK_BUILD)
