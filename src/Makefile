OUT_DIR = output
INSTALL_DIR = /home/jernej/retro-launcher

all: $(OUT_DIR) launcher chip8

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

launcher: main.cpp
	g++ main.cpp -o $(OUT_DIR)/retro-launcher `pkg-config --cflags --libs sdl2 sdl2-ttf` -lSDL2 -lSDL2_ttf

chip8: chip8.cpp
	g++ chip8.cpp -o $(OUT_DIR)/chip8 `pkg-config --cflags --libs sdl2` -lSDL2

install:
	mkdir -p $(INSTALL_DIR)
	mkdir -p $(INSTALL_DIR)/emulators
	mkdir -p $(INSTALL_DIR)/roms
	cp $(OUT_DIR)/retro-launcher $(INSTALL_DIR)/retro-launcher
	cp $(OUT_DIR)/chip8 $(INSTALL_DIR)/emulators/
	cp ../third-party/* $(INSTALL_DIR)/roms/
	cp ../setup/start-chip8.sh $(INSTALL_DIR)/emulators/

clean:
	rm -rf $(OUT_DIR)
