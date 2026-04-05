# 1. Re-download and place DTBO files in the CORRECT modern location
wget https://files.waveshare.com/wiki/3.5inch%20DPI%20LCD/3.5DPI-dtbo.zip -O /tmp/3.5DPI-dtbo.zip
unzip -o /tmp/3.5DPI-dtbo.zip -d /tmp/
sudo mkdir -p /boot/firmware/overlays
sudo cp /tmp/3.5DPI-dtbo/*.dtbo /boot/firmware/overlays/