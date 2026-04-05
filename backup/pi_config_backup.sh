#!/usr/bin/bash
# =============================================================================
# Raspberry Pi Config Backup & Restore Script (with AUTOMATED USB mounting)
# Tailored for your Pi Zero 2 W + LQ035NC111 HDMI LCD + GPIO buttons setup
# Fully automatic USB detection & mounting — no manual path selection needed!
# Backs up/restores key config files
# Run with: sudo ./pi_config_backup.sh [backup|restore]
# =============================================================================

set -euo pipefail

# ====================== CONFIGURATION ======================
# Add or remove any files/folders you want here.
# The script will skip any that don't exist.
declare -a IMPORTANT_FILES=(
    "/boot/firmware/config.txt"
    "/boot/firmware/cmdline.txt"
    "/etc/fstab"
    "/home/jernej/retro-launcher"
    "/home/jernej/pi_config_backup.sh"
    "/home/jernej/disply_3_5_setup.sh"
    "/etc/udev/rules.d/99-waveshare-touch.rules"
    # Add your own here if needed
)

BACKUP_BASE_FOLDER="Pi_Config_Backups"
AUTO_MOUNTED=0          # Internal flag — do not change
USB_MOUNT=""            # Will be set automatically
# ===========================================================

function detect_usb_mount() {
    echo "🔍 Auto-detecting USB drive..."

    # 1. Check for already mounted USB drives (standard Raspberry Pi OS behaviour)
    USB_MOUNT=$(mount | grep -E '(/media/pi/|/mnt/)' | grep -E 'vfat|exfat|ntfs|fat|auto' | awk '{print $3}' | head -n1 || true)

    if [ -n "$USB_MOUNT" ]; then
        echo "✅ Found already mounted USB: $USB_MOUNT"
        AUTO_MOUNTED=0
        return 0
    fi

    # 2. No mounted USB → auto-mount the first removable partition
    echo "No mounted USB found — attempting auto-mount..."

    mkdir -p /mnt/usb_backup

    # Find the first unmounted removable partition (RM=1, type=part, no mountpoint)
    USB_PART=$(lsblk -r -o NAME,RM,TYPE,MOUNTPOINT | awk '$2 == "1" && $3 == "part" && $4 == "" {print "/dev/" $1; exit}' || true)

    if [ -z "$USB_PART" ]; then
        echo "❌ No removable USB drive or partition detected!"
        echo "Please insert a USB drive (formatted as FAT32/exFAT/NTFS) and run the script again."
        exit 1
    fi

    echo "Found USB partition: $USB_PART"

    # Mount with safe options (works on Raspberry Pi OS Bookworm+ for most USB drives)
    if mount -o uid=1000,gid=1000,umask=007 "$USB_PART" /mnt/usb_backup; then
        USB_MOUNT="/mnt/usb_backup"
        AUTO_MOUNTED=1
        echo "✅ Successfully auto-mounted USB to: $USB_MOUNT"
    else
        echo "❌ Failed to mount $USB_PART"
        echo "Tips:"
        echo "   • Make sure the drive is formatted as FAT32 (recommended)"
        echo "   • For exFAT install: sudo apt install exfatprogs"
        echo "   • For NTFS install: sudo apt install ntfs-3g"
        exit 1
    fi
}

function backup() {
    detect_usb_mount

    TIMESTAMP=$(date +"%Y-%m-%d_%H-%M-%S")
    TMP_DIR=$(mktemp -d /backup/pi_backup_XXXXXX)
    BACKUP_NAME="backup_$TIMESTAMP.tar.gz"
    FINAL_PATH="$USB_MOUNT/$BACKUP_BASE_FOLDER/$BACKUP_NAME"

    mkdir -p "$USB_MOUNT/$BACKUP_BASE_FOLDER"

    echo "📦 Creating temporary backup in: $TMP_DIR"

    for file in "${IMPORTANT_FILES[@]}"; do
        if [ -f "$file" ] || [ -d "$file" ]; then
            dest_dir="$TMP_DIR$(dirname "$file")"
            mkdir -p "$dest_dir"
            cp -a "$file" "$dest_dir/"
            echo "✅ Staged: $file"
        else
            echo "⚠️  Skipped (not found): $file"
        fi
    done

    echo "📦 Creating archive..."
    tar -czf "/backup/$BACKUP_NAME" -C "$TMP_DIR" .

    echo "📤 Copying archive to USB..."
    cp "/backup/$BACKUP_NAME" "$FINAL_PATH"

    echo "🧹 Cleaning up temporary files..."
    rm -rf "$TMP_DIR"
    rm -f "/backup/$BACKUP_NAME"

    echo ""
    echo "🎉 Backup complete!"
    echo "Saved as: $FINAL_PATH"
    ls -lh "$FINAL_PATH"
}

function restore() {
    detect_usb_mount

    echo "📋 Available backup archives on USB:"
    find "$USB_MOUNT/$BACKUP_BASE_FOLDER" -type f -name "*.tar.gz" 2>/dev/null | sort -r || echo "No backups found!"

    if [ ! -d "$USB_MOUNT/$BACKUP_BASE_FOLDER" ]; then
        echo "❌ No backup folder found on the USB drive."
        exit 1
    fi

    read -p "Paste FULL path to backup archive (.tar.gz): " ARCHIVE_PATH

    if [ ! -f "$ARCHIVE_PATH" ]; then
        echo "❌ File does not exist!"
        exit 1
    fi

    TMP_DIR=$(mktemp -d /backup/pi_restore_XXXXXX)
    TMP_ARCHIVE="$TMP_DIR/restore.tar.gz"

    echo "📥 Copying archive to temp location..."
    cp "$ARCHIVE_PATH" "$TMP_ARCHIVE"

    echo "📦 Extracting archive..."
    tar -xzf "$TMP_ARCHIVE" -C "$TMP_DIR"

    echo ""
    echo "⚠️  WARNING: This will OVERWRITE your current config files!"
    echo "Files that will be restored:"
    find "$TMP_DIR" -type f | sed "s|$TMP_DIR||"
    read -p "Type 'YES' to continue: " confirm

    if [ "$confirm" != "YES" ]; then
        echo "❌ Restore cancelled."
        rm -rf "$TMP_DIR"
        exit 0
    fi

    echo "🔄 Restoring files..."
    for file in "${IMPORTANT_FILES[@]}"; do
        src_file="$TMP_DIR$(dirname "$file")/$(basename "$file")"
        if [ -f "$src_file" ]; then
            cp -a "$src_file" "$file"
            echo "✅ Restored: $file"
        else
            echo "⚠️  Missing in archive: $file"
        fi
    done

    echo "🧹 Cleaning up..."
    rm -rf "$TMP_DIR"

    echo ""
    echo "🎉 Restore complete!"
    echo "👉 Recommended: sudo reboot"
}

# ====================== MAIN ======================
if [ "$(id -u)" -ne 0 ]; then
    echo "❌ This script must be run as root (sudo)."
    echo "Usage: sudo $0 [backup|restore]"
    exit 1
fi

case "${1:-}" in
    backup)
        backup
        ;;
    restore)
        restore
        ;;
    *)
        echo "Usage: sudo $0 [backup|restore]"
        echo ""
        echo "Examples:"
        echo "  sudo $0 backup     # Create a new timestamped backup"
        echo "  sudo $0 restore    # Choose a backup and restore it"
        ;;
esac

# Auto-unmount if we mounted the USB ourselves (safe ejection)
if [ "$AUTO_MOUNTED" -eq 1 ]; then
    echo ""
    echo "🔌 Unmounting USB drive for safe removal..."
    if umount "$USB_MOUNT" 2>/dev/null; then
        rmdir /mnt/usb_backup 2>/dev/null || true
        echo "✅ USB unmounted successfully. You can now safely remove the drive."
    else
        echo "⚠️  Could not unmount automatically (it may already be unmounted)."
    fi
fi

