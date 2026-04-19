/**
 * chip8.cpp  —  CHIP-8 emulator with SDL2
 *               For Waveshare 3.5" 640×480 DPI display, Pi Zero 2 W, KMS mode
 *
 * REQUIRED in /boot/firmware/config.txt:
 *   gpu_mem=64
 *   dtoverlay=vc4-kms-v3d
 *   dtoverlay=waveshare-35dpi
 *
 * BUILD:
 *   g++ -std=c++17 -Wall -O2 -o chip8 chip8.cpp \
 *       $(pkg-config --cflags --libs sdl2)
 *
 * RUN:
 *   export XDG_RUNTIME_DIR=/run/user/$(id -u)
 *   export SDL_VIDEODRIVER=kmsdrm
 *   ./chip8 rom.ch8
 *
 * ROOT CAUSE OF CORRUPTION (now fixed):
 *   SDL_WINDOW_FULLSCREEN_DESKTOP asks SDL2 to give us "the desktop resolution",
 *   but under kmsdrm/opengl there is no desktop — SDL2 picks a DRM mode and maps
 *   coordinates onto it.  The logical size of the window did NOT match 640×480,
 *   so our 64×32 texture was being stretched with wrong geometry, producing the
 *   QR-code noise pattern visible in the photo.
 *
 *   Fix: open a plain (non-fullscreen) 640×480 window, then call
 *   SDL_RenderSetLogicalSize(renderer, 640, 480) so SDL2 manages all scaling
 *   internally and we always draw in a clean 640×480 coordinate space.
 *   The CHIP-8 64×32 texture is copied into a centred 640×320 dest rect.
 *
 * CHIP-8 key → PC keyboard
 *   1 2 3 C  →  1 2 3 4
 *   4 5 6 D  →  Q W E R
 *   7 8 9 E  →  A S D F
 *   A 0 B F  →  Z X C V
 */

#include <SDL2/SDL.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

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

const uint8_t X_B_PIN      = 1;
const uint8_t Y_B_PIN      = 2;
const uint8_t B_B_PIN      = 3;
const uint8_t BUZZER_B_PIN = 6;           // PB6 — piezo buzzer output

const uint8_t OLATB        = 0x15;        // MCP23017 output latch for port B
static constexpr uint8_t PB6_MASK = (1 << BUZZER_B_PIN);

const uint8_t IODIRA = 0x00;
const uint8_t IODIRB = 0x01;
const uint8_t GPIOA  = 0x12;
const uint8_t GPIOB  = 0x13;
const uint8_t GPPUA  = 0x0C;
const uint8_t GPPUB  = 0x0D;

// ─── Display config ───────────────────────────────────────────────────────────

static constexpr int DISPLAY_W = 64;
static constexpr int DISPLAY_H = 32;
static constexpr int SCREEN_W  = 640;
static constexpr int SCREEN_H  = 480;

// Game area: 640×320, centred vertically → 80 px black bars top and bottom
static constexpr int GAME_W   = 640;
static constexpr int GAME_H   = 480;
static constexpr int OFFSET_X = 0;
static constexpr int OFFSET_Y = (SCREEN_H - GAME_H) / 2;   // 80

// Pixel colours
static constexpr uint8_t FG_R=255, FG_G=255, FG_B=255;
static constexpr uint8_t BG_R=  0, BG_G=  0, BG_B=  0;

// ─── Timing ───────────────────────────────────────────────────────────────────

static constexpr int    CPU_HZ        = 700;
static constexpr int    TIMER_HZ      = 60;
static constexpr double BEEP_HZ       = 440.0;

// ─── Memory ───────────────────────────────────────────────────────────────────

static constexpr std::size_t MEM_SIZE     = 4096;
static constexpr uint16_t    ROM_START    = 0x200;
static constexpr std::size_t MAX_ROM_SIZE = MEM_SIZE - ROM_START;
static constexpr uint16_t    FONT_BASE    = 0x000;

static constexpr std::array<uint8_t,80> FONT = {
    0xF0,0x90,0x90,0x90,0xF0, 0x20,0x60,0x20,0x20,0x70,
    0xF0,0x10,0xF0,0x80,0xF0, 0xF0,0x10,0xF0,0x10,0xF0,
    0x90,0x90,0xF0,0x10,0x10, 0xF0,0x80,0xF0,0x10,0xF0,
    0xF0,0x80,0xF0,0x90,0xF0, 0xF0,0x10,0x20,0x40,0x40,
    0xF0,0x90,0xF0,0x90,0xF0, 0xF0,0x90,0xF0,0x10,0xF0,
    0xF0,0x90,0xF0,0x90,0x90, 0xE0,0x90,0xE0,0x90,0xE0,
    0xF0,0x80,0x80,0x80,0xF0, 0xE0,0x90,0x90,0x90,0xE0,
    0xF0,0x80,0xF0,0x80,0xF0, 0xF0,0x80,0xF0,0x80,0x80,
};

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
    cfg[0] = IODIRA; cfg[1] = 0xFF;              write(mcp_fd, cfg, 2); // port A all inputs
    cfg[0] = IODIRB; cfg[1] = ~PB6_MASK;         write(mcp_fd, cfg, 2); // port B: PB6=output, rest=input
    cfg[0] = GPPUA;  cfg[1] = 0xFF;              write(mcp_fd, cfg, 2); // port A pull-ups on
    cfg[0] = GPPUB;  cfg[1] = ~PB6_MASK;         write(mcp_fd, cfg, 2); // port B pull-ups on inputs only
    cfg[0] = OLATB;  cfg[1] = 0x00;              write(mcp_fd, cfg, 2); // PB6 low (silent)

    debug("MCP23017 initialized (buttons + PB6 buzzer output)");
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

// ─── Keyboard fallback map ────────────────────────────────────────────────────

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

// ─── CHIP-8 ───────────────────────────────────────────────────────────────────

struct Chip8 {
    std::array<uint8_t, MEM_SIZE>           mem{};
    std::array<uint8_t, 16>                 V{};
    std::array<uint16_t,16>                 stack{};
    std::array<bool,DISPLAY_W*DISPLAY_H>    display{};
    std::array<bool,16>                     keys{};
    uint16_t I=0, PC=ROM_START;
    uint8_t  SP=0, DT=0, ST=0;
    bool     displayDirty=false, waitKey=false;
    uint8_t  waitReg=0;
    std::mt19937 rng{std::random_device{}()};

    Chip8(){ std::copy(FONT.begin(),FONT.end(),mem.begin()+FONT_BASE); }

    void loadROM(const std::string& path){
        std::ifstream f(path,std::ios::binary|std::ios::ate);
        if(!f) throw std::runtime_error("Cannot open: "+path);
        auto sz=f.tellg();
        if(sz<=0||static_cast<std::size_t>(sz)>MAX_ROM_SIZE)
            throw std::runtime_error("ROM size invalid");
        f.seekg(0);
        f.read(reinterpret_cast<char*>(&mem[ROM_START]),sz);
    }

    void tickTimers(){ if(DT)--DT; if(ST)--ST; }

    void step(){
        if(waitKey) return;
        const uint16_t op=uint16_t(mem[PC]<<8)|mem[PC+1]; PC+=2;
        const uint8_t  x=(op>>8)&0xF, y=(op>>4)&0xF, n=op&0xF, kk=op&0xFF;
        const uint16_t nnn=op&0xFFF;
        switch(op&0xF000){
        case 0x0000:
            if(op==0x00E0){display.fill(false);displayDirty=true;}
            else if(op==0x00EE) PC=stack[--SP];
            break;
        case 0x1000: PC=nnn; break;
        case 0x2000: stack[SP++]=PC; PC=nnn; break;
        case 0x3000: if(V[x]==kk) PC+=2; break;
        case 0x4000: if(V[x]!=kk) PC+=2; break;
        case 0x5000: if(V[x]==V[y]) PC+=2; break;
        case 0x6000: V[x]=kk; break;
        case 0x7000: V[x]+=kk; break;
        case 0x8000:
            switch(n){
            case 0: V[x] =V[y]; break;
            case 1: V[x]|=V[y]; break;
            case 2: V[x]&=V[y]; break;
            case 3: V[x]^=V[y]; break;
            case 4:{uint16_t r=V[x]+V[y];V[0xF]=r>255;V[x]=r&0xFF;break;}
            case 5:{uint8_t f=V[x]>=V[y];V[x]-=V[y];V[0xF]=f;break;}
            case 6:{uint8_t f=V[x]&1;V[x]>>=1;V[0xF]=f;break;}
            case 7:{uint8_t f=V[y]>=V[x];V[x]=V[y]-V[x];V[0xF]=f;break;}
            case 0xE:{uint8_t f=V[x]>>7;V[x]<<=1;V[0xF]=f;break;}
            } break;
        case 0x9000: if(V[x]!=V[y]) PC+=2; break;
        case 0xA000: I=nnn; break;
        case 0xB000: PC=nnn+V[0]; break;
        case 0xC000: V[x]=(rng()&0xFF)&kk; break;
        case 0xD000:{
            uint8_t vx=V[x]%DISPLAY_W, vy=V[y]%DISPLAY_H;
            V[0xF]=0;
            for(int row=0;row<n;++row){
                uint8_t b=mem[I+row];
                for(int col=0;col<8;++col) if(b&(0x80>>col)){
                    int px=(vx+col)%DISPLAY_W, py=(vy+row)%DISPLAY_H;
                    auto& p=display[py*DISPLAY_W+px];
                    if(p) V[0xF]=1;
                    p^=true;
                }
            }
            displayDirty=true;
        } break;
        case 0xE000:
            if(kk==0x9E&& keys[V[x]]) PC+=2;
            if(kk==0xA1&&!keys[V[x]]) PC+=2;
            break;
        case 0xF000:
            switch(kk){
            case 0x07: V[x]=DT; break;
            case 0x0A: waitKey=true; waitReg=x; break;
            case 0x15: DT=V[x]; break;
            case 0x18: ST=V[x]; break;
            case 0x1E: I+=V[x]; break;
            case 0x29: I=FONT_BASE+V[x]*5; break;
            case 0x33: mem[I]=V[x]/100;mem[I+1]=(V[x]/10)%10;mem[I+2]=V[x]%10; break;
            case 0x55: for(int i=0;i<=x;++i) mem[I+i]=V[i]; break;
            case 0x65: for(int i=0;i<=x;++i) V[i]=mem[I+i]; break;
            } break;
        }
    }

    void keypadReset()
    {
        keys[0x0] = false;  // A
        keys[0x1] = false;  // UP
        keys[0x2] = false;
        keys[0x3] = false;
        keys[0x4] = false;  // DOWN
        keys[0x5] = false;
        keys[0x6] = false;  // RIGHT
        keys[0x7] = false;  // START
        keys[0x8] = false;
        keys[0x9] = false;  // B
        keys[0xA] = false;
        keys[0xC] = false;  // SELECT
        keys[0xD] = false;
        keys[0xE] = false;
        keys[0xF] = false;
    }

    void handleKey(uint8_t key, bool pressed)
    {    
        if (key < 16)
        {
            waitKey=false;
            keys[key] = pressed;
            debug("handleKey: keys[" + std::to_string(key) + "] = " + std::to_string(pressed));
        }
    }

    bool gameButtonAjustment(uint16_t pressed, char* gameName) {
        if (gameName == nullptr) {
            return false;
        }
        std::string s(gameName);
        // Convert the entire string to lowercase (case-insensitive match)
        std::transform(s.begin(), s.end(), s.begin(),
                    [](unsigned char c) { return std::tolower(c); });
        // Check for substring "pong" (now guaranteed lowercase)
        if (s.find("pong") != std::string::npos) {
            if (pressed == 16386) handleKey(0x1, true);  // UP
            if (pressed == 16388) handleKey(0x4, true);  // DOWN
            if (pressed == 16392) handleKey(0x2, true);  // LEFT
        } else {
            if (pressed == 16386) handleKey(0x8, true);  // UP
            if (pressed == 16388) handleKey(0x2, true);  // DOWN
            if (pressed == 16392) handleKey(0x4, true);  // LEFT
        }
        if (pressed == 16402) handleKey(0x6, true);  // RIGHT
        if (pressed == 16512) handleKey(0x5, true);  // A
        if (pressed == 18432) handleKey(0x9, true);  // B
        if (pressed == 16448) handleKey(0xC, true);  // SELECT
        return true;
    }
};

// ─── Buzzer via MCP23017 PB6 ─────────────────────────────────────────────────
//
// Drives PB6 as a square wave at BEEP_HZ from a background thread.
// Uses the shared mcp_fd opened by initMCP() — no second I²C open().
// initMCP() must be called before buzzer.init().

struct Buzzer {
    std::atomic<bool> beeping { false };
    std::atomic<bool> running { false };
    std::thread       thr;

    // Write a single register via the shared mcp_fd
    static bool mcp_write(uint8_t reg, uint8_t val) {
        if (mcp_fd < 0) return false;
        uint8_t buf[2] = { reg, val };
        return ::write(mcp_fd, buf, 2) == 2;
    }

    // Background thread: toggles PB6 at BEEP_HZ while beeping==true
    void squareWaveLoop() {
        const auto half = std::chrono::microseconds(
            static_cast<long>(1'000'000.0 / (2.0 * BEEP_HZ)));
        uint8_t olatb = 0x00;

        while (running.load(std::memory_order_relaxed)) {
            if (beeping.load(std::memory_order_relaxed)) {
                olatb ^= PB6_MASK;
                mcp_write(OLATB, olatb);
                std::this_thread::sleep_for(half);
            } else {
                if (olatb & PB6_MASK) {         // ensure pin goes low when silent
                    olatb &= ~PB6_MASK;
                    mcp_write(OLATB, olatb);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        // Drive low on exit
        mcp_write(OLATB, 0x00);
    }

    void init() {
        if (mcp_fd < 0) {
            debug("[buzzer] mcp_fd not open — buzzer disabled");
            return;
        }
        // PB6 direction and latch already configured by initMCP()
        running = true;
        thr = std::thread(&Buzzer::squareWaveLoop, this);
        debug("[buzzer] MCP23017 PB6 square-wave buzzer started");
    }

    void setBeep(bool on) { beeping.store(on, std::memory_order_relaxed); }

    ~Buzzer() {
        running = false;
        if (thr.joinable()) thr.join();
        // Leave PB6 low — mcp_write called from squareWaveLoop epilogue
    }
};

// ─── Renderer ─────────────────────────────────────────────────────────────────

struct Renderer {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;

    // Destination in logical 640×480 space: 640×320 centred vertically
    SDL_Rect dest{ OFFSET_X, OFFSET_Y, GAME_W, GAME_H };

    void init(const char* title){
        // KEY FIX: open a plain window at exactly the physical display size.
        // Do NOT use SDL_WINDOW_FULLSCREEN_DESKTOP — under kmsdrm/opengl that
        // flag causes SDL2 to pick an arbitrary DRM mode and remap coordinates,
        // breaking the 1:1 mapping between our logical size and the panel pixels.
        window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            SCREEN_W, SCREEN_H, 0);
        if(!window) throw std::runtime_error(
            std::string("SDL_CreateWindow: ")+SDL_GetError());

        renderer = SDL_CreateRenderer(window,-1,
            SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if(!renderer)
            renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
        if(!renderer) throw std::runtime_error(
            std::string("SDL_CreateRenderer: ")+SDL_GetError());

        // Tell SDL2 our logical coordinate space is exactly 640×480.
        // SDL2 will then scale/letterbox automatically if the actual
        // framebuffer differs — but on this display they match exactly.
        if(SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H) != 0)
            std::cerr << "[renderer] SetLogicalSize warning: "
                      << SDL_GetError() << '\n';

        // Use nearest-neighbour scaling so CHIP-8 pixels stay sharp rectangles
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

        texture = SDL_CreateTexture(renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            DISPLAY_W, DISPLAY_H);
        if(!texture) throw std::runtime_error(
            std::string("SDL_CreateTexture: ")+SDL_GetError());

        SDL_SetRenderDrawColor(renderer,0,0,0,255);

        // Log actual backend for diagnostics
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(renderer,&info);
        int w=0,h=0;
        SDL_RenderGetLogicalSize(renderer,&w,&h);
        std::cerr << "[renderer] backend=" << SDL_GetCurrentVideoDriver()
                  << " renderer=" << info.name
                  << " logical=" << w << "x" << h << '\n';
        SDL_GetWindowSize(window,&w,&h);
        std::cerr << "[renderer] window=" << w << "x" << h << '\n';
    }

    void draw(const std::array<bool,DISPLAY_W*DISPLAY_H>& display){
        static std::array<uint32_t,DISPLAY_W*DISPLAY_H> pixels;
        constexpr uint32_t ON  = 0xFF000000
            |(uint32_t(FG_R)<<16)|(uint32_t(FG_G)<<8)|FG_B;
        constexpr uint32_t OFF = 0xFF000000
            |(uint32_t(BG_R)<<16)|(uint32_t(BG_G)<<8)|BG_B;
        for(int i=0;i<DISPLAY_W*DISPLAY_H;++i)
            pixels[i]=display[i]?ON:OFF;
        SDL_UpdateTexture(texture,nullptr,pixels.data(),
                          DISPLAY_W*sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer,texture,nullptr,&dest);
        SDL_RenderPresent(renderer);
    }

    ~Renderer(){
        if(texture)  SDL_DestroyTexture(texture);
        if(renderer) SDL_DestroyRenderer(renderer);
        if(window)   SDL_DestroyWindow(window);
    }
};

// ─── SDL2 init with backend fallback ─────────────────────────────────────────

static bool initSDL(){
    if(SDL_getenv("SDL_VIDEODRIVER"))
        return SDL_Init(SDL_INIT_VIDEO)==0;
    SDL_setenv("SDL_VIDEODRIVER","kmsdrm",1);
    if(SDL_Init(SDL_INIT_VIDEO)==0) return true;
    std::cerr<<"[init] kmsdrm failed ("<<SDL_GetError()<<"), trying auto\n";
    SDL_setenv("SDL_VIDEODRIVER","",1);
    SDL_Quit();
    return SDL_Init(SDL_INIT_VIDEO)==0;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc,char* argv[]){
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    DEBUG_ENABLED = true;
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-d" || std::string(argv[i]) == "--debug") {
            DEBUG_ENABLED = true;
            debug("=== CHIP-8 Emulator Starting (DEBUG MODE) ===");
            break;
        }
    }

    debug("Loading ROM: " + std::string(argv[1]));
    
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " rom.ch8 [-d|--debug]\n";
        return 1;
    }
    
    if(!initSDL()){
        std::cerr<<"SDL_Init failed: "<<SDL_GetError()<<'\n';
        return 1;
    }

    // right after initSDL
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1");
    SDL_ShowCursor(SDL_DISABLE);

    Chip8    chip8;
    Renderer ren;
    Buzzer   buzzer;
    char* gameRom = argv[1];
    char* gameName = strrchr(gameRom, '/');
    try{
        chip8.loadROM(gameRom);
        ren.init("CHIP-8");
    }catch(const std::exception& e){
        std::cerr<<"Init error: "<<e.what()<<'\n';
        SDL_Quit(); return 1;
    }

    if(initMCP()) {
        std::cout << "MCP23017 gamepad + buzzer connected\n";
        buzzer.init();   // must come after initMCP() — shares mcp_fd
    } else {
        std::cout << "MCP23017 not found — using keyboard, buzzer disabled\n";
    }

    using clock=std::chrono::high_resolution_clock;
    using us=std::chrono::microseconds;
    const us cpuPeriod  {static_cast<long>(1'000'000.0/CPU_HZ)  };
    const us timerPeriod{static_cast<long>(1'000'000.0/TIMER_HZ)};
    auto lastCpu=clock::now(), lastTimer=clock::now();

    bool running=true;
    while(running){
        SDL_Event ev;
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_QUIT){running=false;break;}
            if(ev.type==SDL_KEYDOWN&&
               ev.key.keysym.scancode==SDL_SCANCODE_ESCAPE){running=false;break;}
            for(auto&[sc,key]:KEY_MAP){
                if(ev.key.keysym.scancode==sc){
                    if(ev.type==SDL_KEYDOWN) 
                        chip8.handleKey(key, true);
                    else if(ev.type==SDL_KEYUP)
                        chip8.keypadReset();
                }
            }
        }

        // === READ MCP BUTTONS WITH EDGE DETECTION ===
        static uint16_t last_pressed = 0xFFFF;

        uint16_t pressed = readMCPButtons();

        auto now=clock::now();
        chip8.keypadReset();  // Reset all keys before setting the current state
        
        if (pressed == 16416) {running=false;break;}  // START
        if (pressed > 0) chip8.gameButtonAjustment(pressed, gameName);

        last_pressed = pressed;
        debug("MCP Buttons state: " + std::to_string(pressed));


        debug("MCP Buttons state: " + std::to_string(pressed));

        if(now-lastCpu  >=cpuPeriod  ){chip8.step();      lastCpu  +=cpuPeriod;  }
        if(now-lastTimer>=timerPeriod){
            chip8.tickTimers();
            buzzer.setBeep(chip8.ST>0);
            lastTimer+=timerPeriod;
        }
        if(chip8.displayDirty){
            ren.draw(chip8.display);
            chip8.displayDirty=false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    SDL_Quit();
    return 0;
}