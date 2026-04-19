#!/bin/bash

# ================================================
# init_dev_env.sh
# Initializes C++ development environment on Debian Trixie
# Checks and installs: make (build-essential) + SDL2 dev libraries
# ================================================

set -e  # Stop on any error

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

# Helper function: check if git is available
check_git_available() {
    if ! command -v git &> /dev/null; then
        echo "✗ ERROR: git is not installed or not in PATH"
        echo "Please install git and try again."
        exit 1
    else
        echo "✓ git is available"
    fi
}

# Helper function: check if OS is Debian Trixie 13.3
check_os_version() {
    local version_codename
    local debian_version_full
    
    version_codename=$(grep '^VERSION_CODENAME=' /etc/os-release | cut -d= -f2)
    debian_version_full=$(grep '^DEBIAN_VERSION_FULL=' /etc/os-release | cut -d= -f2)
    
    if [ "$version_codename" = "trixie" ] && [ "$debian_version_full" = "13.3" ]; then
        echo "✓ OS is Debian Trixie 13.3"
    else
        echo "✗ ERROR: This script requires Debian Trixie 13.3"
        echo "Detected: $version_codename $debian_version_full"
        exit 1
    fi
}

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

echo "=== Initializing C++ SDL2 Development Environment ==="
echo "Running on: $(cat /etc/os-release | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"')"
echo ""

echo "Checking git availability..."
check_git_available

echo "Checking OS version..."
check_os_version

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

# wtype for disable mouse cursor in SDL2 apps (optional but recommended for a cleaner experience)
install_if_missing "wtype"


echo "Checking i2c-dev kernel module..."
ensure_i2c_dev_module

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