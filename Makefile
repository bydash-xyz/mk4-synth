.PHONY: help build test test-hw sim render manual manual-pdf firmware flash profile capture dac-test clean

# Configuration
BUILD_DIR ?= build
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
PIO ?= pio
PIO_PY ?= $(HOME)/.local/pipx/venvs/platformio/bin/python
SECS ?= 40
FILTER ?= PROFILE

# Default target
.DEFAULT_GOAL := help

help: ## Print each target with a one-line description
	@echo "Usage: make [target] [VARS...]"
	@echo ""
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | \
		sed -E 's/^([^:]+):.*## /  \1\t/' | \
		column -t -s $$'\t'

build: ## Configure and build all desktop targets
	cmake -B $(BUILD_DIR) -S .
	cmake --build $(BUILD_DIR) -j $(JOBS)

test: build ## Run unit tests (skips tests labelled hardware)
	ctest --test-dir $(BUILD_DIR) -LE hardware --output-on-failure

test-hw: build ## Run hardware tests (requires MPK Mini MK4 attached to USB)
	ctest --test-dir $(BUILD_DIR) -L hardware --output-on-failure

sim: build ## Run interactive desktop simulator with optional ARGS
	./$(BUILD_DIR)/synth_sim $(ARGS)

render: build ## Render preset to WAV file; default renders preset 1 to build/render.wav
	./$(BUILD_DIR)/synth_render $(or $(ARGS),--preset 1 --out $(BUILD_DIR)/render.wav)

manual: ## Regenerate docs/mpk_mini_mk4_manual.html from Presets.h
	python3 tools/generate_manual.py

manual-pdf: ## Regenerate the manual and print it to build/mpk_mini_mk4_manual.pdf (US Letter, needs Chrome)
	python3 tools/generate_manual.py --pdf

firmware: ## Build ESP32-S3 Rev 2.0 firmware
	$(PIO) run -e mk4-synth

flash: ## Flash ESP32-S3 Rev 2.0 firmware via USB
	$(PIO) run -e mk4-synth -t upload

profile: ## Build and flash profiling firmware with boot benchmark and audio timing
	$(PIO) run -e mk4-synth-profile -t upload

capture: ## Capture the board's serial log for SECS seconds, keeping lines matching FILTER
	$(PIO_PY) tools/synthctl.py capture $(SECS) -f $(FILTER)

dac-test: ## Build and flash PCM5102A I2S DAC diagnostic test tone
	$(PIO) run -e diag_dac_test -t upload

clean: ## Remove build directory
	rm -rf $(BUILD_DIR)
