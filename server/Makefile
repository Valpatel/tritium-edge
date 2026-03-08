# TRITIUM-EDGE Fleet Server — Makefile
# Usage:
#   make server         Start the fleet server (port 8080)
#   make test           Run Python unit tests
#   make e2e            Run Playwright E2E tests (server must be running)
#   make e2e-headed     Run E2E tests with visible browser
#   make e2e-install    Install E2E dependencies only

.PHONY: server test e2e e2e-headed e2e-install

E2E_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))tests/e2e

server:
	./start.sh

test:
	python -m pytest tests/ -v

e2e-install:
	cd $(E2E_DIR) && npm install && npx playwright install --with-deps chromium

e2e:
	$(E2E_DIR)/run-e2e.sh

e2e-headed:
	$(E2E_DIR)/run-e2e.sh --headed
