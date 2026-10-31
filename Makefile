# ==============================================================================
# IPO_Applications - Master Makefile
# ==============================================================================

.DEFAULT_GOAL := all

include mk/config.mk
include mk/lib.mk
include mk/applications.mk
include mk/toolchain.mk
include mk/disk.mk
include mk/clean.mk

all: lib applications toolchain font-databases

.PHONY: all lib applications apps toolchain font-databases load-apps make-apps install-disk format-disk create-disk clean clean-all

