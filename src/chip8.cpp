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

// === MCP23017 CONFIGURATION ===
const int I2C_BUS = 11;
const uint8_t MCP_ADDR = 0x27;
const uint8_t UP_A_PIN    = 1;
const uint8_t DOWN_A_PIN  = 2;
const uint8_t LEFT_A_PIN  = 3;
const uint8_t RIGHT_A_PIN = 4;
const uint8_t START_A_PIN = 5;
const uint8_t SELECT_A_PIN = 6;
const uint8_t A_A_PIN     = 7;
const uint8_t B_B_PIN     = 0;
const uint8_t X_B_PIN     = 1;
const uint8_t Y_B_PIN     = 2;

const uint8_t IODIRA = 0x00;
const uint8_t GPIOA  = 0x14;
const uint8_t IODIRB = 0x01;
const uint8_t GPIOB  = 0x15;

// CHIP-8 resolution + scaling
const unsigned int CHIP8_W = 64;
const unsigned int CHIP8_H = 32;
const unsigned int SCREEN_W = 640;
const unsigned int SCREEN_H = 480;
const unsigned int SCALE = 10;
const unsigned int OFFSET_X = 0;
const unsigned int OFFSET_Y = (SCREEN_H - CHIP8_H * SCALE) / 2;

// Audio
const int SAMPLE_RATE = 44100;
const int AMPLITUDE = 6000;
const int BEEP_FREQ = 440;

struct AudioState {
    bool playing = false;
    float phase = 0.0f;
};

void audioCallback(void* userdata, Uint8* stream, int len) {
    AudioState* audio = (AudioState*)userdata;
    Sint16* buf = (Sint16*)stream;
    int samples = len / 2;
    if (!audio->playing) {
        SDL_memset(stream, 0, len);
        return;
    }
    float step = (float)BEEP_FREQ / SAMPLE_RATE;
    for (int i = 0; i < samples; ++i) {
        buf[i] = (audio->phase < 0.5f) ? AMPLITUDE : -AMPLITUDE;
        audio->phase += step;
        if (audio->phase >= 1.0f) audio->phase -= 1.0f;
    }
}

// MCP23017
int mcp_fd = -1;

bool DEBUG_ENABLED = false;
std::ofstream logFile;

inline void debug(const std::string& msg) {
    if (DEBUG_ENABLED) {
        if (!logFile.is_open()) {
            logFile.open("/var/log/retro-launcher/chip8.log", std::ios::app);
        }
        logFile << "[DEBUG] " << msg << std::endl;
        logFile.flush();
    }
}

bool initMCP() {
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", I2C_BUS);
    mcp_fd = open(path, O_RDWR);
    if (mcp_fd < 0) {
        debug("Failed to open I2C bus");
        return false;
    }
    if (ioctl(mcp_fd, I2C_SLAVE, MCP_ADDR) < 0) {
        close(mcp_fd);
        mcp_fd = -1;
        debug("Failed to set I2C address");
        return false;
    }

    uint8_t cfg[2];

    // Port A and B = inputs
    cfg[0] = IODIRA; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);
    cfg[0] = IODIRB; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);

    // Enable internal pull-ups on ALL pins
    cfg[0] = 0x0C; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // GPPUA
    cfg[0] = 0x0D; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // GPPUB

    debug("MCP23017 initialized with internal pull-ups");
    return true;
}

uint16_t readMCPButtons() {
    if (mcp_fd < 0) return 0xFFFF;

    uint8_t reg = GPIOA;
    if (write(mcp_fd, &reg, 1) != 1) {
        debug("Failed to set GPIOA pointer");
        return 0xFFFF;
    }

    uint8_t data[2] = {0};
    if (read(mcp_fd, data, 2) != 2) {
        debug("Failed to read GPIO registers");
        return 0xFFFF;
    }

    // Debug raw values (this will show us what the MCP really sees)
    debug("Raw GPIOA=0x" + std::to_string(data[0]) + "  GPIOB=0x" + std::to_string(data[1]));

    // active-low with pull-ups: bit = 0 → pressed
    uint16_t raw = (static_cast<uint16_t>(data[1]) << 8) | data[0];
    uint16_t pressed = (~raw) & 0xFFFF;

    return pressed;
}

class Chip8 {
public:
    uint8_t memory[4096]{};
    uint8_t V[16]{};
    uint16_t I = 0;
    uint16_t pc = 0x200;

    uint16_t stack[16]{};
    uint8_t sp = 0;

    uint8_t delay_timer = 0;
    uint8_t sound_timer = 0;

    bool display[CHIP8_W * CHIP8_H]{};
    bool keypad[16]{};
    bool display_changed = true;  // Track if display needs redraw

    std::mt19937 rng{std::random_device{}()};

    Chip8() {
        const uint8_t font[80] = {
            0xF0,0x90,0x90,0x90,0xF0, 0x20,0x60,0x20,0x20,0x70,
            0xF0,0x10,0xF0,0x80,0xF0, 0xF0,0x10,0xF0,0x10,0xF0,
            0x90,0x90,0xF0,0x10,0x10, 0xF0,0x80,0xF0,0x10,0xF0,
            0xF0,0x80,0xF0,0x90,0xF0, 0xF0,0x10,0x20,0x40,0x40,
            0xF0,0x90,0xF0,0x90,0xF0, 0xF0,0x90,0xF0,0x10,0xF0,
            0xF0,0x90,0xF0,0x90,0x90, 0xE0,0x90,0xE0,0x90,0xE0,
            0xF0,0x80,0x80,0x80,0xF0, 0xE0,0x90,0x90,0x90,0xE0,
            0xF0,0x80,0xF0,0x80,0xF0, 0xF0,0x80,0xF0,0x80,0x80
        };
        std::memcpy(memory + 0x50, font, 80);
    }

    bool loadROM(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;

        file.seekg(0, std::ios::end);
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size > 4096 - 0x200) return false;
        file.read(reinterpret_cast<char*>(memory + 0x200), size);
        return true;
    }

    void emulateCycle() {
        uint16_t op = memory[pc] << 8 | memory[pc + 1];
        pc += 2;

        uint8_t x = (op & 0x0F00) >> 8;
        uint8_t y = (op & 0x00F0) >> 4;
        uint8_t n = op & 0x000F;
        uint8_t nn = op & 0x00FF;
        uint16_t nnn = op & 0x0FFF;

        switch (op & 0xF000) {
            case 0x0000:
                if (op == 0x00E0) {
                    // CLEAR display
                    std::memset(display, 0, sizeof(display));
                    display_changed = true;
                } else if (op == 0x00EE) {
                    // RETURN
                    sp--;
                    pc = stack[sp];
                }
                break;

            case 0x1000:
                // JUMP to nnn
                pc = nnn;
                break;

            case 0x2000:
                // CALL subroutine at nnn
                stack[sp] = pc;
                sp++;
                pc = nnn;
                break;

            case 0x3000:
                // SKIP if Vx == nn
                if (V[x] == nn) pc += 2;
                break;

            case 0x4000:
                // SKIP if Vx != nn
                if (V[x] != nn) pc += 2;
                break;

            case 0x5000:
                // SKIP if Vx == Vy
                if (V[x] == V[y]) pc += 2;
                break;

            case 0x6000:
                // SET Vx = nn
                V[x] = nn;
                break;

            case 0x7000:
                // ADD Vx += nn
                V[x] += nn;
                break;

            case 0x8000: {
                uint8_t vy = V[y];
                switch (n) {
                    case 0x0: V[x] = vy; break;                        // SET Vx = Vy
                    case 0x1: V[x] |= vy; break;                       // OR
                    case 0x2: V[x] &= vy; break;                       // AND
                    case 0x3: V[x] ^= vy; break;                       // XOR
                    case 0x4: {                                        // ADD Vx += Vy, set VF
                        uint16_t sum = V[x] + vy;
                        V[0xF] = (sum > 255) ? 1 : 0;
                        V[x] = sum & 0xFF;
                        break;
                    }
                    case 0x5: {                                        // SUB Vx -= Vy
                        V[0xF] = (V[x] > vy) ? 1 : 0;
                        V[x] -= vy;
                        break;
                    }
                    case 0x6: {                                        // SHR Vx >>= 1
                        V[0xF] = V[x] & 1;
                        V[x] >>= 1;
                        break;
                    }
                    case 0x7: {                                        // SUBN Vx = Vy - Vx
                        V[0xF] = (vy > V[x]) ? 1 : 0;
                        V[x] = vy - V[x];
                        break;
                    }
                    case 0xE: {                                        // SHL Vx <<= 1
                        V[0xF] = (V[x] >> 7) & 1;
                        V[x] <<= 1;
                        break;
                    }
                }
                break;
            }

            case 0x9000:
                // SKIP if Vx != Vy
                if (V[x] != V[y]) pc += 2;
                break;

            case 0xA000:
                // SET I = nnn
                I = nnn;
                break;

            case 0xB000:
                // JUMP to nnn + V0
                pc = nnn + V[0];
                break;

            case 0xC000:
                // SET Vx = random & nn
                V[x] = (rng() & 0xFF) & nn;
                break;

            case 0xD000: {
                // DRAW sprite at (Vx, Vy) with height n
                uint8_t vx = V[x];
                uint8_t vy = V[y];
                V[0xF] = 0;

                for (int row = 0; row < n; ++row) {
                    uint8_t sprite = memory[I + row];
                    for (int col = 0; col < 8; ++col) {
                        if (sprite & (0x80 >> col)) {
                            int px = (vx + col) & 63;  // Wrap at 64
                            int py = (vy + row) & 31;  // Wrap at 32
                            int idx = py * CHIP8_W + px;
                            if (display[idx]) V[0xF] = 1;
                            display[idx] ^= 1;
                            display_changed = true;
                        }
                    }
                }
                break;
            }

            case 0xE000: {
                if (nn == 0x9E) {
                    // SKIP if key Vx is pressed
                    if (keypad[V[x] & 0xF]) pc += 2;
                } else if (nn == 0xA1) {
                    // SKIP if key Vx is NOT pressed
                    if (!keypad[V[x] & 0xF]) pc += 2;
                }
                break;
            }

            case 0xF000: {
                switch (nn) {
                    case 0x07: V[x] = delay_timer; break;              // SET Vx = delay_timer
                    case 0x15: delay_timer = V[x]; break;              // SET delay_timer = Vx
                    case 0x18: sound_timer = V[x]; break;              // SET sound_timer = Vx
                    case 0x1E: I += V[x]; break;                       // ADD I += Vx
                    case 0x29: I = 0x50 + (V[x] & 0xF) * 5; break;    // SET I = font address
                    case 0x33: {                                       // BCD (Binary Coded Decimal)
                        uint8_t val = V[x];
                        memory[I] = val / 100;
                        memory[I + 1] = (val / 10) % 10;
                        memory[I + 2] = val % 10;
                        break;
                    }
                    case 0x55: {                                       // STORE V0...Vx to memory at I
                        for (uint8_t i = 0; i <= x; ++i)
                            memory[I + i] = V[i];
                        break;
                    }
                    case 0x65: {                                       // LOAD V0...Vx from memory at I
                        for (uint8_t i = 0; i <= x; ++i)
                            V[i] = memory[I + i];
                        break;
                    }
                }
                break;
            }
        }
    }

    void updateTimers() {
        if (delay_timer > 0) delay_timer--;
        if (sound_timer > 0) sound_timer--;
    }
};

int main(int argc, char** argv) {
    // Parse command-line arguments for debug flag
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-d" || std::string(argv[i]) == "--debug") {
            DEBUG_ENABLED = true;
            debug("=== CHIP-8 Emulator Starting (DEBUG MODE) ===");
            break;
        }
    }

    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " rom.ch8 [-d|--debug]\n";
        return 1;
    }

    debug("Loading ROM: " + std::string(argv[1]));

    Chip8 chip8;
    if (!chip8.loadROM(argv[1])) {
        std::cout << "Failed to load ROM\n";
        debug("Failed to load ROM: " + std::string(argv[1]));
        return 1;
    }
    debug("ROM loaded successfully");

    debug("Initializing SDL...");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_Window* window = SDL_CreateWindow("CHIP-8", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    debug("SDL initialized");

    // === MCP & AUDIO INIT ===
    debug("Initializing MCP23017...");
    if (initMCP()) {
        debug("MCP23017 initialized successfully");
    } else {
        debug("Warning: MCP23017 initialization failed");
    }
    
    debug("Initializing audio...");
    AudioState audio;
    SDL_AudioSpec want{};
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = audioCallback;
    want.userdata = &audio;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    SDL_PauseAudioDevice(dev, 0);

    bool quit = false;
    const int CPU_FREQ = 2000;
    const int FRAME_RATE = 60;
    const int CYCLES_PER_FRAME = CPU_FREQ / FRAME_RATE;
    
    debug("Entering main emulation loop. CPU freq: " + std::to_string(CPU_FREQ) + 
          " Hz, Frame rate: " + std::to_string(FRAME_RATE) + " FPS");

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
        }

        // === READ MCP BUTTONS ===
        uint16_t pressed = readMCPButtons();
        debug("MCP Buttons state: " + std::to_string(pressed));
        chip8.keypad[0x5] = (pressed & (1 << UP_A_PIN)) != 0;      // UP
        chip8.keypad[0x8] = (pressed & (1 << DOWN_A_PIN)) != 0;    // DOWN
        chip8.keypad[0x4] = (pressed & (1 << LEFT_A_PIN)) != 0;    // LEFT
        chip8.keypad[0x6] = (pressed & (1 << RIGHT_A_PIN)) != 0;   // RIGHT
        chip8.keypad[0xA] = (pressed & (1 << A_A_PIN)) != 0;       // A
        chip8.keypad[0xB] = (pressed & (1 << B_B_PIN)) != 0;       // B
        chip8.keypad[0x7] = (pressed & (1 << START_A_PIN)) != 0;   // START
        chip8.keypad[0xC] = (pressed & (1 << SELECT_A_PIN)) != 0;  // SELECT

        for (int i = 0; i < CYCLES_PER_FRAME; i++) chip8.emulateCycle();
        chip8.updateTimers();

        // Render
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_Rect pixel{0, 0, SCALE, SCALE};
        for (int y = 0; y < CHIP8_H; y++) {
            for (int x = 0; x < CHIP8_W; x++) {
                if (chip8.display[y * CHIP8_W + x]) {
                    pixel.x = OFFSET_X + x * SCALE;
                    pixel.y = OFFSET_Y + y * SCALE;
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
        SDL_RenderPresent(renderer);

        audio.playing = chip8.sound_timer > 0;
        SDL_Delay(1000 / FRAME_RATE);
    }

    debug("=== CHIP-8 Emulator Shutting Down ===");
    SDL_Quit();
    if (mcp_fd >= 0) close(mcp_fd);
    return 0;
}