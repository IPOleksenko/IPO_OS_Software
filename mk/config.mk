# ==============================================================================
# IPO_Applications - Build Configuration & Tool Definitions
# ==============================================================================

# Tools
CC      := gcc
CXX     := g++
ASM     := nasm
LD      := ld
AR      := ar
OBJCOPY := objcopy
PYTHON  := python3

# Directories
PROJECT_ROOT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
APPS_DIR      := $(PROJECT_ROOT)/applications
LIB_DIR       := $(PROJECT_ROOT)/lib
INCLUDE_DIR   := $(PROJECT_ROOT)/include
TOOLS_DIR     := $(PROJECT_ROOT)/tools
ASSETS_DIR    := $(PROJECT_ROOT)/assets
BUILD         := $(PROJECT_ROOT)/build
LIB_BUILD_DIR := $(BUILD)/lib
APPS_BUILD    := $(BUILD)/applications
TOOLCHAIN_DIR := $(BUILD)/toolchain

# Output Libraries
USER_LIBC_A   := $(LIB_BUILD_DIR)/libc.a
USER_LIBM_A   := $(LIB_BUILD_DIR)/libm.a
USER_LIBCXX_A := $(LIB_BUILD_DIR)/libstdc++.a
APP_LIB_A     := $(LIB_BUILD_DIR)/libapp.a
APP_API_LIB_A := $(LIB_BUILD_DIR)/libapp_api.a

USER_CRT0     := $(LIB_BUILD_DIR)/crt0.o
USER_CRTI     := $(LIB_BUILD_DIR)/crti.o
USER_CRTN     := $(LIB_BUILD_DIR)/crtn.o
LIBTCC1_A     := $(LIB_BUILD_DIR)/libtcc1.a
CRT1_O        := $(LIB_BUILD_DIR)/crt1.o
LIBGCC_A      := $(LIB_BUILD_DIR)/libgcc.a

# Compiler Flags
APPS_CFLAGS := -m32 -O2 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-I$(PROJECT_ROOT) \
	-I$(INCLUDE_DIR) \
	-I$(APPS_DIR)/include \
	-I$(APPS_DIR)/micropython/lib/uzlib \
	-I$(APPS_DIR)

TCC_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DONE_SOURCE=1 \
	-I$(APPS_DIR)/tcc/include \
	-I$(APPS_DIR)/tcc \
	-I$(INCLUDE_DIR) \
	-I$(APPS_DIR)/include

# Target Disk Configuration
# No preset disk paths: user must explicitly specify DISK=/path/to/disk.img
DISK ?= $(or $(IPO_DISK),$(shell printenv DISK 2>/dev/null))
START_LBA ?= 2048
