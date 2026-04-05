OUT_DIR = retro-launcher

all: clean $(OUT_DIR) launcher chip8

$(OUT_DIR):
	mkdir -p $(OUT_DIR)     

launcher: main.cpp
	g++ -std=c++17 main.cpp -o $(OUT_DIR)/launcher -lSDL2 -lSDL2_ttf

chip8: chip8.cpp
	g++ -std=c++17 chip8.cpp -o $(OUT_DIR)/chip8 -lSDL2 -lSDL2_ttf

clean:
	rm -rf $(OUT_DIR)
