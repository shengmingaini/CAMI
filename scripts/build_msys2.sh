#!/bin/bash
# ================================================================
# CAMI Build Script — MSYS2 MinGW64 Environment
#
# Run this INSIDE the MSYS2 mingw64 shell:
#   /c/msys64/msys2_shell.cmd -mingw64 -c "bash /f/AI/workbuddy/CAMI/scripts/build_msys2.sh"
#
# What it does:
#   1. Updates MSYS2 package database
#   2. Installs MinGW-w64 GCC, CMake, Make, Boost
#   3. Configures and builds the TCP echo benchmark
#
# Prerequisites:
#   - MSYS2 installed (winget install MSYS2.MSYS2)
#
# [PROTOTYPE] Tech stack verification build
# ================================================================

set -e

echo "=============================================="
echo "  CAMI Build — MSYS2 MinGW64"
echo "=============================================="
echo ""

# === Step 1: Update MSYS2 ===
echo "[1/4] Updating MSYS2 package database..."
pacman -Syu --noconfirm
echo ""

# === Step 2: Install toolchain + Boost ===
echo "[2/4] Installing MinGW-w64 GCC, CMake, Make, Boost..."
pacman -S --noconfirm \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-make \
    mingw-w64-x86_64-boost
echo ""

# === Step 3: Configure ===
echo "[3/4] Configuring CAMI benchmark build..."
cd /f/AI/workbuddy/CAMI
rm -rf build
mkdir -p build
cd build

cmake .. -G "MinGW Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCAMI_BUILD_BENCHMARK=ON \
    -DCAMI_BUILD_TESTS=OFF \
    -DCAMI_BUILD_MODULES=OFF
echo ""

# === Step 4: Build ===
echo "[4/4] Building tcp_echo_server and tcp_echo_benchmark..."
cmake --build . --target tcp_echo_server tcp_echo_benchmark -j$(nproc)
echo ""

echo "=============================================="
echo "  Build Complete!"
echo "=============================================="
echo "  Binaries:"
ls -la bin/ 2>/dev/null || ls -la *.exe 2>/dev/null
echo ""
echo "  Run server:  bin/tcp_echo_server 9999 \$(nproc)"
echo "  Run client:  bin/tcp_echo_benchmark 127.0.0.1 9999 16 64 1000000 8"
echo "=============================================="
