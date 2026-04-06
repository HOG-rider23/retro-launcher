OUT_DIR = output
INSTALL_DIR = /home/jernej/retro-launcher

all: clean $(OUT_DIR) launcher chip8 install

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

launcher: main.cpp
	g++ -std=c++17 main.cpp -o $(OUT_DIR)/launcher -lSDL2 -lSDL2_ttf

chip8: chip8.cpp
	g++ -std=c++17 chip8.cpp -o $(OUT_DIR)/chip8 -lSDL2 -lSDL2_ttf

install:
	mkdir -p $(INSTALL_DIR)
	cp $(OUT_DIR)/launcher $(INSTALL_DIR)/
	cp $(OUT_DIR)/chip8 $(INSTALL_DIR)/

clean:
	rm -rf $(OUT_DIR)
