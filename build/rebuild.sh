#!/bin/bash
# ==============================================================
# update.sh - Retro Launcher Deploy Script for Pi Zero 2 W
# Run this after you push changes from your Ubuntu desktop.
# ==============================================================
set -e  # Stop on any error

# ================== CONFIGURATION (edit these) =================
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"  # Parent directory of build/
BINARY_NAME="retro-launcher"           # ← Name of your final executable
BRANCH="main"                          # ← Usually main or master
BUILD_JOBS=2                           # ← Safe for Pi Zero 2 W (use 4 if you want faster)

# Optional: path to your binary after build
INSTALL_BINARY="/home/pi/retro-launcher/$BINARY_NAME"
# ==============================================================

# Function to update config files from example templates
update_config_file() {
    local source_file="$1"
    local target_file="$2"
    if [ -f "$source_file" ]; then
        if [ -f "$target_file" ]; then
            if diff -q "$target_file" "$source_file" >/dev/null; then
                echo "   No changes needed for $target_file"
                return
            else
                echo "→ Updating $target_file file..."
                sudo cp "$source_file" "$target_file"
                if [ -f "$target_file" ]; then
                    echo "   Updated $target_file from template"
                else
                    echo "   Created new $target_file from template"
                fi
            fi
        fi
    fi
}

echo "=== Starting Retro Launcher Update ==="
echo "Project : $PROJECT_DIR"
echo "Branch  : $BRANCH"
echo ""

# 1. Go to project folder
cd "$PROJECT_DIR"
echo "Working derectory: ${PWD}"

# 2. Make sure we are on the correct branch and up-to-date
echo "→ Switching to branch $BRANCH and pulling latest changes..."
git checkout "$BRANCH"
git pull --ff-only origin "$BRANCH"

# 3. Update retro-launcher.service files
echo "→ Updating files..."
update_config_file "$PROJECT_DIR/config/etc/systemd/system/retro-launcher.service" "/etc/systemd/system/retro-launcher.service"
update_config_file "$PROJECT_DIR/config/boot/firmware/config.txt" "/boot/firmware/config.txt"
update_config_file "$PROJECT_DIR/config/boot/firmware/cmdline.txt" "/boot/firmware/cmdline.txt"
update_config_file "$PROJECT_DIR/config/etc/udev/rules.d/99-waveshare-touch.rules" "/etc/udev/rules.d/99-waveshare-touch.rules"

# 4. Build the project
cd "$PROJECT_DIR/src"
echo "Working derectory: ${PWD}"
make clean || true          # safe if no clean target
echo "→ Building with make ($BUILD_JOBS jobs)..."
make -j"$BUILD_JOBS" all
make install


sudo systemctl daemon-reload
if sudo systemctl is-enabled --quiet retro-launcher.service; then
    echo "   retro-launcher.service is already enabled"
else
    echo "→ Enabling retro-launcher.service..."
    sudo systemctl enable retro-launcher.service
fi

echo ""
echo "✅ Build completed successfully!"
echo "→ Rebooting system..."
echo ""
sudo reboot
