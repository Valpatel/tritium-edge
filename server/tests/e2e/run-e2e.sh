#!/usr/bin/env bash
# Run Playwright E2E tests for the TRITIUM-EDGE fleet admin dashboard.
# Usage:
#   ./run-e2e.sh                  # Run all tests (headless)
#   ./run-e2e.sh --headed         # Run with browser visible
#   ./run-e2e.sh --debug          # Run in debug mode
#   BASE_URL=http://host:port ./run-e2e.sh   # Custom server URL

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Install deps if needed
if [ ! -d node_modules ]; then
  echo "Installing dependencies..."
  npm install
  echo "Installing Playwright browsers..."
  npx playwright install --with-deps chromium
fi

# Default base URL
export BASE_URL="${BASE_URL:-http://localhost:8080}"

echo "Running E2E tests against ${BASE_URL}"
npx playwright test "$@"
