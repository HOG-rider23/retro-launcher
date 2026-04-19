#!/bin/bash
# ==============================================================
# update.sh - Retro Launcher Deploy Script for Pi Zero 2 W
# Run this after you push changes from your Ubuntu desktop.
# ==============================================================

set -e  # Stop on any error

# ================== CONFIGURATION (edit these) =================
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"              # Parent directory of build/
BINARY_NAME="retro-launcher"                        # ← Name of your final executable
BRANCH="main"                                       # ← Usually main or master
BUILD_JOBS=2                                        # ← Safe for Pi Zero 2 W
INSTALL_DIRECTORY="/home/jernej/retro-launcher"

INSTALL_BINARY="$INSTALL_DIRECTORY/$BINARY_NAME"
# ==============================================================

# === NEW: Parse command-line arguments ===
NO_REBOOT=false
for arg in "$@"; do
    case $arg in
        --no-reboot|-n)
            NO_REBOOT=true
            ;;
    esac
done

# Function to update config files from example templates
update_config_file() {
    local source_file="$1"
    local target_file="$2"
    if [ -f "$source_file" ]; then
        if diff -q "$target_file" "$source_file" >/dev/null 2>&1; then
            echo "   No changes needed for $target_file"
            return
        else
            echo "→ Updating $target_file file..."
            sudo cp "$source_file" "$target_file"
            echo "   Updated $target_file from template"
        fi
    fi
}

echo "=== Starting Retro Launcher Update ==="
echo "Project : $PROJECT_DIR"
echo "Branch  : $BRANCH"
echo "Mode    : $( [ "$NO_REBOOT" = true ] && echo "No reboot (restart service only)" || echo "Full reboot" )"
echo ""

# 1. Go to project folder
cd "$PROJECT_DIR"
echo "Working directory: ${PWD}"

# 2. Make sure we are on the correct branch and up-to-date
echo "→ Switching to branch $BRANCH and pulling latest changes..."
git checkout "$BRANCH"
git pull --ff-only origin "$BRANCH"

# mouse configuraction
sudo mkdir -p /etc/xdg/labwc
sudo mkdir -p /usr/share/icons
cp -Rp "$PROJECT_DIR/config/usr/share/icons/xcursor-transparent" /usr/share/icons/

# 3. Update config files
echo "→ Updating configuration files..."
update_config_file "$PROJECT_DIR/config/etc/systemd/system/retro-launcher.service" "/etc/systemd/system/retro-launcher.service"
update_config_file "$PROJECT_DIR/config/boot/firmware/config.txt" "/boot/firmware/config.txt"
update_config_file "$PROJECT_DIR/config/boot/firmware/cmdline.txt" "/boot/firmware/cmdline.txt"
update_config_file "$PROJECT_DIR/config/etc/udev/rules.d/99-waveshare-touch.rules" "/etc/udev/rules.d/99-waveshare-touch.rules"
update_config_file "$PROJECT_DIR/config/etc/xdg/labwc/rc.xml" "/etc/xdg/labwc/rc.xml"
update_config_file "$PROJECT_DIR/config/etc/xdg/labwc/autostart" "/etc/xdg/labwc/autostart"
update_config_file "$PROJECT_DIR/config/etc/xdg/labwc/environment" "/etc/xdg/labwc/environment"

rm -rf "$INSTALL_DIRECTORY"/*

# 4. Build the project
cd "$PROJECT_DIR/src"
echo "Working directory: ${PWD}"
make clean || true
echo "→ Building with make ($BUILD_JOBS jobs)..."
make -j"$BUILD_JOBS" all
make install

# 5. Reload systemd and (re)start service
sudo systemctl daemon-reload
if sudo systemctl is-enabled --quiet retro-launcher.service; then
    echo "   retro-launcher.service is already enabled"
else
    echo "→ Enabling retro-launcher.service..."
    sudo systemctl enable retro-launcher.service
fi

sudo mkdir -p "/var/log/retro-launcher"
sudo chmod 777 "/var/log/retro-launcher"
sudo touch "/var/log/retro-launcher/retro-launcher.log"
sudo chmod 777 "/var/log/retro-launcher/retro-launcher.log"

# mouse configuraction
sudo chmod +x /etc/xdg/labwc/autostart

echo ""
echo "✅ Build completed successfully!"

# === NEW: Choose between reboot and service restart ===
if [ "$NO_REBOOT" = true ]; then
    echo "→ Restarting retro-launcher service only (no reboot)..."
    sudo systemctl restart retro-launcher.service
    echo "   Service restarted successfully."
    echo "   You can check status with: systemctl status retro-launcher.service"
else
    echo "→ Rebooting system..."
    sudo reboot
fi