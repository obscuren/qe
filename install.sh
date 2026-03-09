#!/bin/sh
set -e

# Quick Ed installer
# Usage: curl -fsSL https://jeff.lookingforteam.com/qe/install.sh | sh

REPO="obscuren/qe"
INSTALL_DIR="/usr/local/bin"
BUILD_DIR=$(mktemp -d)

cleanup() { rm -rf "$BUILD_DIR"; }
trap cleanup EXIT

echo "Installing Quick Ed..."

# ── Check dependencies ───────────────────────────────────────────────
missing=""
command -v git   >/dev/null 2>&1 || missing="$missing git"
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
command -v cc    >/dev/null 2>&1 && command -v make >/dev/null 2>&1 || missing="$missing build-essential"

if [ -n "$missing" ]; then
    echo "Missing dependencies:$missing"
    echo ""
    if command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf install${missing} lua-devel cmake gcc make"
    elif command -v apt >/dev/null 2>&1; then
        echo "  sudo apt install${missing} liblua5.4-dev cmake gcc make"
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S${missing} lua cmake gcc make"
    elif command -v brew >/dev/null 2>&1; then
        echo "  brew install${missing} lua cmake"
    else
        echo "  Install:$missing and Lua 5.4 dev headers"
    fi
    exit 1
fi

# Check for Lua headers
if ! pkg-config --exists lua5.4 2>/dev/null && \
   ! pkg-config --exists lua54  2>/dev/null && \
   ! pkg-config --exists lua    2>/dev/null && \
   ! test -f /usr/include/lua.h && \
   ! test -f /usr/include/lua5.4/lua.h; then
    echo "Missing: Lua 5.4 development headers"
    echo ""
    if command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf install lua-devel"
    elif command -v apt >/dev/null 2>&1; then
        echo "  sudo apt install liblua5.4-dev"
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S lua"
    elif command -v brew >/dev/null 2>&1; then
        echo "  brew install lua"
    fi
    exit 1
fi

# ── Clone and build ─────────────────────────────────────────────────
echo "Cloning..."
git clone --depth 1 "https://github.com/$REPO.git" "$BUILD_DIR/qe" 2>/dev/null

echo "Building..."
cmake -S "$BUILD_DIR/qe" -B "$BUILD_DIR/qe/build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    >/dev/null 2>&1
cmake --build "$BUILD_DIR/qe/build" >/dev/null 2>&1

# ── Install ──────────────────────────────────────────────────────────
if [ -w "$INSTALL_DIR" ]; then
    cp "$BUILD_DIR/qe/build/qe" "$INSTALL_DIR/qe"
else
    echo "Installing to $INSTALL_DIR (requires sudo)..."
    sudo cp "$BUILD_DIR/qe/build/qe" "$INSTALL_DIR/qe"
fi

echo ""
echo "Done! Quick Ed installed to $(command -v qe || echo "$INSTALL_DIR/qe")"
echo "Run 'qe' to start."
