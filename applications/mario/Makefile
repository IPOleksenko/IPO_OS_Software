ROOT := ../..
include $(ROOT)/mk/config.mk

MARIO_PORT_DIR := .
UMARIO_DIR     := $(THIRD_PARTY_DIR)/uMario_Jakowski/uNext
MARIO_BUILD    := $(APPS_BUILD)/mario
MARIO_BIN      := $(MARIO_BUILD)/mario.bin
SENSE_PORT     := $(APPS_DIR)/SENSE-The-Game

UMARIO_SRCS := $(wildcard $(UMARIO_DIR)/*.cpp)
UMARIO_OBJS := $(patsubst $(UMARIO_DIR)/%.cpp,$(MARIO_BUILD)/uNext/%.o,$(UMARIO_SRCS))

MARIO_EXTRA_OBJS := \
	$(MARIO_BUILD)/ipo_sdl2.o

MARIO_CFLAGS := $(APPS_CFLAGS) -std=gnu11 -O2 -Wall -Wextra \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare

MARIO_CXXFLAGS := $(APPS_CFLAGS) -std=gnu++14 -O2 -fwrapv \
	-nostdinc++ -fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-D_STDBOOL_H -D__bool_true_false_are_defined=1 -DNULL=0L \
	-I$(MARIO_PORT_DIR)/compat_sdl2 \
	-I$(SENSE_PORT)/compat_stl \
	-I$(UMARIO_DIR) \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare -Wno-reorder \
	-Wno-switch -Wno-write-strings -Wno-unknown-pragmas

.PHONY: mario clean

mario: $(MARIO_BIN)

$(MARIO_BUILD)/uNext/%.o: $(UMARIO_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(MARIO_CXXFLAGS)

$(MARIO_BUILD)/ipo_sdl2.o: ipo_sdl2.c compat_sdl2/SDL.h compat_sdl2/SDL_mixer.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(MARIO_CFLAGS) -I$(MARIO_PORT_DIR)/compat_sdl2

$(MARIO_BUILD)/mario_cxx_rt.o: mario_cxx_rt.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(MARIO_CXXFLAGS)

$(MARIO_BIN): $(UMARIO_OBJS) $(MARIO_EXTRA_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Super Mario Bros (uMario_Jakowski) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(UMARIO_OBJS) $(MARIO_EXTRA_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(MARIO_BUILD)
