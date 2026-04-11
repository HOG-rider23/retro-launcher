#!/bin/bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export SDL_VIDEODRIVER=kmsdrm
export SDL_AUDIODRIVER=alsa
exec /home/jernej/retro-launcher/emulators/chip8 "$@"