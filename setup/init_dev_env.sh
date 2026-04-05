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
install_if_missing "python3-smbus2"
install_if_missing "i2c-tools"


# Helper function: ensure i2c-dev kernel module is loaded
ensure_i2c_dev_module() {
    if ! lsmod | grep -q '^i2c_dev '; then
        echo "→ Loading i2c-dev kernel module..."
        sudo modprobe i2c-dev
        echo "i2c-dev" | sudo tee -a /etc/modules
        echo "✓ i2c-dev module loaded and added to /etc/modules"
    else
        echo "✓ i2c-dev kernel module is already loaded"
    fi
}

echo "Checking i2c-dev kernel module..."
ensure_i2c_dev_module

# Helper function: ensure Waveshare 3.5-inch DPI DTBO files are available
ensure_waveshare_dtbo_files() {
    local dtbo_dir="/boot/firmware/overlays"
    
    if ! ls "$dtbo_dir" 2>/dev/null | grep -q -E 'waveshare|35dpi'; then
        echo "→ Downloading Waveshare 3.5-inch DPI DTBO files..."
        wget -q https://files.waveshare.com/wiki/3.5inch%20DPI%20LCD/3.5DPI-dtbo.zip -O /tmp/3.5DPI-dtbo.zip
        
        echo "→ Extracting DTBO files..."
        unzip -o /tmp/3.5DPI-dtbo.zip -d /tmp/
        
        echo "→ Creating overlays directory and copying DTBO files..."
        sudo mkdir -p "$dtbo_dir"
        sudo cp /tmp/3.5DPI-dtbo/*.dtbo "$dtbo_dir/"
        
        echo "✓ Waveshare 3.5-inch DPI DTBO files installed"
    else
        echo "✓ Waveshare 3.5-inch DPI DTBO files are already available"
    fi
}

echo "Checking Waveshare 3.5-inch DPI DTBO files..."
ensure_waveshare_dtbo_files

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