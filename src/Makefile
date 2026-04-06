OUT_DIR = output
INSTALL_DIR = /home/jernej/retro-launcher

all: $(OUT_DIR) launcher chip8

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

launcher: main.cpp
	g++ main.cpp -o retro-launcher `pkg-config --cflags --libs sdl2 sdl2_ttf` -lSDL2 -lSDL2_ttf

chip8: chip8.cpp
	g++ chip8.cpp -o chip8 `pkg-config --cflags --libs sdl2` -lSDL2

install:
	mkdir -p $(INSTALL_DIR)
	mkdir -p $(INSTALL_DIR)/emulators
	mkdir -p $(INSTALL_DIR)/roms
	cp $(OUT_DIR)/launcher $(INSTALL_DIR)/retro-launcher
	cp $(OUT_DIR)/chip8 $(INSTALL_DIR)/emulators/
	cp ../third-party/* $(INSTALL_DIR)/roms/

clean:
	rm -rf $(OUT_DIR)
