# ==============================================================================
# IPO_Applications - Clean Rules
# ==============================================================================

.PHONY: clean clean-all clean-lib clean-applications clean-toolchain

clean: clean-lib clean-applications clean-toolchain
	@echo "[clean] Cleaning build artifacts..."
	@rm -rf $(BUILD)

clean-all: clean
	@echo "[clean-all] Cleaning all temporary files..."
	@find $(PROJECT_ROOT) -name "*.o" -type f -delete 2>/dev/null || true
	@find $(PROJECT_ROOT) -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true

