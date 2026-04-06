#include <SDL2/SDL.h>
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <cstring>

// CHIP-8 resolution
const unsigned int CHIP8_W = 64;
const unsigned int CHIP8_H = 32;

// Waveshare DPI display resolution (FIXED)
const unsigned int SCREEN_W = 640;
const unsigned int SCREEN_H = 480;

// Perfect integer scaling
const unsigned int SCALE = 10; // 64*10=640, 32*10=320

// Center vertically
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
            case 0x1000: pc = nnn; break;
            case 0x6000: V[x] = nn; break;
            case 0x7000: V[x] += nn; break;

            case 0xA000: I = nnn; break;

            case 0xD000: {
                uint8_t vx = V[x] % CHIP8_W;
                uint8_t vy = V[y] % CHIP8_H;
                V[0xF] = 0;

                for (int row = 0; row < n; ++row) {
                    uint8_t sprite = memory[I + row];
                    for (int col = 0; col < 8; ++col) {
                        if (sprite & (0x80 >> col)) {
                            int idx = (vy + row) * CHIP8_W + (vx + col);
                            if (display[idx]) V[0xF] = 1;
                            display[idx] ^= 1;
                        }
                    }
                }
                break;
            }
        }
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " rom.ch8\n";
        return 1;
    }

    Chip8 chip8;
    if (!chip8.loadROM(argv[1])) {
        std::cout << "Failed to load ROM\n";
        return 1;
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    // Window FIXED to 640x480
    SDL_Window* window = SDL_CreateWindow(
        "CHIP-8",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        SCREEN_W,
        SCREEN_H,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

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
    auto last = std::chrono::high_resolution_clock::now();

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
        }

        chip8.emulateCycle();

        // draw
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_Rect pixel{0,0,SCALE,SCALE};

        for (int y = 0; y < CHIP8_H; y++) {
            for (int x = 0; x < CHIP8_W; x++) {
                if (chip8.display[y * CHIP8_W + x]) {
                    pixel.x = OFFSET_X + x * SCALE;
                    pixel.y = OFFSET_Y + y * SCALE;
                    SDL_SetRenderDrawColor(renderer, 255,255,255,255);
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }

        SDL_RenderPresent(renderer);

        audio.playing = chip8.sound_timer > 0;

        SDL_Delay(2);    
    }

    SDL_Quit();
}
