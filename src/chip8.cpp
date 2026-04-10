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

const uint8_t UP_A_PIN     = 1;
const uint8_t DOWN_A_PIN   = 2;
const uint8_t LEFT_A_PIN   = 3;
const uint8_t RIGHT_A_PIN  = 4;
const uint8_t START_A_PIN  = 5;
const uint8_t SELECT_A_PIN = 6;
const uint8_t A_A_PIN      = 7;

const uint8_t X_B_PIN     = 1;
const uint8_t Y_B_PIN     = 2;
const uint8_t B_B_PIN     = 3;

const uint8_t IODIRA = 0x00;
const uint8_t IODIRB = 0x01;
const uint8_t GPIOA  = 0x12;
const uint8_t GPIOB  = 0x13;
const uint8_t GPPUA  = 0x0C;
const uint8_t GPPUB  = 0x0D;

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
        close(mcp_fd); mcp_fd = -1;
        debug("Failed to set I2C address");
        return false;
    }

    uint8_t cfg[2];
    cfg[0] = IODIRA; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);
    cfg[0] = IODIRB; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);
    cfg[0] = GPPUA;  cfg[1] = 0xFF; write(mcp_fd, cfg, 2);
    cfg[0] = GPPUB;  cfg[1] = 0xFF; write(mcp_fd, cfg, 2);

    debug("MCP23017 initialized (B button on B3)");
    return true;
}

uint16_t readMCPButtons() {
    if (mcp_fd < 0) return 0xFFFF;

    uint8_t reg = GPIOA;
    if (write(mcp_fd, &reg, 1) != 1) return 0xFFFF;

    uint8_t data[2] = {0};
    if (read(mcp_fd, data, 2) != 2) return 0xFFFF;

    debug("Raw GPIOA=0x" + std::to_string(data[0]) + "  GPIOB=0x" + std::to_string(data[1]));

    uint16_t raw = (static_cast<uint16_t>(data[1]) << 8) | data[0];
    return (~raw) & 0xFFFF;   // active-low
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

        // === NEW: Large 8×10 SCHIP font (required by David Winter's ROM) ===
        const uint8_t largeFont[16][10] = {
            {0xF0,0x90,0x90,0x90,0xF0,0x00,0x00,0x00,0x00,0x00}, // 0
            {0x20,0x60,0x20,0x20,0x70,0x00,0x00,0x00,0x00,0x00}, // 1
            {0xF0,0x10,0xF0,0x80,0xF0,0x00,0x00,0x00,0x00,0x00}, // 2
            {0xF0,0x10,0xF0,0x10,0xF0,0x00,0x00,0x00,0x00,0x00}, // 3
            {0x90,0x90,0xF0,0x10,0x10,0x00,0x00,0x00,0x00,0x00}, // 4
            {0xF0,0x80,0xF0,0x10,0xF0,0x00,0x00,0x00,0x00,0x00}, // 5
            {0xF0,0x80,0xF0,0x90,0xF0,0x00,0x00,0x00,0x00,0x00}, // 6
            {0xF0,0x10,0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00}, // 7
            {0xF0,0x90,0xF0,0x90,0xF0,0x00,0x00,0x00,0x00,0x00}, // 8
            {0xF0,0x90,0xF0,0x10,0xF0,0x00,0x00,0x00,0x00,0x00}, // 9
            {0xF0,0x90,0xF0,0x90,0x90,0x00,0x00,0x00,0x00,0x00}, // A
            {0xE0,0x90,0xE0,0x90,0xE0,0x00,0x00,0x00,0x00,0x00}, // B
            {0xF0,0x80,0x80,0x80,0xF0,0x00,0x00,0x00,0x00,0x00}, // C
            {0xE0,0x90,0x90,0x90,0xE0,0x00,0x00,0x00,0x00,0x00}, // D
            {0xF0,0x80,0xF0,0x80,0xF0,0x00,0x00,0x00,0x00,0x00}, // E
            {0xF0,0x80,0xF0,0x80,0x80,0x00,0x00,0x00,0x00,0x00}  // F
        };

        for (int i = 0; i < 16; ++i) {
            for (int j = 0; j < 10; ++j) {
                memory[0x100 + i * 10 + j] = largeFont[i][j];
            }
        }
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
        debug("--- emulateCycle() start --- pc = 0x" + std::to_string(pc));

        if (pc >= 4094) {
            debug("PC overflow - resetting to 0x200");
            pc = 0x200;
            return;
        }

        uint16_t op  = memory[pc] << 8 | memory[pc + 1];
        uint8_t  x   = (op & 0x0F00) >> 8;
        uint8_t  y   = (op & 0x00F0) >> 4;
        uint8_t  n   = op & 0x000F;
        uint8_t  nn  = op & 0x00FF;
        uint16_t nnn = op & 0x0FFF;

        char buf[64];
        snprintf(buf, sizeof(buf), "Fetched opcode: 0x%04X at pc=0x%04X", op, pc - 2);  // pc already +=2
        debug(buf);

        pc += 2;

        switch (op & 0xF000) {
            case 0x0000:
                debug("0x0000 group");
                if (nn == 0xE0) {
                    debug("00E0 - Clear screen");
                    std::memset(display, 0, sizeof(display));
                } else if (nn == 0xEE) {
                    debug("00EE - Return from subroutine");
                    if (sp > 0) pc = stack[--sp];
                }
                break;

            case 0x1000:
                debug("1nnn - Jump to 0x" + std::to_string(nnn));
                pc = nnn;
                break;

            case 0x2000:
                debug("2nnn - Call subroutine at 0x" + std::to_string(nnn));
                if (sp < 16) stack[sp++] = pc;
                pc = nnn;
                break;

            case 0x3000:
                debug("3xnn - Skip if V[" + std::to_string(x) + "] == 0x" + std::to_string(nn));
                if (V[x] == nn) pc += 2;
                break;

            case 0x4000:
                debug("4xnn - Skip if V[" + std::to_string(x) + "] != 0x" + std::to_string(nn));
                if (V[x] != nn) pc += 2;
                break;

            case 0x5000:
                debug("5xy0 - Skip if V[" + std::to_string(x) + "] == V[" + std::to_string(y) + "]");
                if (V[x] == V[y]) pc += 2;
                break;

            case 0x6000:
                debug("6xnn - V[" + std::to_string(x) + "] = 0x" + std::to_string(nn));
                V[x] = nn;
                break;

            case 0x7000:
                debug("7xnn - V[" + std::to_string(x) + "] += 0x" + std::to_string(nn));
                V[x] += nn;
                break;

            case 0x8000:
                debug("8xyN group (n=" + std::to_string(n) + ")");
                switch (n) {
                    case 0x0: V[x] = V[y]; break;
                    case 0x1: V[x] |= V[y]; V[0xF] = 0; break;
                    case 0x2: V[x] &= V[y]; V[0xF] = 0; break;
                    case 0x3: V[x] ^= V[y]; V[0xF] = 0; break;
                    case 0x4: { uint16_t r = V[x]+V[y]; V[x]=r&0xFF; V[0xF]=(r>255); break; }
                    case 0x5: { uint8_t f=(V[x]>=V[y]); V[x]-=V[y]; V[0xF]=f; break; }
                    case 0x6: { uint8_t f=V[x]&1; V[x]>>=1; V[0xF]=f; break; }
                    case 0x7: { uint8_t f=(V[y]>=V[x]); V[x]=V[y]-V[x]; V[0xF]=f; break; }
                    case 0xE: { uint8_t f=(V[x]&0x80)>>7; V[x]<<=1; V[0xF]=f; break; }
                }
                break;

            case 0x9000:
                debug("9xy0 - Skip if V[" + std::to_string(x) + "] != V[" + std::to_string(y) + "]");
                if (V[x] != V[y]) pc += 2;
                break;

            case 0xA000:
                debug("Annn - Set I = 0x" + std::to_string(nnn));
                I = nnn;
                break;

            case 0xB000:
                debug("Bnnn - Jump to 0x" + std::to_string(nnn) + " + V[0]");
                pc = nnn + V[0];
                break;

            case 0xC000:
                debug("Cxnn - V[" + std::to_string(x) + "] = random & 0x" + std::to_string(nn));
                V[x] = (rng() & 0xFF) & nn;
                break;

            case 0xD000:
                debug("Dxyn - Draw sprite at Vx=" + std::to_string(V[x]) + ", Vy=" + std::to_string(V[y]) + ", height=" + std::to_string(n));
                debug("Dxyn - Draw sprite height=" + std::to_string(n) + " at Vx=" + std::to_string(V[x]) + ", Vy=" + std::to_string(V[y]) + ", I=0x" + std::to_string(I));
                {
                    uint8_t vx = V[x] % CHIP8_W;
                    uint8_t vy = V[y] % CHIP8_H;
                    V[0xF] = 0;
                    for (uint8_t row = 0; row < n; ++row) {
                        if (vy + row >= CHIP8_H) break;
                        uint8_t sprite = memory[I + row];
                        debug("  Sprite row=" + std::to_string(row) + " byte=0x" + std::to_string(sprite));
                        for (uint8_t col = 0; col < 8; ++col) {
                            if (vx + col >= CHIP8_W) break;
                            if (sprite & (0x80 >> col)) {
                                size_t idx = (vy + row) * CHIP8_W + (vx + col);
                                if (display[idx]) V[0xF] = 1;
                                display[idx] ^= true;
                            }
                        }
                    }
                }
                break;

            case 0xE000:
                debug("ExNN - Key test for V[" + std::to_string(x) + "]");
                debug("   Key test: V[" + std::to_string(x) + "] = " + std::to_string(V[x]) + 
                    " → checking keypad[" + std::to_string(V[x] & 0xF) + "] = " + 
                    std::to_string(keypad[V[x] & 0xF]));
                if (nn == 0x9E &&  keypad[V[x] & 0xF]) pc += 2;
                else if (nn == 0xA1 && !keypad[V[x] & 0xF]) pc += 2;
                break;

            case 0xF000:
                debug("FxNN - x=" + std::to_string(x) + ", nn=0x" + std::to_string(nn));
                switch (nn) {
                    case 0x07: V[x] = delay_timer; break;
                    case 0x0A: {   // Fx0A - Wait for any key
                        debug("Fx0A - Waiting for any key press...");
                        bool any_key_pressed = false;
                        for (int k = 0; k < 16; ++k) {
                            if (keypad[k]) {
                                V[x] = k;
                                any_key_pressed = true;
                                debug("Fx0A - Key pressed: " + std::to_string(k));
                                break;
                            }
                        }
                        if (!any_key_pressed) {
                            debug("Fx0A - No key yet - re-executing instruction");
                            pc -= 2;
                        }
                        break;
                    }
                    case 0x15: delay_timer = V[x]; break;
                    case 0x18: sound_timer = V[x]; break;
                    case 0x1E: I += V[x]; break;
                    case 0x29: I = 0x50 + (V[x] & 0xF) * 5; break;
                    case 0x30:  // SCHIP Fx30 - large 10-byte font sprite for digit in Vx
                        I = 0x100 + (V[x] & 0xF) * 10;
                        debug(">>> Fx30 EXECUTED! I=0x" + std::to_string(I) + " (digit " + std::to_string(V[x]&0xF) + ")");
                        break;
                    case 0x33:
                        memory[I]   = V[x] / 100;
                        memory[I+1] = (V[x] / 10) % 10;
                        memory[I+2] = V[x] % 10;
                        break;
                    case 0x55: for (int i = 0; i <= x; ++i) memory[I + i] = V[i]; break;
                    case 0x65: for (int i = 0; i <= x; ++i) V[i] = memory[I + i]; break;
                }
                break;
        }

        // <<< IMPORTANT: Update timers EVERY cycle for tight loops like Space Invaders >>>
        updateTimers();

        debug("--- emulateCycle() end --- new pc = 0x" + std::to_string(pc));
    }   

    void updateTimers() {
        if (delay_timer > 0) delay_timer--;
        if (sound_timer > 0) sound_timer--;
    }

    void keypadReset()
    {
        keypad[0x1] = false;  // UP
        keypad[0x4] = false;  // DOWN
        keypad[0x6] = false;  // RIGHT
        keypad[0x0] = false;  // A
        keypad[0x9] = false;  // B
        keypad[0x7] = false;  // START
        keypad[0xC] = false;  // SELECT
    }

    void handleKey(uint8_t key, bool pressed)
    {    
        if (key < 16)
        {
            keypad[key] = pressed;
            debug("handleKey: keypad[" + std::to_string(key) + "] = " + std::to_string(pressed));
        }
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

        // === READ MCP BUTTONS WITH EDGE DETECTION ===
        static uint16_t last_pressed = 0xFFFF;
        static Uint32 last_press_time = 0;

        uint16_t pressed = readMCPButtons();

        Uint32 now = SDL_GetTicks();
        if (now - last_press_time > 80) {   // 80ms debounce
            chip8.keypadReset();  // Reset all keys before setting the current state
            if (pressed == 3) chip8.handleKey(0x1, true);  // UP    → 0x1
            if (pressed == 5) chip8.handleKey(0x4, true);  // DOWN  → 0x4
            if (pressed == 9) chip8.handleKey(0x4, true);  // LEFT  → 0x4
            if (pressed == 17) chip8.handleKey(0x6, true);  // RIGHT → 0x6
            if (pressed == 129) chip8.handleKey(0x0, true);  // A
            if (pressed == 2049) chip8.handleKey(0x9, true);  // B
            if (pressed == 33) chip8.handleKey(0x7, true);  // START
            if (pressed == 65) chip8.handleKey(0xC, true);  // SELECT
            last_pressed = pressed;
            last_press_time = now;
            debug("MCP Buttons state: " + std::to_string(pressed) + " last press time: " + std::to_string(last_press_time));
        }

        // Debug keypad state
        debug("Keypad stanje:");
        debug("------------------------------------------");
        debug("UP     keypad[0x1]: " + std::to_string(chip8.keypad[0x1]));
        debug("DOWN   keypad[0x4]: " + std::to_string(chip8.keypad[0x4]));
        debug("RIGHT  keypad[0x6]: " + std::to_string(chip8.keypad[0x6]));
        debug("A      keypad[0x0]: " + std::to_string(chip8.keypad[0x0]));
        debug("B      keypad[0x9]: " + std::to_string(chip8.keypad[0x9]));
        debug("START  keypad[0x7]: " + std::to_string(chip8.keypad[0x7]));
        debug("SELECT keypad[0xC]: " + std::to_string(chip8.keypad[0xC]));
        debug("------------------------------------------");
        debug("MCP Buttons state: " + std::to_string(pressed));

        for (int i = 0; i < CYCLES_PER_FRAME; i++) chip8.emulateCycle();
        // chip8.updateTimers();

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