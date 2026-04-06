#include <SDL2/SDL.h>
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <cstring>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <unistd.h>
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

// CHIP-8 resolution
const unsigned int CHIP8_W = 64;
const unsigned int CHIP8_H = 32;
const unsigned int SCREEN_W = 640;
const unsigned int SCREEN_H = 480;
const unsigned int SCALE = 10;
const unsigned int OFFSET_X = 0;
const unsigned int OFFSET_Y = (SCREEN_H - CHIP8_H * SCALE) / 2;

// Audio (unchanged)
const int SAMPLE_RATE = 44100;
const int AMPLITUDE = 6000;
const int BEEP_FREQ = 440;

struct AudioState {
    bool playing = false;
    float phase = 0.0f;
};

void audioCallback(void* userdata, Uint8* stream, int len) { /* unchanged */ }

// MCP23017 helper
int mcp_fd = -1;

bool initMCP() {
    if (mcp_fd >= 0) return true;
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", I2C_BUS);
    mcp_fd = open(path, O_RDWR);
    if (mcp_fd < 0) { std::cerr << "Failed to open I2C bus\n"; return false; }
    if (ioctl(mcp_fd, I2C_SLAVE, MCP_ADDR) < 0) {
        close(mcp_fd); mcp_fd = -1; std::cerr << "Failed to set I2C address\n"; return false;
    }
    uint8_t cfg[2];
    cfg[0] = IODIRA; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // Port A = inputs
    cfg[0] = IODIRB; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // Port B = inputs
    return true;
}

uint16_t readMCPButtons() {
    if (mcp_fd < 0) return 0xFFFF;
    uint8_t reg = GPIOA;
    write(mcp_fd, &reg, 1);
    uint8_t data[2];
    if (read(mcp_fd, data, 2) != 2) return 0xFFFF;
    // active-low → pressed when bit == 0
    return (~(data[0] | (data[1] << 8))) & 0xFFFF;
}

class Chip8 { /* unchanged class content */ };

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "Usage: " << argv[0] << " rom.ch8\n"; return 1; }

    Chip8 chip8;
    if (!chip8.loadROM(argv[1])) { std::cout << "Failed to load ROM\n"; return 1; }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_Window* window = SDL_CreateWindow("CHIP-8", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    // === MCP INIT ===
    initMCP();

    // Audio setup unchanged...

    bool quit = false;
    const int CPU_FREQ = 2000;
    const int FRAME_RATE = 60;
    const int CYCLES_PER_FRAME = CPU_FREQ / FRAME_RATE;

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
        }

        // === READ MCP BUTTONS EVERY FRAME ===
        uint16_t pressed = readMCPButtons();

        // Map your physical buttons to CHIP-8 keypad (0-F)
        chip8.keypad[0x5] = (pressed & (1 << UP_A_PIN)) != 0;      // UP    → 5
        chip8.keypad[0x8] = (pressed & (1 << DOWN_A_PIN)) != 0;    // DOWN  → 8
        chip8.keypad[0x4] = (pressed & (1 << LEFT_A_PIN)) != 0;    // LEFT  → 4
        chip8.keypad[0x6] = (pressed & (1 << RIGHT_A_PIN)) != 0;   // RIGHT → 6
        chip8.keypad[0xA] = (pressed & (1 << A_A_PIN)) != 0;       // A     → A
        chip8.keypad[0xB] = (pressed & (1 << B_B_PIN)) != 0;       // B     → B
        chip8.keypad[0x7] = (pressed & (1 << START_A_PIN)) != 0;   // START → 7
        chip8.keypad[0xC] = (pressed & (1 << SELECT_A_PIN)) != 0;  // SELECT→ C
        // X and Y are available if you need them later
        chip8.keypad[0xD] = (pressed & (1 << X_B_PIN)) != 0;
        chip8.keypad[0xE] = (pressed & (1 << Y_B_PIN)) != 0;

        // Run CPU + timers + render (unchanged)
        for (int i = 0; i < CYCLES_PER_FRAME; i++) chip8.emulateCycle();
        chip8.updateTimers();

        // Render loop unchanged...
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        // ... (your existing render code)
        SDL_RenderPresent(renderer);

        audio.playing = chip8.sound_timer > 0;
        SDL_Delay(1000 / FRAME_RATE);
    }

    SDL_Quit();
    if (mcp_fd >= 0) close(mcp_fd);
    return 0;
}