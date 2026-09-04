#!/usr/bin/env bash
# Rigicon Live - Linux build helper (wraps Makefile).
set -euo pipefail
cd "$(dirname "$0")/../../.."
make clean >/dev/null 2>&1 || true
make
echo
echo "Built: dist/linux/RigiconLive"
echo "Run  : ./dist/linux/RigiconLive"
