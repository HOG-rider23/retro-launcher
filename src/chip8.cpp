#include <SDL2/SDL.h>
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <cstring>
#include <string>

const unsigned int CHIP8_W = 64;
const unsigned int CHIP8_H = 32;
unsigned int SCALE    = 5;
unsigned int SCREEN_W = 320;
unsigned int SCREEN_H = 240;
unsigned int OFFSET_X = 0;
unsigned int OFFSET_Y = 0;

// --- Color themes (bg R,G,B / fg R,G,B) ---
struct Theme { uint8_t bgR,bgG,bgB, fgR,fgG,fgB; };
const Theme THEMES[] = {
    {  0,  0,  0, 255,255,255 }, // 0: Classic white on black
    {  0,  0,  0,   0,255,128 }, // 1: Green phosphor
    {  0,  0,  0, 255,176,  0 }, // 2: Amber
    { 10, 10, 40, 100,180,255 }, // 3: Blue LCD
};
const int NUM_THEMES = 4;
int currentTheme = 0;

// --- Audio ---
const int SAMPLE_RATE = 44100;
const int AMPLITUDE   = 6000;
const int BEEP_FREQ   = 440;

struct AudioState {
    bool  playing = false;
    bool  enabled = true;
    float phase   = 0.0f;
};

void audioCallback(void* userdata, Uint8* stream, int len) {
    AudioState* audio = (AudioState*)userdata;
    Sint16* buf = (Sint16*)stream;
    int samples = len / 2;
    if (!audio->playing || !audio->enabled) {
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

class Chip8 {
public:
    uint8_t  memory[4096]{};
    uint8_t  V[16]{};
    uint16_t I  = 0;
    uint16_t pc = 0x200;
    uint16_t stack[16]{};
    uint8_t  sp = 0;

    uint8_t delay_timer = 0;
    uint8_t sound_timer = 0;

    bool display[CHIP8_W * CHIP8_H]{};
    bool displayPrev[CHIP8_W * CHIP8_H]{}; // ghost frame for flicker reduction
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

    void snapshotDisplay() {
        std::memcpy(displayPrev, display, sizeof(display));
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
                if (nn == 0xE0) std::memset(display, 0, sizeof(display));
                else if (nn == 0xEE) { if (sp > 0) pc = stack[--sp]; }
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
                uint8_t vx = V[x] % CHIP8_W;
                uint8_t vy = V[y] % CHIP8_H;
                V[0xF] = 0;
                for (uint8_t row = 0; row < n; ++row) {
                    if (vy + row >= CHIP8_H) break;
                    uint8_t sprite = memory[I + row];
                    for (uint8_t col = 0; col < 8; ++col) {
                        if (vx + col >= CHIP8_W) break;
                        if (sprite & (0x80 >> col)) {
                            size_t idx = (vy + row) * CHIP8_W + (vx + col);
                            if (display[idx]) V[0xF] = 1;
                            display[idx] ^= true;
                        }
                    }
                }
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
        }
    }
};

void handleKey(Chip8& chip8, SDL_Scancode sc, bool down,
               bool& quit, bool& paused, AudioState& audio) {
    switch (sc) {
        // Pong: paddle up/down
        case SDL_SCANCODE_UP:        chip8.keypad[0x1] = down; break;
        case SDL_SCANCODE_DOWN:      chip8.keypad[0x4] = down; break;
        // Space Invaders: move left/right
        case SDL_SCANCODE_LEFT:      chip8.keypad[0x4] = down; break;
        case SDL_SCANCODE_RIGHT:     chip8.keypad[0x6] = down; break;
        // A: shoot / confirm
        case SDL_SCANCODE_RETURN:    chip8.keypad[0x5] = down; break;
        // B: Space Invaders coin/start
        case SDL_SCANCODE_BACKSPACE: chip8.keypad[0x9] = down; break;
        // SELECT: pause toggle
        case SDL_SCANCODE_TAB:       if (down) paused = !paused; break;
        // START: quit
        case SDL_SCANCODE_ESCAPE:    if (down) quit = true; break;
        default: break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rom.ch8> [cpu_hz] [theme 0-3]\n";
        std::cerr << "  cpu_hz  : CPU speed in Hz (default 500, Pong=500, SpaceInvaders=600)\n";
        std::cerr << "  theme   : 0=white 1=green 2=amber 3=blue (default 0)\n";
        return 1;
    }

    double CPU_HZ = 500.0;
    if (argc >= 3) CPU_HZ = std::stod(argv[2]);
    if (argc >= 4) currentTheme = std::stoi(argv[3]) % NUM_THEMES;

    Chip8 chip8;
    if (!chip8.loadROM(argv[1])) {
        std::cerr << "Failed to load ROM: " << argv[1] << "\n";
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // --- Audio ---
    AudioState audioState;
    SDL_AudioSpec want{}, have{};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    want.callback = audioCallback;
    want.userdata = &audioState;

    SDL_AudioDeviceID audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDev == 0)
        std::cerr << "Audio warning: " << SDL_GetError() << "\n";
    else
        SDL_PauseAudioDevice(audioDev, 0);

    // Detect screen resolution
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        SCREEN_W = (unsigned int)dm.w;
        SCREEN_H = (unsigned int)dm.h;
    }
    unsigned int scaleX = SCREEN_W / CHIP8_W;
    unsigned int scaleY = SCREEN_H / CHIP8_H;
    SCALE    = (scaleX < scaleY) ? scaleX : scaleY;
    OFFSET_X = (SCREEN_W - CHIP8_W * SCALE) / 2;
    OFFSET_Y = (SCREEN_H - CHIP8_H * SCALE) / 2;

    // ROM name for window title
    std::string romName = argv[1];
    size_t slash = romName.find_last_of("/\\");
    if (slash != std::string::npos) romName = romName.substr(slash + 1);

    SDL_Window* win = SDL_CreateWindow(
        ("CHIP-8 | " + romName).c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        0, 0,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
    );
    if (!win) {
        std::cerr << "Window failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::cerr << "Renderer failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // Initial black frame
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    bool quit   = false;
    bool paused = false;

    const double TIMER_HZ = 60.0;
    const double CPU_DT   = 1.0 / CPU_HZ;
    const double TIMER_DT = 1.0 / TIMER_HZ;

    auto   lastTime = std::chrono::steady_clock::now();
    double cpuAcc   = 0.0;
    double timerAcc = 0.0;

    while (!quit) {
        auto   now = std::chrono::steady_clock::now();
        double dt  = std::chrono::duration<double>(now - lastTime).count();
        lastTime   = now;

        // Cap dt to prevent burst after lag spike
        if (dt > 0.05) dt = 0.05;

        cpuAcc   += dt;
        timerAcc += dt;

        // --- Events ---
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_KEYDOWN)
                handleKey(chip8, e.key.keysym.scancode, true,  quit, paused, audioState);
            if (e.type == SDL_KEYUP)
                handleKey(chip8, e.key.keysym.scancode, false, quit, paused, audioState);
        }

        // --- CPU cycles ---
        if (!paused) {
            while (cpuAcc >= CPU_DT) {
                chip8.emulateCycle();
                cpuAcc -= CPU_DT;
            }
        } else {
            cpuAcc = 0.0; // drain so we don't burst on unpause
        }

        // --- Timers + render at 60fps ---
        if (timerAcc >= TIMER_DT) {
            timerAcc -= TIMER_DT;

            if (!paused) {
                if (chip8.delay_timer > 0) --chip8.delay_timer;
                if (chip8.sound_timer > 0) --chip8.sound_timer;
            }

            // Snapshot for ghost pixels
            chip8.snapshotDisplay();

            const Theme& t = THEMES[currentTheme];
            SDL_SetRenderDrawColor(ren, t.bgR, t.bgG, t.bgB, 255);
            SDL_RenderClear(ren);

            SDL_Rect pixel{0, 0, (int)SCALE, (int)SCALE};

            for (unsigned int py = 0; py < CHIP8_H; ++py) {
                for (unsigned int px = 0; px < CHIP8_W; ++px) {
                    size_t idx = py * CHIP8_W + px;
                    bool cur  = chip8.display[idx];
                    bool prev = chip8.displayPrev[idx];

                    if (cur) {
                        // Full brightness
                        SDL_SetRenderDrawColor(ren, t.fgR, t.fgG, t.fgB, 255);
                    } else if (prev) {
                        // Ghost — dim version of fg color to reduce flicker
                        SDL_SetRenderDrawColor(ren, t.fgR/3, t.fgG/3, t.fgB/3, 255);
                    } else {
                        continue;
                    }

                    pixel.x = (int)(OFFSET_X + px * SCALE);
                    pixel.y = (int)(OFFSET_Y + py * SCALE);
                    SDL_RenderFillRect(ren, &pixel);
                }
            }

            // Paused: dim overlay
            if (paused) {
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
                SDL_Rect overlay = {0, 0, (int)SCREEN_W, (int)SCREEN_H};
                SDL_RenderFillRect(ren, &overlay);
            }

            SDL_RenderPresent(ren);

            // --- Audio ---
            if (audioDev) {
                bool shouldPlay = (chip8.sound_timer > 0) && !paused;
                if (shouldPlay != audioState.playing) {
                    SDL_LockAudioDevice(audioDev);
                    audioState.playing = shouldPlay;
                    SDL_UnlockAudioDevice(audioDev);
                }
            }
        }

        SDL_Delay(0);
    }

    if (audioDev) SDL_CloseAudioDevice(audioDev);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}