# ESP32 Hardware Project - Build Automation
# Usage:
#   make build                        Build default board (touch-amoled-241b)
#   make build BOARD=amoled-191m      Build for specific board
#   make flash BOARD=touch-amoled-241b Flash firmware
#   make monitor                      Open serial monitor
#   make flash-monitor BOARD=...      Flash then monitor
#   make clean                        Clean build artifacts
#   make list-boards                  Show available board environments
#   make list-apps                    Show available apps
#   make new-app NAME=myapp           Scaffold a new app
#   make sim                          Build & run simulator (default board)
#   make sim BOARD=amoled-191m        Simulate specific board
#   make sim-all                      Launch all boards side by side

BOARD ?= touch-amoled-241b
APP ?=
PIO ?= pio
MONITOR_BAUD ?= 115200
PROJECT_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

# Board environments defined in platformio.ini
BOARDS := touch-amoled-241b amoled-191m touch-amoled-18 touch-lcd-35bc touch-lcd-43c-box touch-lcd-349

# Validate board selection
_VALID_BOARD := $(filter $(BOARD),$(BOARDS))

.PHONY: build flash monitor flash-monitor clean list-boards list-apps new-app check-board format sim sim-all

check-board:
ifndef _VALID_BOARD
	$(error Invalid BOARD='$(BOARD)'. Run 'make list-boards' to see options)
endif

build: check-board
	$(PIO) run -e $(BOARD)

flash: check-board
	$(PIO) run -e $(BOARD) -t upload

monitor:
	$(PIO) device monitor -b $(MONITOR_BAUD)

flash-monitor: check-board
	$(PIO) run -e $(BOARD) -t upload -t monitor

clean:
	$(PIO) run -t clean

list-boards:
	@echo "Available board environments:"
	@echo ""
	@echo "  touch-amoled-241b   ESP32-S3-Touch-AMOLED-2.41-B  (600x450, RM690B0)"
	@echo "  amoled-191m         ESP32-S3-AMOLED-1.91-M        (240x536, RM67162)"
	@echo "  touch-amoled-18     ESP32-S3-Touch-AMOLED-1.8     (368x448, SH8601Z)"
	@echo "  touch-lcd-35bc      ESP32-S3-Touch-LCD-3.5B-C     (320x480, AXS15231B*)"
	@echo "  touch-lcd-43c-box   ESP32-S3-Touch-LCD-4.3C-BOX   (800x480, ST7262 RGB)"
	@echo "  touch-lcd-349       ESP32-S3-Touch-LCD-3.49        (172x640, AXS15231B*)"
	@echo ""
	@echo "  * AXS15231B boards use placeholder driver - not yet functional"
	@echo ""
	@echo "Default: BOARD=touch-amoled-241b"

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
