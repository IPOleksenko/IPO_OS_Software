# ==============================================================================
# IPO_OS_Software - Loadable Kernel Drivers (.kmd)
# ==============================================================================

DRIVERS_DIR       := $(PROJECT_ROOT)/drivers
DRIVERS_BUILD_DIR := $(BUILD)/drivers

KMD_CFLAGS := -m32 -O2 -fno-strict-aliasing -fno-omit-frame-pointer \
	-ffreestanding -fno-pic -fno-pie -fno-builtin -nostdlib -nostartfiles \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-mno-sse -mno-sse2 -mno-mmx \
	-Wall -Wextra

KMD_LD_FLAGS := -m elf_i386 -T $(DRIVERS_DIR)/kmd.ld --unresolved-symbols=ignore-all --emit-relocs --no-warn-rwx-segments

KSYMS_FILE := $(wildcard $(PROJECT_ROOT)/../IPO_OS/build/kernel/kernel_syms_autogen.c)
KMD_KSYMS_ARG := $(if $(KSYMS_FILE),--ksyms $(KSYMS_FILE),)

KMD_CRT_O := $(DRIVERS_BUILD_DIR)/kmd_crt.o
KMD_STUB_S := $(DRIVERS_BUILD_DIR)/kmd_stub_relocs.s
KMD_STUB_O := $(DRIVERS_BUILD_DIR)/kmd_stub_relocs.o
DRV_APP_ENTRY_O := $(DRIVERS_BUILD_DIR)/driver_app_entry.o
DRV_APP_STUB_O  := $(DRIVERS_BUILD_DIR)/driver_app_stub.o

DRIVERS_TARGETS := \
	$(DRIVERS_BUILD_DIR)/touchpad \
	$(DRIVERS_BUILD_DIR)/gamepad \
	$(DRIVERS_BUILD_DIR)/rtl8139 \
	$(DRIVERS_BUILD_DIR)/custom_proto

NET_DRV_SRCS := $(wildcard $(DRIVERS_DIR)/net/src/*.c)
NET_DRV_OBJS := $(patsubst $(DRIVERS_DIR)/net/src/%.c,$(DRIVERS_BUILD_DIR)/net/%.o,$(NET_DRV_SRCS))
NET_KMD_CFLAGS := $(KMD_CFLAGS) -I$(DRIVERS_DIR)/net/include -I$(PROJECT_ROOT)/include

.PHONY: drivers clean-drivers

drivers: $(DRIVERS_TARGETS)

$(DRV_APP_ENTRY_O): $(DRIVERS_DIR)/driver_app_entry.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -fno-pic -fno-pie -c $< -o $@

$(DRV_APP_STUB_O): $(DRIVERS_DIR)/driver_app_stub.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -O2 -fno-pic -fno-pie -fno-stack-protector -ffreestanding -fno-builtin -nostdlib -nostdinc -I$(PROJECT_ROOT)/include -c $< -o $@

$(KMD_CRT_O): $(DRIVERS_DIR)/kmd_crt.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(KMD_STUB_S):
	@mkdir -p $(dir $@)
	@printf '.section .kmd_self_relocs, "a", @progbits\n.global _kmd_reloc_tbl\n.global _kmd_ext_tbl\n.global _kmd_meta_tbl\n_kmd_reloc_tbl:\n    .long 0\n_kmd_ext_tbl:\n    .long 0\n_kmd_meta_tbl:\n    .long 0, 0, 0, -1\n' > $@

$(KMD_STUB_O): $(KMD_STUB_S)
	@mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $@

$(DRIVERS_BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c $(DRIVERS_DIR)/kmd_api.h $(DRIVERS_DIR)/virtio_input.h
	@mkdir -p $(dir $@)
	$(CC) $(KMD_CFLAGS) -c $< -o $@

define KMD_RULE
$(DRIVERS_BUILD_DIR)/$(1).pass1.elf: $(KMD_CRT_O) $(DRIVERS_BUILD_DIR)/$(1).o $(KMD_STUB_O) $(DRIVERS_DIR)/kmd.ld
	@mkdir -p $$(dir $$@)
	$(LD) $(KMD_LD_FLAGS) $(KMD_CRT_O) $(DRIVERS_BUILD_DIR)/$(1).o $(KMD_STUB_O) -o $$@

$(DRIVERS_BUILD_DIR)/$(1).relocs.s: $(DRIVERS_BUILD_DIR)/$(1).pass1.elf $(TOOLS_DIR)/build_kmd.py
	$(PYTHON) $(TOOLS_DIR)/build_kmd.py $$< --emit-self-relocs-s $$@ --name $(1) --author "IPO_OS" --description $(2) $(KMD_KSYMS_ARG)

$(DRIVERS_BUILD_DIR)/$(1).relocs.o: $(DRIVERS_BUILD_DIR)/$(1).relocs.s
	$(CC) -m32 -c $$< -o $$@

$(DRIVERS_BUILD_DIR)/$(1).pass2.elf: $(KMD_CRT_O) $(DRIVERS_BUILD_DIR)/$(1).o $(DRIVERS_BUILD_DIR)/$(1).relocs.o $(DRIVERS_DIR)/kmd.ld
	$(LD) $(KMD_LD_FLAGS) $(KMD_CRT_O) $(DRIVERS_BUILD_DIR)/$(1).o $(DRIVERS_BUILD_DIR)/$(1).relocs.o -o $$@

$(DRIVERS_BUILD_DIR)/$(1).kmd: $(DRIVERS_BUILD_DIR)/$(1).pass2.elf $(TOOLS_DIR)/build_kmd.py
	$(PYTHON) $(TOOLS_DIR)/build_kmd.py $$< -o $$@ $(KMD_KSYMS_ARG)

$(DRIVERS_BUILD_DIR)/$(1).payload.S: $(DRIVERS_BUILD_DIR)/$(1).kmd
	@printf '.section .rodata\n.global kmd_driver_name\n.global kmd_payload_start\n.global kmd_payload_end\nkmd_driver_name:\n    .asciz "$(1)"\n.balign 4\nkmd_payload_start:\n    .incbin "$(1).kmd"\nkmd_payload_end:\n' > $$@

$(DRIVERS_BUILD_DIR)/$(1).payload.o: $(DRIVERS_BUILD_DIR)/$(1).payload.S $(DRIVERS_BUILD_DIR)/$(1).kmd
	$(CC) -m32 -fno-pic -fno-pie -I$(DRIVERS_BUILD_DIR) -c $$< -o $$@

$(DRIVERS_BUILD_DIR)/$(1): $(DRV_APP_ENTRY_O) $(DRV_APP_STUB_O) $(DRIVERS_BUILD_DIR)/$(1).payload.o $(DRIVERS_DIR)/driver_app.ld
	$(LD) -m elf_i386 -T $(DRIVERS_DIR)/driver_app.ld --no-warn-rwx-segments $(DRV_APP_ENTRY_O) $(DRV_APP_STUB_O) $(DRIVERS_BUILD_DIR)/$(1).payload.o -o $$@.elf
	$(OBJCOPY) --set-section-flags .bss=alloc,load,contents -j .text -j .rodata -j .data -j .bss -O binary $$@.elf $$@
	@rm -f $$@.elf
	@SIZE=$$(stat -c%s "$$@" 2>/dev/null || stat -f%z "$$@" 2>/dev/null); \
	echo "[drivers] ✓ Created executable driver app: $$@ ($$SIZE bytes)"
endef

$(eval $(call KMD_RULE,touchpad,"IPO_OS Multi-Touch Touchpad Driver"))
$(eval $(call KMD_RULE,gamepad,"IPO_OS Gamepad Controller Driver"))
$(eval $(call KMD_RULE,custom_proto,"IPO-XMesh Custom Experimental Protocol Extension"))

$(DRIVERS_BUILD_DIR)/net/%.o: $(DRIVERS_DIR)/net/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(NET_KMD_CFLAGS) -c $< -o $@

$(DRIVERS_BUILD_DIR)/rtl8139.pass1.elf: $(KMD_CRT_O) $(NET_DRV_OBJS) $(KMD_STUB_O) $(DRIVERS_DIR)/kmd.ld
	@mkdir -p $(dir $@)
	$(LD) $(KMD_LD_FLAGS) $(KMD_CRT_O) $(NET_DRV_OBJS) $(KMD_STUB_O) -o $@

$(DRIVERS_BUILD_DIR)/rtl8139.relocs.s: $(DRIVERS_BUILD_DIR)/rtl8139.pass1.elf $(TOOLS_DIR)/build_kmd.py
	$(PYTHON) $(TOOLS_DIR)/build_kmd.py $< --emit-self-relocs-s $@ --name rtl8139 --author "IPO_OS_Software" --description "Realtek RTL8139 PCI NIC + Modular Network Stack" $(KMD_KSYMS_ARG)

$(DRIVERS_BUILD_DIR)/rtl8139.relocs.o: $(DRIVERS_BUILD_DIR)/rtl8139.relocs.s
	$(CC) -m32 -c $< -o $@

$(DRIVERS_BUILD_DIR)/rtl8139.pass2.elf: $(KMD_CRT_O) $(NET_DRV_OBJS) $(DRIVERS_BUILD_DIR)/rtl8139.relocs.o $(DRIVERS_DIR)/kmd.ld
	@mkdir -p $(dir $@)
	$(LD) $(KMD_LD_FLAGS) $(KMD_CRT_O) $(NET_DRV_OBJS) $(DRIVERS_BUILD_DIR)/rtl8139.relocs.o -o $@

$(DRIVERS_BUILD_DIR)/rtl8139.kmd: $(DRIVERS_BUILD_DIR)/rtl8139.pass2.elf $(TOOLS_DIR)/build_kmd.py
	$(PYTHON) $(TOOLS_DIR)/build_kmd.py $< -o $@ $(KMD_KSYMS_ARG)

$(DRIVERS_BUILD_DIR)/rtl8139.payload.S: $(DRIVERS_BUILD_DIR)/rtl8139.kmd
	@printf '.section .rodata\n.global kmd_driver_name\n.global kmd_payload_start\n.global kmd_payload_end\nkmd_driver_name:\n    .asciz "rtl8139"\n.balign 4\nkmd_payload_start:\n    .incbin "rtl8139.kmd"\nkmd_payload_end:\n' > $@

$(DRIVERS_BUILD_DIR)/rtl8139.payload.o: $(DRIVERS_BUILD_DIR)/rtl8139.payload.S $(DRIVERS_BUILD_DIR)/rtl8139.kmd
	$(CC) -m32 -fno-pic -fno-pie -I$(DRIVERS_BUILD_DIR) -c $< -o $@

$(DRIVERS_BUILD_DIR)/rtl8139: $(DRV_APP_ENTRY_O) $(DRV_APP_STUB_O) $(DRIVERS_BUILD_DIR)/rtl8139.payload.o $(DRIVERS_DIR)/driver_app.ld
	$(LD) -m elf_i386 -T $(DRIVERS_DIR)/driver_app.ld --no-warn-rwx-segments $(DRV_APP_ENTRY_O) $(DRV_APP_STUB_O) $(DRIVERS_BUILD_DIR)/rtl8139.payload.o -o $@.elf
	$(OBJCOPY) --set-section-flags .bss=alloc,load,contents -j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[drivers] ✓ Created executable driver app: $@ ($$SIZE bytes)"

clean-drivers:
	@rm -rf $(DRIVERS_BUILD_DIR)
