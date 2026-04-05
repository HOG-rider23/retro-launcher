#!/bin/bash

# ================================================
# init_dev_env.sh
# Initializes C++ development environment on Debian Trixie
# Checks and installs: make (build-essential) + SDL2 dev libraries
# ================================================

set -e  # Stop on any error

echo "=== Initializing C++ SDL2 Development Environment ==="
echo "Running on: $(cat /etc/os-release | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"')"
echo ""

UPDATE_DONE=false

# Helper function: install package only if missing
install_if_missing() {
    local pkg=$1
    if ! dpkg -s "$pkg" &> /dev/null; then
        if [ "$UPDATE_DONE" = false ]; then
            echo "→ Updating package list..."
            sudo apt update -qq
            UPDATE_DONE=true
        fi
        echo "→ Installing $pkg..."
        sudo apt install -y "$pkg"
    else
        echo "✓ $pkg is already installed"
    fi
}

echo "Checking dependencies..."

# Build tools (includes make, g++, cmake, etc.)
install_if_missing "build-essential"

# SDL2 development libraries (C/C++)
install_if_missing "libsdl2-dev"
install_if_missing "libsdl2-image-dev"
install_if_missing "libsdl2-ttf-dev"
install_if_missing "libsdl2-mixer-dev"

echo ""
echo "✅ Development environment is ready!"
echo ""
echo "SDL2 version installed:"
pkg-config --modversion sdl2 2>/dev/null || echo "   (SDL2 not detected - something went wrong)"
echo ""
echo "You can now compile SDL2 C++ programs with:"
echo "   g++ your_program.cpp -o your_program \`pkg-config --cflags --libs sdl2\`"
echo ""
echo "Run this script anytime — it is safe to run multiple times."