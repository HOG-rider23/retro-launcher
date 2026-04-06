#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <algorithm>
#include <fstream>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <sys/ioctl.h>

// === MCP23017 CONFIGURATION (your pins) ===
const int I2C_BUS = 11;
const uint8_t MCP_ADDR = 0x27;
const uint8_t UP_A_PIN = 1;
const uint8_t DOWN_A_PIN = 2;
const uint8_t LEFT_A_PIN = 3;
const uint8_t RIGHT_A_PIN = 4;
const uint8_t START_A_PIN = 5;
const uint8_t SELECT_A_PIN = 6;
const uint8_t A_A_PIN = 7;

const uint8_t B_B_PIN = 0;
const uint8_t X_B_PIN = 1;
const uint8_t Y_B_PIN = 2;

const uint8_t IODIRB = 0x01;
const uint8_t GPIOB = 0x15;
const uint8_t IODIRA = 0x00;
const uint8_t GPIOA = 0x14;

// MCP helper (same as in chip8.cpp)
int mcp_fd = -1;

bool initMCP() { /* identical to the one in chip8.cpp above */ }
uint16_t readMCPButtons() { /* identical to the one in chip8.cpp above */ }

// rest of your main.cpp code (unchanged until the event loop)

namespace fs = std::filesystem;

// ... (your existing variables, functions, initSDL, loadROMs, etc. stay exactly the same)

bool handleKey(SDL_Keycode key, int& scrollOffset, int maxVisible) { /* your existing function stays */ }

int main(int argc, char* argv[]) {
    // ... your existing debug / init code ...

    if (!initSDL()) return 1;

    // === MCP INIT ===
    initMCP();

    fs::path baseDir = getBaseDir();
    loadROMs(baseDir / "roms");
    cacheStaticTextures();
    updateCountTexture();
    cacheTextures();

    int scrollOffset = 0;
    int maxVisible = (SCREEN_HEIGHT - LIST_TOP_PAD - ITEM_HEIGHT - 4) / ITEM_HEIGHT;
    bool running = true;
    SDL_Event e;

    while (running) {
        Uint32 frameStart = SDL_GetTicks();

        // === READ MCP BUTTONS EVERY FRAME ===
        uint16_t pressed = readMCPButtons();

        // Simulate keyboard events from MCP
        if (pressed & (1 << UP_A_PIN))    handleKey(SDLK_UP, scrollOffset, maxVisible);
        if (pressed & (1 << DOWN_A_PIN))  handleKey(SDLK_DOWN, scrollOffset, maxVisible);
        if (pressed & (1 << LEFT_A_PIN))  handleKey(SDLK_LEFT, scrollOffset, maxVisible);
        if (pressed & (1 << RIGHT_A_PIN)) handleKey(SDLK_RIGHT, scrollOffset, maxVisible);
        if (pressed & (1 << A_A_PIN))     handleKey(SDLK_RETURN, scrollOffset, maxVisible);
        if (pressed & (1 << B_B_PIN))     handleKey(SDLK_BACKSPACE, scrollOffset, maxVisible);
        if (pressed & (1 << START_A_PIN)) handleKey(SDLK_ESCAPE, scrollOffset, maxVisible);
        if (pressed & (1 << SELECT_A_PIN)) handleKey(SDLK_TAB, scrollOffset, maxVisible);

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
        }

        // ... rest of your drawing loop stays exactly the same ...

        SDL_Delay(FRAME_DELAY);
    }

    shutdownSDL();
    if (mcp_fd >= 0) close(mcp_fd);
    return 0;
}