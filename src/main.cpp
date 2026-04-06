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

    // === CONFIGURE MCP23017 ===
    uint8_t cfg[2];

    // Port A = all inputs
    cfg[0] = IODIRA; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);
    // Port B = all inputs
    cfg[0] = IODIRB; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);

    // === ENABLE INTERNAL PULL-UPS (this fixes the constant-pressed issue) ===
    cfg[0] = 0x0C; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // GPPUA  = pull-ups on Port A
    cfg[0] = 0x0D; cfg[1] = 0xFF; write(mcp_fd, cfg, 2);  // GPPUB  = pull-ups on Port B

    debug("MCP23017 initialized with internal pull-ups");
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

namespace fs = std::filesystem;

const int FRAME_DELAY = 1000 / 30;

bool DEBUG_ENABLED = false;

std::ofstream logFile;

inline void debug(const std::string& msg) {
    if (DEBUG_ENABLED) {
        if (!logFile.is_open()) {
            logFile.open("/var/log/retro-launcher/retro-launcher.log", std::ios::app);
        }
        logFile << "[DEBUG] " << msg << std::endl;
        logFile.flush();
    }
}

int SCREEN_WIDTH  = 320;
int SCREEN_HEIGHT = 240;
int FONT_SIZE     = 16;
int ITEM_HEIGHT   = 24;
int LIST_TOP_PAD  = 36;
int LIST_LEFT_PAD = 8;

SDL_Window*   window   = nullptr;
SDL_Renderer* renderer = nullptr;
TTF_Font*     font     = nullptr;

SDL_Texture* texTitle   = nullptr;
SDL_Texture* texFooter  = nullptr;
SDL_Texture* texNoRoms  = nullptr;
SDL_Texture* texCount   = nullptr;
int          cachedRomCount = -1;

struct RomEntry {
    std::string  path;
    std::string  system;
    std::string  displayName;
    SDL_Texture* texNormal   = nullptr;
    SDL_Texture* texSelected = nullptr;
};

std::vector<RomEntry> romList;
size_t selectedIndex = 0;

// Returns the directory containing the launcher executable
fs::path getBaseDir() {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len == -1) return fs::current_path();
    buf[len] = '\0';
    return fs::path(buf).parent_path();
}

bool initSDL();
void shutdownSDL();
void loadROMs(const fs::path& basePath);
void cacheTextures();
void cacheStaticTextures();
void updateCountTexture();

SDL_Texture* renderText(const std::string& text, SDL_Color color) {
    if (text.empty()) return nullptr;
    SDL_Surface* surf = TTF_RenderText_Solid(font, text.c_str(), color);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

void cacheTextures() {
    SDL_Color white  = {255, 255, 255, 255};
    SDL_Color yellow = {255, 255,   0, 255};
    for (auto& rom : romList) {
        if (rom.texNormal)   SDL_DestroyTexture(rom.texNormal);
        if (rom.texSelected) SDL_DestroyTexture(rom.texSelected);
        rom.texNormal   = renderText(rom.displayName, white);
        rom.texSelected = renderText(rom.displayName, yellow);
    }
}

void cacheStaticTextures() {
    if (texTitle)  SDL_DestroyTexture(texTitle);
    if (texFooter) SDL_DestroyTexture(texFooter);
    if (texNoRoms) SDL_DestroyTexture(texNoRoms);
    SDL_Color cyan  = {  0, 255, 255, 255};
    SDL_Color gray  = {150, 150, 150, 255};
    SDL_Color white = {255, 255, 255, 255};
    texTitle  = renderText("~ Retro Launcher ~", cyan);
    texFooter = renderText("A:launch  B:top  START:quit", gray);
    texNoRoms = renderText("No ROMs found in 'roms/' folder", white);
}

void updateCountTexture() {
    if (cachedRomCount == (int)romList.size()) return;
    if (texCount) SDL_DestroyTexture(texCount);
    SDL_Color gray = {150, 150, 150, 255};
    texCount = renderText(std::to_string(romList.size()) + " ROMs", gray);
    cachedRomCount = (int)romList.size();
}

void freeTextures() {
    for (auto& rom : romList) {
        if (rom.texNormal)   { SDL_DestroyTexture(rom.texNormal);   rom.texNormal   = nullptr; }
        if (rom.texSelected) { SDL_DestroyTexture(rom.texSelected); rom.texSelected = nullptr; }
    }
}

void freeAllTextures() {
    freeTextures();
    if (texTitle)  { SDL_DestroyTexture(texTitle);  texTitle  = nullptr; }
    if (texFooter) { SDL_DestroyTexture(texFooter); texFooter = nullptr; }
    if (texNoRoms) { SDL_DestroyTexture(texNoRoms); texNoRoms = nullptr; }
    if (texCount)  { SDL_DestroyTexture(texCount);  texCount  = nullptr; }
    cachedRomCount = -1;
}

bool initSDL() {
    debug("Initializing SDL...");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    if (TTF_Init() < 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
        return false;
    }
    debug("SDL and TTF initialized");

    window = SDL_CreateWindow("Retro Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        0, 0,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        std::cerr << "Window failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_GetWindowSize(window, &SCREEN_WIDTH, &SCREEN_HEIGHT);
    debug("Window size: " + std::to_string(SCREEN_WIDTH) + "x" + std::to_string(SCREEN_HEIGHT));

    float scale   = (float)SCREEN_HEIGHT / 240.0f;
    FONT_SIZE     = (int)(14 * scale);
    ITEM_HEIGHT   = (int)(22 * scale);
    LIST_TOP_PAD  = (int)(32 * scale);
    LIST_LEFT_PAD = (int)(8  * scale);

    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::cerr << "Renderer failed: " << SDL_GetError() << std::endl;
        return false;
    }

    const char* fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        nullptr
    };
    for (int i = 0; fontPaths[i]; ++i) {
        font = TTF_OpenFont(fontPaths[i], FONT_SIZE);
        if (font) {
            debug("Font loaded: " + std::string(fontPaths[i]));
            break;
        }
    }
    if (!font) {
        std::cerr << "Font failed: " << TTF_GetError() << std::endl;
        return false;
    }

    return true;
}

void shutdownSDL() {
    freeAllTextures();
    if (font)     { TTF_CloseFont(font);           font     = nullptr; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
    if (window)   { SDL_DestroyWindow(window);     window   = nullptr; }
    TTF_Quit();
    SDL_Quit();
}

void loadROMs(const fs::path& basePath) {
    debug("Loading ROMs from: " + basePath.string());
    freeTextures();
    romList.clear();
    if (!fs::exists(basePath)) {
        debug("ROM path does not exist: " + basePath.string());
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(basePath)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::string system;
        if      (ext == ".ch8") system = "chip8";
        else if (ext == ".gb")  system = "gb";
        else if (ext == ".gbc") system = "gbc";
        else if (ext == ".gba") system = "gba";
        else continue;

        std::string name = entry.path().filename().string();
        romList.push_back({ entry.path().string(), system, "[" + system + "] " + name });
    }

    std::sort(romList.begin(), romList.end(), [](const RomEntry& a, const RomEntry& b) {
        return a.displayName < b.displayName;
    });
    debug("Loaded " + std::to_string(romList.size()) + " ROMs");
}

void launchROM(const RomEntry& entry) {
    debug("Launching ROM: " + entry.displayName);

    fs::path baseDir = getBaseDir();
    std::string emulator;

    if (entry.system == "chip8") {
        // CHIP-8 emulator is in the emulators/ subfolder
        emulator = (baseDir / "emulators" / "chip8").string();
    } else if (entry.system == "gb" || entry.system == "gbc" || entry.system == "gba") {
        // Third-party emulator (mgba) is in the roms/ subfolder
        emulator = (baseDir / "roms" / "mgba" / "build" / "sdl" / "mgba").string();
    } else {
        std::cerr << "No emulator for: " << entry.system << std::endl;
        debug("ERROR: No emulator defined for system " + entry.system);
        return;
    }

    debug("Using emulator: " + emulator);

    // Safety check
    if (!fs::exists(emulator)) {
        debug("ERROR: Emulator not found at " + emulator);
        return;
    }

    std::string romPath = fs::absolute(entry.path).string();
    debug("ROM path: " + romPath);

    shutdownSDL();
    usleep(800000);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        usleep(200000);
        const char* args[] = { emulator.c_str(), romPath.c_str(), nullptr };
        execvp(emulator.c_str(), const_cast<char* const*>(args));
        std::cerr << "Failed to launch: " << emulator << std::endl;
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        std::cerr << "fork() failed" << std::endl;
    }

    usleep(800000);

    if (!initSDL()) { 
        std::cerr << "Failed to reinit SDL" << std::endl; 
        exit(1); 
    }
    cacheStaticTextures();
    updateCountTexture();
    cacheTextures();
}

void drawTextureCenteredX(SDL_Texture* tex, int y) {
    if (!tex) return;
    int w, h;
    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
    SDL_Rect dst = {SCREEN_WIDTH / 2 - w / 2, y, w, h};
    SDL_RenderCopy(renderer, tex, nullptr, &dst);
}

void drawHeader() {
    drawTextureCenteredX(texTitle, 4);
    if (texCount) {
        int w, h;
        SDL_QueryTexture(texCount, nullptr, nullptr, &w, &h);
        SDL_Rect dst = {SCREEN_WIDTH - w - 4, 4, w, h};
        SDL_RenderCopy(renderer, texCount, nullptr, &dst);
    }
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderDrawLine(renderer, 0, LIST_TOP_PAD - 4, SCREEN_WIDTH, LIST_TOP_PAD - 4);
}

void drawFooter() {
    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderDrawLine(renderer, 0, SCREEN_HEIGHT - ITEM_HEIGHT - 2, SCREEN_WIDTH, SCREEN_HEIGHT - ITEM_HEIGHT - 2);
    drawTextureCenteredX(texFooter, SCREEN_HEIGHT - ITEM_HEIGHT);
}

void drawHighlight(int y) {
    SDL_SetRenderDrawColor(renderer, 40, 40, 120, 255);
    SDL_Rect bar = {0, y, SCREEN_WIDTH, ITEM_HEIGHT};
    SDL_RenderFillRect(renderer, &bar);
}

bool handleKey(SDL_Keycode key, int& scrollOffset, int maxVisible) {
    switch (key) {
        case SDLK_ESCAPE:
            debug("Exiting launcher"); 
            return true;
        case SDLK_UP:
            debug("Key UP"); 
            if (!romList.empty())
                selectedIndex = (selectedIndex == 0) ? romList.size() - 1 : selectedIndex - 1;
            break;
        case SDLK_DOWN:
            debug("Key DOWN");
            if (!romList.empty())
                selectedIndex = (selectedIndex + 1) % romList.size();
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            debug("Key RETURN");
            if (!romList.empty()) launchROM(romList[selectedIndex]);
            break;
        case SDLK_BACKSPACE:
            debug("Key BACKSPACE");
            selectedIndex = 0;
            scrollOffset  = 0;
            break;
        case SDLK_TAB:
            debug("Key TAB");
            if (!romList.empty()) selectedIndex = romList.size() - 1;
            break;
        default: break;
    }
    return false;
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-d" || std::string(argv[i]) == "--debug") {
            DEBUG_ENABLED = true;
            break;
        }
    }

    debug("=== Retro Launcher Starting ===");
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

        if (!romList.empty()) {
            if ((int)selectedIndex >= scrollOffset + maxVisible)
                scrollOffset = (int)selectedIndex - maxVisible + 1;
            if ((int)selectedIndex < scrollOffset)
                scrollOffset = (int)selectedIndex;
            if (scrollOffset < 0) scrollOffset = 0;
            int maxScroll = (int)romList.size() - maxVisible;
            if (maxScroll >= 0 && scrollOffset > maxScroll)
                scrollOffset = maxScroll;
        }

        SDL_SetRenderDrawColor(renderer, 10, 10, 20, 255);
        SDL_RenderClear(renderer);

        drawHeader();

        if (romList.empty()) {
            drawTextureCenteredX(texNoRoms, SCREEN_HEIGHT / 2 - ITEM_HEIGHT / 2);
        } else {
            size_t first = (size_t)scrollOffset;
            size_t last  = std::min(first + (size_t)maxVisible, romList.size());
            int y = LIST_TOP_PAD;

            for (size_t i = first; i < last; ++i) {
                if (i == selectedIndex) drawHighlight(y);
                SDL_Texture* tex = (i == selectedIndex)
                    ? romList[i].texSelected : romList[i].texNormal;
                if (tex) {
                    int w, h;
                    SDL_QueryTexture(tex, nullptr, nullptr, &w, &h);
                    int maxW = SCREEN_WIDTH - LIST_LEFT_PAD - 8;
                    if (w > maxW) w = maxW;
                    SDL_Rect src = {0, 0, w, h};
                    SDL_Rect dst = {LIST_LEFT_PAD, y + (ITEM_HEIGHT - h) / 2, w, h};
                    SDL_RenderCopy(renderer, tex, &src, &dst);
                }
                y += ITEM_HEIGHT;
            }

            if ((int)romList.size() > maxVisible) {
                int listH = SCREEN_HEIGHT - LIST_TOP_PAD - ITEM_HEIGHT - 4;
                int barH  = listH * maxVisible / (int)romList.size();
                int barY  = LIST_TOP_PAD + listH * scrollOffset / (int)romList.size();
                SDL_SetRenderDrawColor(renderer, 100, 100, 200, 255);
                SDL_Rect bar = {SCREEN_WIDTH - 4, barY, 4, barH};
                SDL_RenderFillRect(renderer, &bar);
            }
        }

        drawFooter();
        SDL_RenderPresent(renderer);

        Uint32 frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < (Uint32)FRAME_DELAY)
            SDL_Delay(FRAME_DELAY - frameTime);
    }

    shutdownSDL();
    debug("=== Retro Launcher Shutting Down ===");
    return 0;
}
