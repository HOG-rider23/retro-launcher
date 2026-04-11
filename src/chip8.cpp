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

    debug("MCP23017 initialized");
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
    uint8_t  memory[4096]{};
    uint8_t  V[16]{};
    uint16_t I = 0;
    uint16_t pc = 0x200;

    uint16_t stack[16]{};
    uint8_t  sp = 0;

    uint8_t delay_timer = 0;
    uint8_t sound_timer = 0;

    uint32_t display[128 * 64]{};   // always 128x64 buffer

    bool highRes = false;           // SuperCHIP high-res flag
    unsigned int currentW = 64;
    unsigned int currentH = 32;
    unsigned int currentScale = 10;
    
    bool drawFlag = true;

    bool keypad[16]{};

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
        highRes = false;
        currentW = 64;
        currentH = 32;
        currentScale = 10;
        std::memset(display, 0, sizeof(display));

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
        if (pc >= 4094) { pc = 0x200; return; }

        uint16_t op  = memory[pc] << 8 | memory[pc + 1];
        uint8_t  x   = (op & 0x0F00) >> 8;
        uint8_t  y   = (op & 0x00F0) >> 4;
        uint8_t  n   = op & 0x000F;
        uint8_t  nn  = op & 0x00FF;
        uint16_t nnn = op & 0x0FFF;

        pc += 2;

        switch (op & 0xF000) {
            case 0x0000:
                if (nn == 0xE0) {
                    std::memset(display, 0, sizeof(display));
                    drawFlag = true;
                }
                else if (nn == 0xEE) {
                    if (sp > 0) pc = stack[--sp];
                }
                else if (nn == 0xFE) {          // low-res
                    highRes = false;
                    currentW = 64;
                    currentH = 32;
                    currentScale = 10;
                    std::memset(display, 0, sizeof(display));
                    drawFlag = true;
                }
                else if (nn == 0xFF) {          // high-res
                    highRes = true;
                    currentW = 128;
                    currentH = 64;
                    currentScale = 5;
                    std::memset(display, 0, sizeof(display));
                    drawFlag = true;
                }
                break;
            case 0x1000: pc = nnn; break;
            case 0x2000:
                if (sp < 16) stack[sp++] = pc;
                pc = nnn;
                break;
            case 0x3000: if (V[x] == nn) pc += 2; break;
            case 0x4000: if (V[x] != nn) pc += 2; break;
            case 0x5000: if (V[x] == V[y]) pc += 2; break;
            case 0x6000: V[x] = nn; break;
            case 0x7000: V[x] += nn; break;
            case 0x8000:
                switch (n) {
                    case 0x0: V[x]  = V[y]; break;
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
            case 0x9000: if (V[x] != V[y]) pc += 2; break;
            case 0xA000: I = nnn; break;
            case 0xB000: pc = nnn + V[0]; break;
            case 0xC000: V[x] = (rng() & 0xFF) & nn; break;
            case 0xD000: {
                uint8_t vx = V[x] % currentW;
                uint8_t vy = V[y] % currentH;
                V[0xF] = 0;

                for (uint8_t row = 0; row < n; ++row) {
                    if (vy + row >= currentH) break;
                    uint8_t sprite = memory[I + row];
                    for (uint8_t col = 0; col < 8; ++col) {
                        if (vx + col >= currentW) break;
                        if (sprite & (0x80 >> col)) {
                            size_t idx = (vy + row) * 128 + (vx + col);  // always 128-wide
                            if (display[idx] == 0xFFFFFFFF) V[0xF] = 1;
                            display[idx] ^= 0xFFFFFFFF;
                        }
                    }
                }
                drawFlag = true;
                break;
            }
            case 0xE000:
                if      (nn == 0x9E &&  keypad[V[x] & 0xF]) pc += 2;
                else if (nn == 0xA1 && !keypad[V[x] & 0xF]) pc += 2;
                break;
            case 0xF000:
                switch (nn) {
                    case 0x07: V[x] = delay_timer; break;
                    case 0x0A: {
                        bool pressed = false;
                        for (int k = 0; k < 16; ++k) {
                            if (keypad[k]) { V[x] = k; pressed = true; break; }
                        }
                        if (!pressed) pc -= 2;
                        break;
                    }
                    case 0x15: delay_timer = V[x]; break;
                    case 0x18: sound_timer = V[x]; break;
                    case 0x1E: I += V[x]; break;
                    case 0x29: I = 0x50 + (V[x] & 0xF) * 5; break;
                    case 0x33:
                        memory[I]   = V[x] / 100;
                        memory[I+1] = (V[x] / 10) % 10;
                        memory[I+2] = V[x] % 10;
                        break;
                    case 0x55: for (int i = 0; i <= x; ++i) memory[I + i] = V[i]; break;
                    case 0x65: for (int i = 0; i <= x; ++i) V[i] = memory[I + i]; break;
                }
                break;
            case 0x75:  // Fx75 - Save V0..Vx to memory[I..]
                for (int i = 0; i <= x; ++i)
                    memory[I + i] = V[i];
                break;
            case 0x85:  // Fx85 - Load V0..Vx from memory[I..]
                for (int i = 0; i <= x; ++i)
                    V[i] = memory[I + i];
                break;
        }
    }

    
    void updateTimers() {
        if (delay_timer > 0) delay_timer--;
        if (sound_timer > 0) sound_timer--;
    }

    void handleKey(uint8_t key, bool pressed) {
        if (key < 16) {
            keypad[key] = pressed;
            debug("handleKey: keypad[" + std::to_string(key) + "] = " + std::to_string(pressed));
        }
    }
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-d" || std::string(argv[i]) == "--debug") {
            DEBUG_ENABLED = true;
            debug("=== CHIP-8 Emulator Starting (DEBUG MODE) ===");
            break;
        }
    }

    if (argc < 2) {
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
    SDL_Window* window = SDL_CreateWindow(
        "CHIP-8",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

   
    // === HIGH-RES COMPATIBLE TEXTURE (always 128x64) ===
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        128,          // maximum width (SuperCHIP)
        64            // maximum height (SuperCHIP)
    );

    debug("SDL texture created (128x64 for high-res support)");

    debug("SDL initialized");

    debug("Initializing MCP23017...");
    if (initMCP()) {
        debug("MCP23017 initialized successfully");
    } else {
        debug("Warning: MCP23017 initialization failed");
    }

    debug("Initializing audio...");
    AudioState audio;
    SDL_AudioSpec want{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    want.callback = audioCallback;
    want.userdata = &audio;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    SDL_PauseAudioDevice(dev, 0);

    const int CPU_FREQ        = 600;   // cycles per second (adjust per game)
    const int FRAME_RATE      = 60;
    const int CYCLES_PER_FRAME = CPU_FREQ / FRAME_RATE;

   
    auto lastFrameTime = std::chrono::high_resolution_clock::now();

   
    uint16_t prev_pressed = 0;

    bool quit = false;

    debug("Entering main emulation loop. CPU freq: " + std::to_string(CPU_FREQ) +
          " Hz, Frame rate: " + std::to_string(FRAME_RATE) + " FPS");

    while (!quit) {
        // --- SDL events (window close, ESC) ---
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
        }

        // --- MCP button reading with edge detection ---
        uint16_t pressed = readMCPButtons();

        uint16_t just_pressed  = pressed  & ~prev_pressed;   // newly pressed this frame
        uint16_t just_released = prev_pressed & ~pressed;     // newly released this frame

        auto applyEdge = [&](uint16_t mask, uint8_t chip8key, bool state) {
            if (mask) chip8.handleKey(chip8key, state);
        };

        // Button mask → CHIP-8 key mappings (same as before)
        // Press events
        if (just_pressed  & 3)    chip8.handleKey(0x1, true);   // UP
        if (just_pressed  & 5)    chip8.handleKey(0x4, true);   // DOWN
        if (just_pressed  & 9)    chip8.handleKey(0x4, true);   // LEFT → DOWN
        if (just_pressed  & 17)   chip8.handleKey(0x6, true);   // RIGHT
        if (just_pressed  & 129)  chip8.handleKey(0x0, true);   // A
        if (just_pressed  & 2049) chip8.handleKey(0x9, true);   // B
        if (just_pressed  & 33)   chip8.handleKey(0x7, true);   // START
        if (just_pressed  & 65)   chip8.handleKey(0xC, true);   // SELECT

        // Release events
        if (just_released & 3)    chip8.handleKey(0x1, false);  // UP
        if (just_released & 5)    chip8.handleKey(0x4, false);  // DOWN
        if (just_released & 9)    chip8.handleKey(0x4, false);  // LEFT
        if (just_released & 17)   chip8.handleKey(0x6, false);  // RIGHT
        if (just_released & 129)  chip8.handleKey(0x0, false);  // A
        if (just_released & 2049) chip8.handleKey(0x9, false);  // B
        if (just_released & 33)   chip8.handleKey(0x7, false);  // START
        if (just_released & 65)   chip8.handleKey(0xC, false);  // SELECT

        prev_pressed = pressed;

        debug("MCP raw state: " + std::to_string(pressed));

        // --- Frame timing ---
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float, std::chrono::milliseconds::period>(now - lastFrameTime).count();

        if (dt >= 1000.0f / FRAME_RATE) {
            lastFrameTime = now;

            // --- CPU cycles ---
            for (int i = 0; i < CYCLES_PER_FRAME; i++) {
                chip8.emulateCycle();
            }

           
            chip8.updateTimers();

           
            if (chip8.drawFlag) {
                chip8.drawFlag = false;

                // Dynamic resolution for low-res (64x32) and high-res (128x64)
                unsigned int drawW     = chip8.highRes ? 128 : 64;
                unsigned int drawH     = chip8.highRes ? 64  : 32;
                unsigned int drawScale = chip8.highRes ? 5   : 10;

                // Center vertically on the 640x480 screen
                unsigned int offsetY = (SCREEN_H - drawH * drawScale) / 2;

                SDL_Rect destRect = {
                    (int)OFFSET_X,                    // left border (usually 0)
                    (int)offsetY,
                    (int)(drawW * drawScale),
                    (int)(drawH * drawScale)
                };

                // Update the full 128-wide buffer (stride is always 128)
                SDL_UpdateTexture(texture, nullptr, chip8.display, 128 * sizeof(uint32_t));

                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, &destRect);
                SDL_RenderPresent(renderer);
            }

            // --- Audio ---
            audio.playing = chip8.sound_timer > 0;
        }
    }

    debug("=== CHIP-8 Emulator Shutting Down ===");
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    if (mcp_fd >= 0) close(mcp_fd);
    return 0;
}
