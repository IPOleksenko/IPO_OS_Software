ROOT := ../..
include $(ROOT)/mk/config.mk

WOLF_PORT_DIR := .
WOLF_UPSTREAM := $(THIRD_PARTY_DIR)/Chocolate-Wolfenstein-3D
WOLF_BUILD    := $(APPS_BUILD)/wolf3d
WOLF_BIN      := $(WOLF_BUILD)/wolf3d.bin

WOLF_CPP_SRCS := \
	$(WOLF_UPSTREAM)/fmopl.cpp \
	$(WOLF_UPSTREAM)/id_ca.cpp \
	$(WOLF_UPSTREAM)/id_in.cpp \
	$(WOLF_UPSTREAM)/id_pm.cpp \
	$(WOLF_UPSTREAM)/id_sd.cpp \
	$(WOLF_UPSTREAM)/id_us_1.cpp \
	$(WOLF_UPSTREAM)/id_vh.cpp \
	$(WOLF_UPSTREAM)/id_vl.cpp \
	$(WOLF_UPSTREAM)/signon.cpp \
	$(WOLF_UPSTREAM)/wl_act1.cpp \
	$(WOLF_UPSTREAM)/wl_act2.cpp \
	$(WOLF_UPSTREAM)/wl_agent.cpp \
	$(WOLF_UPSTREAM)/wl_debug.cpp \
	$(WOLF_UPSTREAM)/wl_draw.cpp \
	$(WOLF_UPSTREAM)/wl_game.cpp \
	$(WOLF_UPSTREAM)/wl_inter.cpp \
	$(WOLF_UPSTREAM)/wl_main.cpp \
	$(WOLF_UPSTREAM)/wl_menu.cpp \
	$(WOLF_UPSTREAM)/wl_play.cpp \
	$(WOLF_UPSTREAM)/wl_state.cpp \
	$(WOLF_UPSTREAM)/wl_text.cpp

WOLF_OBJS := $(patsubst $(WOLF_UPSTREAM)/%.cpp,$(WOLF_BUILD)/upstream/%.o,$(WOLF_CPP_SRCS))

WOLF_EXTRA_OBJS := \
	$(WOLF_BUILD)/ipo_wolf_crt.o \
	$(WOLF_BUILD)/ipo_wolf_sdl1.o \
	$(WOLF_BUILD)/ipo_wolf_fs.o

WOLF_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 -Wall -Wextra \
	-I$(WOLF_PORT_DIR)/compat_sdl1 \
	-I$(APPS_DIR) \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare

WOLF_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++11 -O2 -fwrapv -fpermissive \
	-fno-exceptions -fno-threadsafe-statics -ftrivial-auto-var-init=zero \
	-DNULL=0L \
	-I$(WOLF_PORT_DIR)/compat_sdl1 \
	-include $(WOLF_PORT_DIR)/compat_sdl1/wolf_c_headers.h \
	-I$(WOLF_UPSTREAM) \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare \
	-Wno-write-strings -Wno-unused-but-set-variable -Wno-narrowing \
	-Wno-conversion-null -Wno-address-of-packed-member -Wno-format

.PHONY: wolf3d clean

wolf3d: $(WOLF_BIN)

$(WOLF_BUILD)/upstream/%.o: $(WOLF_UPSTREAM)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(WOLF_CXXFLAGS)

$(WOLF_BUILD)/ipo_wolf_crt.o: ipo_wolf_crt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(WOLF_CXXFLAGS)

$(WOLF_BUILD)/wolf_cxx_rt.o: wolf_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(WOLF_CXXFLAGS)

$(WOLF_BUILD)/ipo_wolf_sdl1.o: ipo_wolf_sdl1.c compat_sdl1/SDL/SDL.h compat_sdl1/SDL/SDL_mixer.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(WOLF_CFLAGS)

$(WOLF_BUILD)/ipo_wolf_fs.o: ipo_wolf_fs.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(WOLF_CFLAGS)

$(WOLF_BIN): $(WOLF_OBJS) $(WOLF_EXTRA_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Wolfenstein 3D (Chocolate-Wolfenstein-3D) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		-Wl,--wrap=fopen -Wl,--wrap=open -Wl,--wrap=stat -Wl,--wrap=mkdir -Wl,--wrap=getenv \
		$(USER_CRT0) $(WOLF_OBJS) $(WOLF_EXTRA_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(WOLF_BUILD)
