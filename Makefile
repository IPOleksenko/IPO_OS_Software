# ==============================================================================
# IPO_OS_Software - Master Makefile
# ==============================================================================

.DEFAULT_GOAL := all

include mk/config.mk
include mk/lib.mk
include mk/drivers.mk
include mk/applications.mk
include mk/toolchain.mk
include mk/disks.mk
include mk/clean.mk

SENSE_SUBMODULE_DIR    := third_party/SENSE-The-Game
SENSE_SUBMODULE_MARKER := $(SENSE_SUBMODULE_DIR)/app/src/main/cpp/main.cpp
GIT_SUBMODULE_DIR      := third_party/git
GIT_SUBMODULE_MARKER   := $(GIT_SUBMODULE_DIR)/git.c

all: check-submodules lib drivers applications toolchain font-databases

check-submodules:
	@if [ ! -f "$(SENSE_SUBMODULE_MARKER)" ] || [ ! -f "$(GIT_SUBMODULE_MARKER)" ]; then \
		echo "================================================================="; \
		echo " NOTE: Submodule sources not found in third_party/;"; \
		echo " to initialize submodules, run:"; \
		echo "   git submodule update --init --recursive"; \
		echo "================================================================="; \
	fi

.PHONY: all check-submodules lib drivers applications toolchain font-databases load-apps format-disk create-disk live-image clean clean-all
