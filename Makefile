# ESP32 Hardware Project - Build Automation
# Usage:
#   make build                              Build default board (starfield app)
#   make build BOARD=touch-lcd-35bc         Build for specific board
#   make build BOARD=touch-lcd-35bc APP=camera  Build camera app for 3.5B-C
#   make flash BOARD=touch-lcd-35bc         Flash firmware (auto-detects port)
#   make monitor                            Open serial monitor
#   make flash-monitor BOARD=...            Flash then monitor
#   make identify                           Identify connected boards
#   make clean                              Clean build artifacts
#   make list-boards                        Show available board environments
#   make list-apps                          Show available apps
#   make new-app NAME=myapp                 Scaffold a new app
#   make sim                                Build & run simulator
#   make sim-all                            Launch all boards side by side

BOARD ?= touch-amoled-241b
APP ?=
PIO ?= pio
MONITOR_BAUD ?= 115200
PROJECT_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

# Board environments defined in platformio.ini
BOARDS := touch-amoled-241b amoled-191m touch-amoled-18 touch-lcd-35bc touch-lcd-43c-box touch-lcd-349

# App suffix mapping: APP=camera -> env:BOARD-camera, APP=system -> env:BOARD-system
# Empty APP uses the base env (starfield default)
ifdef APP
  ENV := $(BOARD)-$(APP)
else
  ENV := $(BOARD)
endif

# Validate board selection
_VALID_BOARD := $(filter $(BOARD),$(BOARDS))

# Auto-detect serial port
PORT := $(shell ls /dev/ttyACM0 2>/dev/null || ls /dev/ttyUSB0 2>/dev/null || echo "")

.PHONY: build flash monitor flash-monitor clean list-boards list-apps new-app check-board format sim sim-all identify fix-perms

check-board:
ifndef _VALID_BOARD
	$(error Invalid BOARD='$(BOARD)'. Run 'make list-boards' to see options)
endif

build: check-board
	$(PIO) run -e $(ENV)

flash: check-board fix-perms
ifdef PORT
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT)
else
	$(error No serial device found. Connect a board via USB.)
endif

monitor: fix-perms
ifdef PORT
	$(PIO) device monitor -b $(MONITOR_BAUD) -p $(PORT)
else
	$(PIO) device monitor -b $(MONITOR_BAUD)
endif

flash-monitor: check-board fix-perms
ifdef PORT
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT) && $(PIO) device monitor -b $(MONITOR_BAUD) -p $(PORT)
else
	$(PIO) run -e $(ENV) -t upload -t monitor
endif

identify:
	@python3 $(PROJECT_DIR)/tools/detect_boards.py

clean:
	$(PIO) run -t clean

# Fix serial port permissions (needs sudo)
fix-perms:
ifdef PORT
	@if [ ! -w "$(PORT)" ]; then \
		echo "Fixing permissions on $(PORT)..."; \
		sudo chmod 666 $(PORT); \
	fi
endif

list-boards:
	@echo "Available boards:"
	@echo ""
	@echo "  touch-amoled-241b   ESP32-S3-Touch-AMOLED-2.41-B  (450x600, RM690B0)"
	@echo "  amoled-191m         ESP32-S3-AMOLED-1.91-M        (240x536, RM67162)"
	@echo "  touch-amoled-18     ESP32-S3-Touch-AMOLED-1.8     (368x448, SH8601Z)"
	@echo "  touch-lcd-35bc      ESP32-S3-Touch-LCD-3.5B-C     (320x480, AXS15231B)"
	@echo "  touch-lcd-43c-box   ESP32-S3-Touch-LCD-4.3C-BOX   (800x480, ST7262 RGB)"
	@echo "  touch-lcd-349       ESP32-S3-Touch-LCD-3.49       (172x640, AXS15231B)"
	@echo ""
	@echo "Default: BOARD=touch-amoled-241b"
	@echo ""
	@echo "App variants (use APP=name):"
	@echo "  (none)    Starfield demo (default)"
	@echo "  camera    Camera preview (3.5B-C only)"
	@echo "  system    Full hardware dashboard"
	@echo "  wifi      WiFi setup"
	@echo "  ui        UI demo"

list-apps:
	@echo "Available apps:"
	@echo ""
	@for dir in $(PROJECT_DIR)/apps/*/; do \
		name=$$(basename "$$dir"); \
		if [ "$$name" != "_template" ]; then \
			echo "  $$name"; \
		fi; \
	done
	@echo ""
	@echo "Create a new app: make new-app NAME=myapp"

new-app:
ifndef NAME
	$(error Usage: make new-app NAME=myapp)
endif
	@bash $(PROJECT_DIR)/scripts/new-app.sh "$(NAME)"

format:
	@find $(PROJECT_DIR)/src $(PROJECT_DIR)/include $(PROJECT_DIR)/lib $(PROJECT_DIR)/apps \
		-name '*.cpp' -o -name '*.h' -o -name '*.hpp' | \
		xargs clang-format -i --style=file
	@echo "Formatted all source files."

# ---------------------------------------------------------------------------
# Desktop Simulator (SDL2)
# ---------------------------------------------------------------------------
SIM_BINARY := $(PROJECT_DIR)/.pio/build/simulator/program

sim: $(SIM_BINARY)
	$(SIM_BINARY) --board $(BOARD)

$(SIM_BINARY): FORCE
	$(PIO) run -e simulator

sim-all: $(SIM_BINARY)
	@echo "Launching simulator for all boards..."
	@for board in $(BOARDS); do \
		echo "--- $$board ---"; \
		$(SIM_BINARY) --board $$board & \
	done; \
	echo "All boards launched. Press Ctrl+C to stop."; \
	wait

FORCE:
