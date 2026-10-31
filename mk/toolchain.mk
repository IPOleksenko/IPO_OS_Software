# ==============================================================================
# IPO_Applications - In-OS Toolchain Binaries (Binutils, GCC, Python, Lua, NASM)
# ==============================================================================

.PHONY: toolchain clean-toolchain

TOOLCHAIN_BINUTILS_OBJS := \
	$(TOOLCHAIN_DIR)/gcc.o \
	$(TOOLCHAIN_DIR)/ld.o \
	$(TOOLCHAIN_DIR)/as.o \
	$(TOOLCHAIN_DIR)/ar.o \
	$(TOOLCHAIN_DIR)/objcopy.o \
	$(TOOLCHAIN_DIR)/objdump.o \
	$(TOOLCHAIN_DIR)/readelf.o \
	$(TOOLCHAIN_DIR)/nm.o

toolchain: lib \
	$(TOOLCHAIN_DIR)/gcc.elf \
	$(TOOLCHAIN_DIR)/g++.elf \
	$(TOOLCHAIN_DIR)/ld.elf \
	$(TOOLCHAIN_DIR)/as.elf \
	$(TOOLCHAIN_DIR)/ar.elf \
	$(TOOLCHAIN_DIR)/ranlib.elf \
	$(TOOLCHAIN_DIR)/objcopy.elf \
	$(TOOLCHAIN_DIR)/strip.elf \
	$(TOOLCHAIN_DIR)/objdump.elf \
	$(TOOLCHAIN_DIR)/readelf.elf \
	$(TOOLCHAIN_DIR)/nm.elf \
	$(TOOLCHAIN_DIR)/lua/lua.elf \
	$(TOOLCHAIN_DIR)/lua/luac.elf \
	$(TOOLCHAIN_DIR)/nasm.elf \
	$(TOOLCHAIN_DIR)/python.elf

$(TOOLCHAIN_DIR)/%.o: $(TOOLS_DIR)/binutils/%.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -nostdinc -I$(INCLUDE_DIR) -fno-pic -fno-pie -c $< -o $@

$(TOOLCHAIN_DIR)/gcc.elf: $(TOOLCHAIN_DIR)/gcc.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/g++.elf: $(TOOLCHAIN_DIR)/gcc.elf
	@mkdir -p $(dir $@)
	cp $< $@

$(TOOLCHAIN_DIR)/ld.elf: $(TOOLCHAIN_DIR)/ld.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/as.elf: $(TOOLCHAIN_DIR)/as.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/ar.elf: $(TOOLCHAIN_DIR)/ar.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/ranlib.elf: $(TOOLCHAIN_DIR)/ar.elf
	@mkdir -p $(dir $@)
	cp $< $@

$(TOOLCHAIN_DIR)/objcopy.elf: $(TOOLCHAIN_DIR)/objcopy.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/strip.elf: $(TOOLCHAIN_DIR)/objcopy.elf
	@mkdir -p $(dir $@)
	cp $< $@

$(TOOLCHAIN_DIR)/objdump.elf: $(TOOLCHAIN_DIR)/objdump.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/readelf.elf: $(TOOLCHAIN_DIR)/readelf.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

$(TOOLCHAIN_DIR)/nm.elf: $(TOOLCHAIN_DIR)/nm.o $(USER_CRT0) $(USER_CRTI) $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $< $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

# ------------------------------------------------------------------------------
# Lua 5.3 runtime
# ------------------------------------------------------------------------------
$(TOOLCHAIN_DIR)/lua/lua.elf $(TOOLCHAIN_DIR)/lua/luac.elf: $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(TOOLCHAIN_DIR)/lua
	cd $(APPS_DIR)/lua/src && $(MAKE) clean && $(MAKE) a CC="$(CC) -m32 -nostdinc -I$(INCLUDE_DIR)" AR="$(AR) rcs" RANLIB="ranlib" MYCFLAGS="-DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin -fno-pic -fno-pie"
	$(CC) -m32 -nostdinc -I$(INCLUDE_DIR) -c $(APPS_DIR)/lua/src/lua.c -o $(APPS_DIR)/lua/src/lua.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin -fno-pic -fno-pie
	$(CC) -m32 -nostdinc -I$(INCLUDE_DIR) -c $(APPS_DIR)/lua/src/luac.c -o $(APPS_DIR)/lua/src/luac.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin -fno-pic -fno-pie
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $(APPS_DIR)/lua/src/lua.o $(APPS_DIR)/lua/src/liblua.a $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $(TOOLCHAIN_DIR)/lua/lua.elf
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $(APPS_DIR)/lua/src/luac.o $(APPS_DIR)/lua/src/liblua.a $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $(TOOLCHAIN_DIR)/lua/luac.elf

# ------------------------------------------------------------------------------
# NASM assembler
# ------------------------------------------------------------------------------
$(TOOLCHAIN_DIR)/nasm.elf: $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(TOOLCHAIN_DIR)
	@mkdir -p $(APPS_DIR)/nasm/doc && touch $(APPS_DIR)/nasm/doc/Makefile.in
	$(MAKE) -C $(APPS_DIR)/nasm asm/nasm.o
	cd $(APPS_DIR)/nasm && $(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) asm/nasm.o libnasm.a $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $(TOOLCHAIN_DIR)/nasm.elf

# ------------------------------------------------------------------------------
# MicroPython runtime
# ------------------------------------------------------------------------------
$(TOOLCHAIN_DIR)/python.elf: $(APPS_DIR)/micropython/micropython_shim.c $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) $(LIB_DIR)/linker.ld
	@mkdir -p $(TOOLCHAIN_DIR)
	$(MAKE) -C $(APPS_DIR)/micropython/ports/unix VARIANT=minimal CFLAGS_EXTRA="-m32 -fno-pic -fno-pie" LDFLAGS_EXTRA="-m32"
	$(CC) -m32 -c $(APPS_DIR)/micropython/micropython_shim.c -o $(TOOLCHAIN_DIR)/micropython_shim.o -I$(APPS_DIR)/micropython -I$(APPS_DIR)/micropython/ports/unix -I$(APPS_DIR)/micropython/ports/unix/variants/minimal -I$(APPS_DIR)/micropython/ports/unix/build-minimal -I$(APPS_DIR)/micropython/ports/unix/build-minimal/genhdr -fno-pic -fno-pie
	$(LD) -m elf_i386 -T $(LIB_DIR)/linker.ld $(USER_CRT0) $(USER_CRTI) $$(find $(APPS_DIR)/micropython/ports/unix/build-minimal -name '*.o' ! -name 'printf.o' ! -name 'abort_.o' ! -name 'vfs_blockdev.o') $(TOOLCHAIN_DIR)/micropython_shim.o $(USER_LIBC_A) $(USER_LIBM_A) $(LIBGCC_A) $(USER_CRTN) -o $@

clean-toolchain:
	rm -rf $(TOOLCHAIN_DIR)
