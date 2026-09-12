# Convenience wrapper around ESP-IDF's idf.py.
#
#   make setup      # one-time: create .env (then fill in WiFi + Vapi keys)
#   make run        # build + flash + monitor  (auto target, auto port)
#
# Requires ESP-IDF v5.5.1 or newer. Point IDF_PATH at your install, or export it
# once in your shell — the usual `. $HOME/esp/esp-idf/export.sh` also works, and
# this Makefile will use whatever that set up.
#
# Override the port if auto-detect picks the wrong device:
#   make PORT=/dev/cu.usbmodem101 run
#
# Run `make help` for the full list of targets.

SHELL := /bin/bash

# Where ESP-IDF lives. Uses $IDF_PATH from the environment if set; otherwise
# tries the two conventional install locations before giving up.
ifeq ($(origin IDF_PATH), undefined)
  IDF_PATH := $(firstword $(wildcard $(HOME)/esp/esp-idf $(HOME)/esp-idf))
endif
EXPORT := $(IDF_PATH)/export.sh

# Chip target
TARGET ?= esp32s3

# Serial port. The AtomS3R has no USB-serial bridge — it enumerates over the
# ESP32-S3's built-in USB-Serial-JTAG as /dev/cu.usbmodem* (macOS) or
# /dev/ttyACM* (Linux), so those come first. Beware: other USB devices also
# claim usbmodem names, so if auto-detect picks the wrong one, pass PORT=
# explicitly. `ls /dev/cu.usbmodem*` before and after unplugging the board tells
# you which is which.
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem* /dev/ttyACM* /dev/cu.usbserial* /dev/ttyUSB*))

# idf.py inside the exported IDF environment, with -p PORT when known.
IDF = source "$(EXPORT)" >/dev/null && idf.py $(if $(PORT),-p "$(PORT)",)

.DEFAULT_GOAL := build

.PHONY: help
help: ## Show this help
	@grep -hE '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | \
	  awk 'BEGIN{FS=":.*?## "}{printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

# Internal: fail early and clearly if ESP-IDF is not where we think it is.
.PHONY: check-idf
check-idf:
	@if [ -z "$(IDF_PATH)" ] || [ ! -f "$(EXPORT)" ]; then \
	  echo "ESP-IDF not found."; \
	  echo "Install v5.5.1+ (https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/),"; \
	  echo "then either export IDF_PATH, or run:  make IDF_PATH=/path/to/esp-idf $(MAKECMDGOALS)"; \
	  exit 1; \
	fi

# Internal: the audio stack lives in a submodule; a checkout without it fails
# deep inside CMake, so say so plainly up front.
.PHONY: check-deps
check-deps:
	@if [ ! -d deps/esp-webrtc-solution/components ]; then \
	  echo "deps/esp-webrtc-solution is missing (git submodule not initialised)."; \
	  echo "Run:  git submodule update --init --recursive"; \
	  exit 1; \
	fi

# Internal: configure the chip target once (also does the first CMake configure
# and downloads managed components).
.PHONY: ensure-target
ensure-target: check-idf check-deps
	@if [ ! -f build/CMakeCache.txt ] || ! grep -q "IDF_TARGET:STRING=$(TARGET)" build/CMakeCache.txt 2>/dev/null; then \
	  echo ">> configuring target $(TARGET) (first run — this also fetches components, and can take a few minutes)…"; \
	  source "$(EXPORT)" >/dev/null && idf.py set-target $(TARGET); \
	fi

# Internal: regenerate env_config.generated.h from .env before every build, so
# edits to .env always take effect. Does not rely on CMake noticing that .env
# appeared/changed (its configure-dependency detection is unreliable for a file
# created after the first configure). The generator only rewrites the header
# when the content actually changed, so this stays a no-op when nothing moved.
.PHONY: gen-env
gen-env:
	@python3 tools/gen_env_header.py .env main/env_config.generated.h

# Internal: require a serial port for flash/monitor/erase.
.PHONY: check-port
check-port:
	@if [ -z "$(PORT)" ]; then \
	  echo "No serial port auto-detected."; \
	  echo "Plug in the board, or pass one: make PORT=/dev/cu.usbmodem101 $(MAKECMDGOALS)"; \
	  exit 1; \
	fi

.PHONY: setup
setup: ## One-time setup: create .env (edit it after) and configure the target
	@if [ -f .env ]; then echo ".env already present."; \
	else cp .env.example .env && echo ">> created .env — fill in WIFI_SSID / WIFI_PASSWORD / VAPI_API_KEY / VAPI_ASSISTANT_ID, then: make run"; fi
	@$(MAKE) --no-print-directory ensure-target

.PHONY: env
env: ## Create .env from .env.example (if missing)
	@if [ -f .env ]; then echo ".env already exists — leaving it alone"; \
	else cp .env.example .env && echo "Created .env — fill in your WiFi + Vapi keys"; fi

.PHONY: build
build: gen-env ensure-target ## Build the firmware
	$(IDF) build

.PHONY: flash
flash: gen-env ensure-target check-port ## Build + flash (auto-detects PORT)
	$(IDF) flash

.PHONY: monitor
monitor: check-idf check-port ## Open the serial monitor
	$(IDF) monitor

.PHONY: run flashmonitor
run flashmonitor: gen-env ensure-target check-port ## Build + flash + monitor (the usual one)
	$(IDF) flash monitor

.PHONY: menuconfig
menuconfig: ensure-target ## Open the IDF configuration menu (alternative to .env)
	source "$(EXPORT)" >/dev/null && idf.py menuconfig

.PHONY: size
size: gen-env ensure-target ## Show binary size breakdown
	$(IDF) size

.PHONY: set-target
set-target: check-idf ## Force re-set the chip target (default: esp32s3)
	source "$(EXPORT)" >/dev/null && idf.py set-target $(TARGET)

.PHONY: erase
erase: check-idf check-port ## Erase the entire flash (clears stale NVS WiFi creds)
	$(IDF) erase-flash

.PHONY: clean
clean: check-idf ## Remove build output
	source "$(EXPORT)" >/dev/null && idf.py clean

.PHONY: fullclean
fullclean: ## Remove build/, managed_components/, and the dependency lock
	@if [ -n "$(IDF_PATH)" ] && [ -f "$(EXPORT)" ]; then \
	  source "$(EXPORT)" >/dev/null && idf.py fullclean || true; \
	fi
	rm -rf build managed_components dependencies.lock
