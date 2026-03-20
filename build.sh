#!/bin/bash
# Build script for Odyssey GBC
set -e

GBDK=/c/gbdk
LCC=$GBDK/bin/lcc
BUILD=build
SRC=src/main.c
TARGET=$BUILD/odyssey.gbc

echo "=== Converting assets ==="
python tools/convert_assets.py

echo "=== Compiling ==="
mkdir -p $BUILD
$LCC -msm83:gb -Wl-yp0x143=0xC0 -Wf--max-allocs-per-node50000 -o $TARGET $SRC

echo "=== Done ==="
ls -la $TARGET
echo "Open $TARGET in a GBC emulator (BGB, Emulicious, SameBoy)"
