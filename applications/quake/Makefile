ROOT := ../..
include $(ROOT)/mk/config.mk

QUAKE_PORT_DIR := .
QUAKE_UPSTREAM := $(THIRD_PARTY_DIR)/Quake/WinQuake
QUAKE_BUILD    := $(APPS_BUILD)/quake
QUAKE_BIN      := $(QUAKE_BUILD)/quake.bin

QUAKE_UPSTREAM_SRCS := \
	$(QUAKE_UPSTREAM)/cl_demo.c \
	$(QUAKE_UPSTREAM)/cl_input.c \
	$(QUAKE_UPSTREAM)/cl_main.c \
	$(QUAKE_UPSTREAM)/cl_parse.c \
	$(QUAKE_UPSTREAM)/cl_tent.c \
	$(QUAKE_UPSTREAM)/chase.c \
	$(QUAKE_UPSTREAM)/cmd.c \
	$(QUAKE_UPSTREAM)/common.c \
	$(QUAKE_UPSTREAM)/console.c \
	$(QUAKE_UPSTREAM)/crc.c \
	$(QUAKE_UPSTREAM)/cvar.c \
	$(QUAKE_UPSTREAM)/draw.c \
	$(QUAKE_UPSTREAM)/d_edge.c \
	$(QUAKE_UPSTREAM)/d_fill.c \
	$(QUAKE_UPSTREAM)/d_init.c \
	$(QUAKE_UPSTREAM)/d_modech.c \
	$(QUAKE_UPSTREAM)/d_part.c \
	$(QUAKE_UPSTREAM)/d_polyse.c \
	$(QUAKE_UPSTREAM)/d_scan.c \
	$(QUAKE_UPSTREAM)/d_sky.c \
	$(QUAKE_UPSTREAM)/d_sprite.c \
	$(QUAKE_UPSTREAM)/d_surf.c \
	$(QUAKE_UPSTREAM)/d_vars.c \
	$(QUAKE_UPSTREAM)/d_zpoint.c \
	$(QUAKE_UPSTREAM)/host.c \
	$(QUAKE_UPSTREAM)/host_cmd.c \
	$(QUAKE_UPSTREAM)/keys.c \
	$(QUAKE_UPSTREAM)/menu.c \
	$(QUAKE_UPSTREAM)/mathlib.c \
	$(QUAKE_UPSTREAM)/model.c \
	$(QUAKE_UPSTREAM)/net_loop.c \
	$(QUAKE_UPSTREAM)/net_main.c \
	$(QUAKE_UPSTREAM)/net_none.c \
	$(QUAKE_UPSTREAM)/net_vcr.c \
	$(QUAKE_UPSTREAM)/nonintel.c \
	$(QUAKE_UPSTREAM)/pr_cmds.c \
	$(QUAKE_UPSTREAM)/pr_edict.c \
	$(QUAKE_UPSTREAM)/pr_exec.c \
	$(QUAKE_UPSTREAM)/r_aclip.c \
	$(QUAKE_UPSTREAM)/r_alias.c \
	$(QUAKE_UPSTREAM)/r_bsp.c \
	$(QUAKE_UPSTREAM)/r_light.c \
	$(QUAKE_UPSTREAM)/r_draw.c \
	$(QUAKE_UPSTREAM)/r_efrag.c \
	$(QUAKE_UPSTREAM)/r_edge.c \
	$(QUAKE_UPSTREAM)/r_misc.c \
	$(QUAKE_UPSTREAM)/r_main.c \
	$(QUAKE_UPSTREAM)/r_sky.c \
	$(QUAKE_UPSTREAM)/r_sprite.c \
	$(QUAKE_UPSTREAM)/r_surf.c \
	$(QUAKE_UPSTREAM)/r_part.c \
	$(QUAKE_UPSTREAM)/r_vars.c \
	$(QUAKE_UPSTREAM)/screen.c \
	$(QUAKE_UPSTREAM)/sbar.c \
	$(QUAKE_UPSTREAM)/sv_main.c \
	$(QUAKE_UPSTREAM)/sv_phys.c \
	$(QUAKE_UPSTREAM)/sv_move.c \
	$(QUAKE_UPSTREAM)/sv_user.c \
	$(QUAKE_UPSTREAM)/zone.c \
	$(QUAKE_UPSTREAM)/view.c \
	$(QUAKE_UPSTREAM)/wad.c \
	$(QUAKE_UPSTREAM)/world.c \
	$(QUAKE_UPSTREAM)/snd_dma.c \
	$(QUAKE_UPSTREAM)/snd_mem.c \
	$(QUAKE_UPSTREAM)/snd_mix.c \
	$(QUAKE_UPSTREAM)/cd_null.c

QUAKE_OBJS := $(patsubst $(QUAKE_UPSTREAM)/%.c,$(QUAKE_BUILD)/upstream/%.o,$(QUAKE_UPSTREAM_SRCS))

QUAKE_PORT_OBJS := \
	$(QUAKE_BUILD)/sys_ipo.o \
	$(QUAKE_BUILD)/vid_ipo.o \
	$(QUAKE_BUILD)/in_ipo.o \
	$(QUAKE_BUILD)/snd_ipo.o

QUAKE_CFLAGS := $(APPS_CFLAGS) -std=gnu99 -O2 -U__i386__ \
	-I$(QUAKE_UPSTREAM) \
	-I$(APPS_DIR) \
	-include $(QUAKE_PORT_DIR)/ipo_quake_compat.h \
	-Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare \
	-Wno-unused-but-set-variable -Wno-implicit-function-declaration \
	-Wno-pointer-sign -Wno-incompatible-pointer-types \
	-Wno-strict-aliasing -Wno-char-subscripts

.PHONY: quake clean

quake: $(QUAKE_BIN)

$(QUAKE_BUILD)/upstream/%.o: $(QUAKE_UPSTREAM)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(QUAKE_CFLAGS)

$(QUAKE_BUILD)/%.o: %.c ipo_quake_compat.h
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(QUAKE_CFLAGS)

$(QUAKE_BIN): $(QUAKE_OBJS) $(QUAKE_PORT_OBJS) $(USER_CRT0) $(APP_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	@echo "[applications] Building Quake (id-Software WinQuake) → $@"
	@$(CC) $(APPS_CFLAGS) -no-pie -Wl,-T,$(LIB_DIR)/linker.ld -Wl,-q \
		$(USER_CRT0) $(QUAKE_OBJS) $(QUAKE_PORT_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group $(USER_LIBC_A) $(USER_LIBM_A) $(APP_LIB_A) -lgcc -Wl,--end-group \
		-o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@test -f $@.elf
	@cp $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

clean:
	rm -rf $(QUAKE_BUILD)
