#!/usr/bin/env bash
# Rigicon Live - macOS build helper (wraps Makefile).
set -euo pipefail
cd "$(dirname "$0")/../../.."
make clean >/dev/null 2>&1 || true
make
echo
echo "Built: dist/macos/RigiconLive"
echo "Run  : ./dist/macos/RigiconLive"
