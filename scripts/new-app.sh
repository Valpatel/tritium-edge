#!/usr/bin/env bash
set -euo pipefail

# Scaffold a new app from the template directory.
# Usage: ./scripts/new-app.sh <app-name>

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APPS_DIR="$PROJECT_DIR/apps"
TEMPLATE_DIR="$APPS_DIR/_template"

NAME="${1:-}"

if [ -z "$NAME" ]; then
    echo "Usage: $0 <app-name>"
    echo "Example: $0 clock"
    exit 1
fi

# Validate name (lowercase alphanumeric and underscores only)
if ! echo "$NAME" | grep -qE '^[a-z][a-z0-9_]*$'; then
    echo "ERROR: App name must be lowercase, start with a letter, and contain only [a-z0-9_]."
    exit 1
fi

APP_DIR="$APPS_DIR/$NAME"

if [ -d "$APP_DIR" ]; then
    echo "ERROR: App '$NAME' already exists at $APP_DIR"
    exit 1
fi

# Create app directory
mkdir -p "$APP_DIR"

# Generate class name from app name (snake_case -> PascalCase + "App")
CLASS_NAME=$(echo "$NAME" | sed -r 's/(^|_)([a-z])/\U\2/g')App

# Create the app header
cat > "$APP_DIR/${NAME}_app.h" << EOF
#pragma once
#include "app.h"

class ${CLASS_NAME} : public App {
public:
    const char* name() override { return "${NAME}"; }
    void setup(LGFX& display) override;
    void loop(LGFX& display) override;

private:
    // Add your app state here
};
EOF

# Create the app implementation
cat > "$APP_DIR/${NAME}_app.cpp" << EOF
#include "${NAME}_app.h"

void ${CLASS_NAME}::setup(LGFX& display) {
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.println("${NAME}");
}

void ${CLASS_NAME}::loop(LGFX& display) {
    // TODO: Implement your app loop
}
EOF

echo "Created app '$NAME' at $APP_DIR/"
echo "Files:"
echo "  $APP_DIR/${NAME}_app.h"
echo "  $APP_DIR/${NAME}_app.cpp"
echo ""
echo "Next steps:"
echo "  1. Edit $APP_DIR/${NAME}_app.h to add state variables"
echo "  2. Implement setup() and loop() in $APP_DIR/${NAME}_app.cpp"
echo "  3. Build with: make build BOARD=touch-amoled-241b"
