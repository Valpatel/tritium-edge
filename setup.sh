#!/usr/bin/env bash
# Moved to scripts/setup.sh — this wrapper exists for backwards compatibility.
exec "$(dirname "$0")/scripts/setup.sh" "$@"
