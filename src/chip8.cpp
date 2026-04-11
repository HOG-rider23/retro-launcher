/**
 * chip8_sdl.cpp  —  CHIP-8 emulator with SDL2
 *
 * Display : 64 × 32 pixels, scaled up to a window
 * Audio   : SDL2 audio — buzzer beep on sound timer
 * Input   : CHIP-8 hex keypad mapped to keyboard (see table below)
 *
 * CHIP-8 key → PC keyboard
 *   1 2 3 C      →   1 2 3 4
 *   4 5 6 D      →   Q W E R
 *   7 8 9 E      →   A S D F
 *   A 0 B F      →   Z X C V
 *
 * Build:
 *   g++ -std=c++17 -Wall -O2 -o chip8 chip8_sdl.cpp \
 *       $(pkg-config --cflags --libs sdl2)
 *
 * Run:
 *   ./chip8 Space_Invaders__David_Winter_.ch8
 */

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

// ─── Config ──────────────────────────────────────────────────────────────────

static constexpr int   DISPLAY_W   = 64;
static constexpr int   DISPLAY_H   = 32;
static constexpr int   SCALE       = 12;          // window = 768 × 384
static constexpr int   WIN_W       = DISPLAY_W * SCALE;
static constexpr int   WIN_H       = DISPLAY_H * SCALE;
static constexpr int   FG_R = 255, FG_G = 255, FG_B = 255; // pixel ON  colour
static constexpr int   BG_R =   0, BG_G =   0, BG_B =   0; // pixel OFF colour

static constexpr int   CPU_HZ      = 700;   // instructions per second
static constexpr int   TIMER_HZ    = 60;    // delay/sound timers tick rate
static constexpr int   AUDIO_FREQ  = 44100;
static constexpr int   AUDIO_SAMPLES = 512;
static constexpr double BEEP_HZ    = 440.0; // buzzer pitch

// ─── Memory / ROM ────────────────────────────────────────────────────────────

static constexpr std::size_t MEM_SIZE     = 4096;
static constexpr uint16_t    ROM_START    = 0x200;
static constexpr std::size_t MAX_ROM_SIZE = MEM_SIZE - ROM_START;
static constexpr uint16_t    FONT_BASE    = 0x000;

static constexpr std::array<uint8_t, 80> FONT = {
    0xF0,0x90,0x90,0x90,0xF0, // 0
    0x20,0x60,0x20,0x20,0x70, // 1
    0xF0,0x10,0xF0,0x80,0xF0, // 2
    0xF0,0x10,0xF0,0x10,0xF0, // 3
    0x90,0x90,0xF0,0x10,0x10, // 4
    0xF0,0x80,0xF0,0x10,0xF0, // 5
    0xF0,0x80,0xF0,0x90,0xF0, // 6
    0xF0,0x10,0x20,0x40,0x40, // 7
    0xF0,0x90,0xF0,0x90,0xF0, // 8
    0xF0,0x90,0xF0,0x10,0xF0, // 9
    0xF0,0x90,0xF0,0x90,0x90, // A
    0xE0,0x90,0xE0,0x90,0xE0, // B
    0xF0,0x80,0x80,0x80,0xF0, // C
    0xE0,0x90,0x90,0x90,0xE0, // D
    0xF0,0x80,0xF0,0x80,0xF0, // E
    0xF0,0x80,0xF0,0x80,0x80, // F
};

// ─── Keymap: SDL scancode → CHIP-8 key (0-F) ─────────────────────────────────
//
//  CHIP-8 pad   Keyboard
//  1 2 3 C      1 2 3 4
//  4 5 6 D      Q W E R
//  7 8 9 E      A S D F
//  A 0 B F      Z X C V

static const std::array<std::pair<SDL_Scancode, uint8_t>, 16> KEY_MAP = {{
    { SDL_SCANCODE_X, 0x0 }, { SDL_SCANCODE_1, 0x1 },
    { SDL_SCANCODE_2, 0x2 }, { SDL_SCANCODE_3, 0x3 },
    { SDL_SCANCODE_Q, 0x4 }, { SDL_SCANCODE_W, 0x5 },
    { SDL_SCANCODE_E, 0x6 }, { SDL_SCANCODE_A, 0x7 },
    { SDL_SCANCODE_S, 0x8 }, { SDL_SCANCODE_D, 0x9 },
    { SDL_SCANCODE_Z, 0xA }, { SDL_SCANCODE_C, 0xB },
    { SDL_SCANCODE_4, 0xC }, { SDL_SCANCODE_R, 0xD },
    { SDL_SCANCODE_F, 0xE }, { SDL_SCANCODE_V, 0xF },
}};

// ─── CHIP-8 machine ───────────────────────────────────────────────────────────

struct Chip8 {
    std::array<uint8_t,  MEM_SIZE> mem{};
    std::array<uint8_t,  16>       V{};
    uint16_t                       I   = 0;
    uint16_t                       PC  = ROM_START;
    uint8_t                        SP  = 0;
    std::array<uint16_t, 16>       stack{};
    uint8_t                        DT  = 0;
    uint8_t                        ST  = 0;

    // 64 × 32 display; true = pixel on
    std::array<bool, DISPLAY_W * DISPLAY_H> display{};
    bool displayDirty = false;

    // Keypad state
    std::array<bool, 16> keys{};

    // Waiting-for-key state (opcode Fx0A)
    bool    waitKey   = false;
    uint8_t waitReg   = 0;

    std::mt19937 rng{ std::random_device{}() };

    Chip8() {
        std::copy(FONT.begin(), FONT.end(), mem.begin() + FONT_BASE);
    }

    void loadROM(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("Cannot open: " + path);
        auto sz = f.tellg();
        if (sz <= 0 || static_cast<std::size_t>(sz) > MAX_ROM_SIZE)
            throw std::runtime_error("ROM size invalid: " + std::to_string(sz));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(&mem[ROM_START]), sz);
    }

    // Tick delay/sound timers (call at 60 Hz)
    void tickTimers() {
        if (DT) --DT;
        if (ST) --ST;
    }

    // Execute one instruction
    void step() {
        if (waitKey) return;

        const uint16_t op = (mem[PC] << 8) | mem[PC + 1];
        PC += 2;

        const uint8_t  x   = (op >> 8) & 0xF;
        const uint8_t  y   = (op >> 4) & 0xF;
        const uint8_t  n   =  op       & 0xF;
        const uint8_t  kk  =  op       & 0xFF;
        const uint16_t nnn =  op       & 0xFFF;

        switch (op & 0xF000) {

        case 0x0000:
            if (op == 0x00E0) {                         // CLS
                display.fill(false);
                displayDirty = true;
            } else if (op == 0x00EE) {                  // RET
                PC = stack[--SP];
            }
            break;

        case 0x1000: PC = nnn; break;                   // JP nnn

        case 0x2000:                                    // CALL nnn
            stack[SP++] = PC;
            PC = nnn;
            break;

        case 0x3000: if (V[x] == kk) PC += 2; break;   // SE Vx, kk
        case 0x4000: if (V[x] != kk) PC += 2; break;   // SNE Vx, kk
        case 0x5000: if (V[x] == V[y]) PC += 2; break; // SE Vx, Vy
        case 0x6000: V[x] = kk; break;                 // LD Vx, kk
        case 0x7000: V[x] += kk; break;                // ADD Vx, kk

        case 0x8000:
            switch (n) {
            case 0x0: V[x]  = V[y]; break;
            case 0x1: V[x] |= V[y]; break;
            case 0x2: V[x] &= V[y]; break;
            case 0x3: V[x] ^= V[y]; break;
            case 0x4: { uint16_t r = V[x]+V[y]; V[0xF]=r>255; V[x]=r&0xFF; break; }
            case 0x5: { uint8_t f=V[x]>=V[y]; V[x]-=V[y]; V[0xF]=f; break; }
            case 0x6: { uint8_t f=V[x]&1; V[x]>>=1; V[0xF]=f; break; }
            case 0x7: { uint8_t f=V[y]>=V[x]; V[x]=V[y]-V[x]; V[0xF]=f; break; }
            case 0xE: { uint8_t f=V[x]>>7; V[x]<<=1; V[0xF]=f; break; }
            }
            break;

        case 0x9000: if (V[x] != V[y]) PC += 2; break; // SNE Vx, Vy
        case 0xA000: I = nnn; break;                   // LD I, nnn
        case 0xB000: PC = nnn + V[0]; break;           // JP V0, nnn
        case 0xC000: V[x] = (rng()&0xFF) & kk; break; // RND Vx, kk

        case 0xD000: {                                  // DRW Vx, Vy, n
            uint8_t vx = V[x] % DISPLAY_W;
            uint8_t vy = V[y] % DISPLAY_H;
            V[0xF] = 0;
            for (int row = 0; row < n; ++row) {
                uint8_t byte = mem[I + row];
                for (int col = 0; col < 8; ++col) {
                    if (byte & (0x80 >> col)) {
                        int px = (vx + col) % DISPLAY_W;
                        int py = (vy + row) % DISPLAY_H;
                        auto& pixel = display[py * DISPLAY_W + px];
                        if (pixel) V[0xF] = 1;
                        pixel ^= true;
                    }
                }
            }
            displayDirty = true;
            break;
        }

        case 0xE000:
            if (kk == 0x9E && keys[V[x]]) PC += 2;     // SKP Vx
            if (kk == 0xA1 && !keys[V[x]]) PC += 2;    // SKNP Vx
            break;

        case 0xF000:
            switch (kk) {
            case 0x07: V[x] = DT; break;
            case 0x0A:                                  // LD Vx, K (block)
                waitKey = true;
                waitReg = x;
                break;
            case 0x15: DT = V[x]; break;
            case 0x18: ST = V[x]; break;
            case 0x1E: I += V[x]; break;
            case 0x29: I = FONT_BASE + V[x] * 5; break;
            case 0x33:                                  // BCD
                mem[I]   =  V[x] / 100;
                mem[I+1] = (V[x] / 10) % 10;
                mem[I+2] =  V[x] % 10;
                break;
            case 0x55: for (int i=0;i<=x;++i) mem[I+i]=V[i]; break;
            case 0x65: for (int i=0;i<=x;++i) V[i]=mem[I+i]; break;
            }
            break;
        }
    }

    void keyDown(uint8_t key) {
        keys[key] = true;
        if (waitKey) {
            V[waitReg] = key;
            waitKey = false;
        }
    }

    void keyUp(uint8_t key) { keys[key] = false; }
};

// ─── Audio ────────────────────────────────────────────────────────────────────

struct Buzzer {
    SDL_AudioDeviceID dev = 0;
    double phase = 0.0;
    bool   beeping = false;

    static void audioCallback(void* userdata, uint8_t* stream, int len) {
        auto* b = static_cast<Buzzer*>(userdata);
        auto* out = reinterpret_cast<int16_t*>(stream);
        int samples = len / 2;
        for (int i = 0; i < samples; ++i) {
            out[i] = b->beeping
                ? static_cast<int16_t>(10000 * (b->phase < 0.5 ? 1 : -1))
                : 0;
            b->phase += BEEP_HZ / AUDIO_FREQ;
            if (b->phase >= 1.0) b->phase -= 1.0;
        }
    }

    void init() {
        SDL_AudioSpec want{}, got{};
        want.freq     = AUDIO_FREQ;
        want.format   = AUDIO_S16SYS;
        want.channels = 1;
        want.samples  = AUDIO_SAMPLES;
        want.callback = audioCallback;
        want.userdata = this;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (dev) SDL_PauseAudioDevice(dev, 0);
    }

    void setBeep(bool on) {
        if (dev) beeping = on;
    }

    ~Buzzer() { if (dev) SDL_CloseAudioDevice(dev); }
};

// ─── Renderer ─────────────────────────────────────────────────────────────────

struct Renderer {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;

    void init(const char* title) {
        window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            WIN_W, WIN_H, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());

        renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) throw std::runtime_error(SDL_GetError());

        texture = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
            DISPLAY_W, DISPLAY_H);
        if (!texture) throw std::runtime_error(SDL_GetError());
    }

    void draw(const std::array<bool, DISPLAY_W * DISPLAY_H>& display) {
        uint32_t pixels[DISPLAY_W * DISPLAY_H];
        for (int i = 0; i < DISPLAY_W * DISPLAY_H; ++i) {
            pixels[i] = display[i]
                ? (FG_R << 24 | FG_G << 16 | FG_B << 8 | 0xFF)
                : (BG_R << 24 | BG_G << 16 | BG_B << 8 | 0xFF);
        }
        SDL_UpdateTexture(texture, nullptr, pixels, DISPLAY_W * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    ~Renderer() {
        if (texture)  SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window)   SDL_DestroyWindow(window);
    }
};

// ─── Main loop ────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <rom.ch8>\n";
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL_Init: " << SDL_GetError() << '\n';
        return 1;
    }

    Chip8    chip8;
    Renderer ren;
    Buzzer   buzzer;

    try {
        chip8.loadROM(argv[1]);
        ren.init("CHIP-8  —  Space Invaders");
        buzzer.init();
    } catch (const std::exception& e) {
        std::cerr << "Init error: " << e.what() << '\n';
        SDL_Quit();
        return 1;
    }

    // Timing
    using clock     = std::chrono::high_resolution_clock;
    using us        = std::chrono::microseconds;
    const us cpuPeriod   { static_cast<long>(1'000'000.0 / CPU_HZ)   };
    const us timerPeriod { static_cast<long>(1'000'000.0 / TIMER_HZ) };

    auto lastCpu   = clock::now();
    auto lastTimer = clock::now();

    bool running = true;
    while (running) {
        // ── Events ──
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; break; }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                running = false;

            for (auto& [sc, key] : KEY_MAP) {
                if (ev.key.keysym.scancode == sc) {
                    if (ev.type == SDL_KEYDOWN) chip8.keyDown(key);
                    if (ev.type == SDL_KEYUP)   chip8.keyUp(key);
                }
            }
        }

        auto now = clock::now();

        // ── CPU steps ──
        if (now - lastCpu >= cpuPeriod) {
            chip8.step();
            lastCpu = now;
        }

        // ── Timer tick (60 Hz) ──
        if (now - lastTimer >= timerPeriod) {
            chip8.tickTimers();
            buzzer.setBeep(chip8.ST > 0);
            lastTimer = now;
        }

        // ── Render if dirty ──
        if (chip8.displayDirty) {
            ren.draw(chip8.display);
            chip8.displayDirty = false;
        }

        // Small sleep to avoid burning 100% CPU
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    SDL_Quit();
    return 0;
}