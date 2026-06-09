#include "ternary_host_runtime.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct TextureState {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

constexpr int kTextCellWidth = 8;
constexpr int kTextCellHeight = 12;
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr std::uint32_t kTextBackgroundRgba = 0x050814ff;

void destroyTexture(TextureState& state) {
    if (state.texture) {
        SDL_DestroyTexture(state.texture);
        state.texture = nullptr;
    }
    state.width = 0;
    state.height = 0;
}

std::uint32_t toSdlAbgr(std::uint32_t rgba) {
    const std::uint32_t r = (rgba >> 24) & 0xff;
    const std::uint32_t g = (rgba >> 16) & 0xff;
    const std::uint32_t b = (rgba >> 8) & 0xff;
    const std::uint32_t a = rgba & 0xff;
    return (a << 24) | (b << 16) | (g << 8) | r;
}

SDL_Rect letterboxRect(int window_w, int window_h, int source_w, int source_h) {
    if (window_w <= 0 || window_h <= 0 || source_w <= 0 || source_h <= 0) {
        return SDL_Rect{0, 0, 0, 0};
    }
    int scale = std::min(window_w / source_w, window_h / source_h);
    if (scale < 1) scale = 1;
    const int w = source_w * scale;
    const int h = source_h * scale;
    return SDL_Rect{(window_w - w) / 2, (window_h - h) / 2, w, h};
}

std::pair<int, int> textureDimensions(
    const sandbox::host::TosFramebufferSnapshot& framebuffer) {
    if (framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25) {
        return {framebuffer.width * kTextCellWidth,
                framebuffer.height * kTextCellHeight};
    }
    return {framebuffer.width, framebuffer.height};
}

std::array<std::uint8_t, kGlyphHeight> glyphRows(char ch) {
    if (ch >= 'a' && ch <= 'z') {
        ch = static_cast<char>(ch - 'a' + 'A');
    }

    switch (ch) {
        case 'A': return {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        case 'B': return {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
        case 'C': return {0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f};
        case 'D': return {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
        case 'E': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
        case 'F': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
        case 'G': return {0x0f, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0f};
        case 'H': return {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        case 'I': return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
        case 'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
        case 'M': return {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
        case 'P': return {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
        case 'Q': return {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d};
        case 'R': return {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
        case 'S': return {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
        case 'T': return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11};
        case 'X': return {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f};

        case '0': return {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
        case '1': return {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
        case '2': return {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
        case '3': return {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
        case '4': return {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
        case '5': return {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e};
        case '6': return {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
        case '7': return {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
        case '9': return {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c};

        case '!': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
        case '"': return {0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00};
        case '#': return {0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a};
        case '$': return {0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04};
        case '%': return {0x19, 0x1a, 0x02, 0x04, 0x08, 0x0b, 0x13};
        case '&': return {0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d};
        case '\'': return {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00};
        case '(': return {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
        case ')': return {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
        case '*': return {0x00, 0x15, 0x0e, 0x1f, 0x0e, 0x15, 0x00};
        case '+': return {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00};
        case ',': return {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08};
        case '-': return {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
        case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
        case ':': return {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00};
        case ';': return {0x00, 0x0c, 0x0c, 0x00, 0x04, 0x04, 0x08};
        case '<': return {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
        case '=': return {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00};
        case '>': return {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
        case '?': return {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
        case '@': return {0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0e};
        case '[': return {0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e};
        case '\\': return {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01};
        case ']': return {0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e};
        case '^': return {0x04, 0x0a, 0x11, 0x00, 0x00, 0x00, 0x00};
        case '_': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f};
        case '`': return {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00};
        case '{': return {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02};
        case '|': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case '}': return {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08};
        case '~': return {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00};
        default: return {0x00, 0x00, 0x0e, 0x02, 0x04, 0x00, 0x04};
    }
}

void fillRect(std::vector<std::uint32_t>& pixels,
              int width,
              int height,
              int x,
              int y,
              int w,
              int h,
              std::uint32_t color) {
    const int x0 = std::clamp(x, 0, width);
    const int y0 = std::clamp(y, 0, height);
    const int x1 = std::clamp(x + w, 0, width);
    const int y1 = std::clamp(y + h, 0, height);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            pixels[static_cast<std::size_t>(py * width + px)] = color;
        }
    }
}

void drawGlyph(std::vector<std::uint32_t>& pixels,
               int width,
               int height,
               int x,
               int y,
               char ch,
               std::uint32_t color) {
    const auto rows = glyphRows(ch);
    const int gx = x + (kTextCellWidth - kGlyphWidth) / 2;
    const int gy = y + (kTextCellHeight - kGlyphHeight) / 2;
    for (int row = 0; row < kGlyphHeight; ++row) {
        for (int col = 0; col < kGlyphWidth; ++col) {
            if ((rows[static_cast<std::size_t>(row)] & (1 << (kGlyphWidth - col - 1))) == 0) {
                continue;
            }
            const int px = gx + col;
            const int py = gy + row;
            if (px >= 0 && px < width && py >= 0 && py < height) {
                pixels[static_cast<std::size_t>(py * width + px)] = color;
            }
        }
    }
}

std::vector<std::uint32_t> renderTextFramebuffer(
    const sandbox::host::TosFramebufferSnapshot& framebuffer,
    int width,
    int height) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width * height),
                                      kTextBackgroundRgba);
    for (int cy = 0; cy < framebuffer.height; ++cy) {
        for (int cx = 0; cx < framebuffer.width; ++cx) {
            const int cell = cy * framebuffer.width + cx;
            const std::size_t index = static_cast<std::size_t>(cell);
            const char ch =
                index < framebuffer.glyphs.size() ? framebuffer.glyphs[index] : ' ';
            const std::uint32_t fg =
                index < framebuffer.rgba.size() ? framebuffer.rgba[index] : kTextBackgroundRgba;
            const int px = cx * kTextCellWidth;
            const int py = cy * kTextCellHeight;
            const bool is_space = ch == ' ';
            const std::uint32_t bg =
                is_space && fg != kTextBackgroundRgba ? fg : kTextBackgroundRgba;
            fillRect(pixels, width, height, px, py, kTextCellWidth, kTextCellHeight, bg);
            if (!is_space) {
                drawGlyph(pixels, width, height, px, py, ch, fg);
            }
        }
    }

    const char sprite_ch = static_cast<char>(framebuffer.sprite_attr & 0xff);
    const int sprite_color = static_cast<int>((framebuffer.sprite_attr >> 8) & 0x0f);
    if (sprite_ch >= 32 && sprite_ch <= 126) {
        drawGlyph(pixels, width, height,
                  static_cast<int>(framebuffer.sprite_x) * kTextCellWidth,
                  static_cast<int>(framebuffer.sprite_y) * kTextCellHeight,
                  sprite_ch,
                  sandbox::host::detail::paletteColor(sprite_color));
    }
    return pixels;
}

std::vector<std::uint32_t> renderFramebufferPixels(
    const sandbox::host::TosFramebufferSnapshot& framebuffer,
    int width,
    int height) {
    if (framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25) {
        return renderTextFramebuffer(framebuffer, width, height);
    }
    std::vector<std::uint32_t> pixels(framebuffer.rgba.size(), 0);
    for (std::size_t i = 0; i < framebuffer.rgba.size(); ++i) {
        pixels[i] = framebuffer.rgba[i];
    }
    return pixels;
}

bool updateTexture(SDL_Renderer* renderer,
                   TextureState& texture,
                   const sandbox::host::TosFramebufferSnapshot& framebuffer) {
    const auto [texture_width, texture_height] = textureDimensions(framebuffer);
    if (!texture.texture ||
        texture.width != texture_width ||
        texture.height != texture_height) {
        destroyTexture(texture);
        texture.texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            texture_width,
                                            texture_height);
        if (!texture.texture) return false;
        SDL_SetTextureBlendMode(texture.texture, SDL_BLENDMODE_NONE);
        texture.width = texture_width;
        texture.height = texture_height;
    }

    std::vector<std::uint32_t> pixels =
        renderFramebufferPixels(framebuffer, texture_width, texture_height);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = toSdlAbgr(pixels[i]);
    }
    return SDL_UpdateTexture(texture.texture,
                             nullptr,
                             pixels.data(),
                             texture_width * static_cast<int>(sizeof(std::uint32_t))) == 0;
}

std::string statusTitle(const sandbox::host::TosRuntimeSnapshot& snapshot,
                        bool paused,
                        bool debug_overlay) {
    std::ostringstream out;
    out << "OS 3";
    if (debug_overlay) {
        out << " | PC " << snapshot.pc
            << " | " << sandbox::vm::vmStatusToString(snapshot.status)
            << " | cycles " << snapshot.cycles
            << " | mode " << snapshot.gpu_mode;
        if (paused) out << " | paused";
        if (!snapshot.image_version.empty()) out << " | " << snapshot.image_version;
    }
    return out.str();
}

void mapMouseToGuest(const SDL_Rect& dest,
                     int frame_w,
                     int frame_h,
                     int mouse_x,
                     int mouse_y,
                     long long& out_x,
                     long long& out_y) {
    if (dest.w <= 0 || dest.h <= 0) {
        out_x = 0;
        out_y = 0;
        return;
    }
    const int clamped_x = std::clamp(mouse_x - dest.x, 0, std::max(0, dest.w - 1));
    const int clamped_y = std::clamp(mouse_y - dest.y, 0, std::max(0, dest.h - 1));
    out_x = (static_cast<long long>(clamped_x) * frame_w) / dest.w;
    out_y = (static_cast<long long>(clamped_y) * frame_h) / dest.h;
}

std::filesystem::path executableDirectory(char** argv) {
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        std::filesystem::path path(base_path);
        SDL_free(base_path);
        if (!path.empty()) return path;
    }

    if (argv && argv[0] && argv[0][0] != '\0') {
        std::error_code ec;
        std::filesystem::path exe_path =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec);
        if (!ec && !exe_path.parent_path().empty()) return exe_path.parent_path();
    }

    std::error_code ec;
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

std::string bundledPath(const std::filesystem::path& bundle_dir,
                        const char* filename) {
    return (bundle_dir / filename).string();
}

bool hasHostModifier(SDL_Keymod mods) {
    return (mods & KMOD_CTRL) != 0 || (mods & KMOD_GUI) != 0;
}

long long asciiFromKey(SDL_Keycode key, SDL_Keymod mods) {
    const bool shifted = (mods & KMOD_SHIFT) != 0;
    if (key >= SDLK_a && key <= SDLK_z) {
        return shifted ? ('A' + key - SDLK_a) : ('a' + key - SDLK_a);
    }
    if (key >= SDLK_0 && key <= SDLK_9) {
        static constexpr char shifted_digits[] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
        const int digit = static_cast<int>(key - SDLK_0);
        return shifted ? shifted_digits[digit] : ('0' + digit);
    }
    if (key >= SDLK_KP_0 && key <= SDLK_KP_9) {
        return '0' + key - SDLK_KP_0;
    }
    switch (key) {
        case SDLK_SPACE: return ' ';
        case SDLK_MINUS: return shifted ? '_' : '-';
        case SDLK_EQUALS: return shifted ? '+' : '=';
        case SDLK_LEFTBRACKET: return shifted ? '{' : '[';
        case SDLK_RIGHTBRACKET: return shifted ? '}' : ']';
        case SDLK_BACKSLASH: return shifted ? '|' : '\\';
        case SDLK_SEMICOLON: return shifted ? ':' : ';';
        case SDLK_QUOTE: return shifted ? '"' : '\'';
        case SDLK_COMMA: return shifted ? '<' : ',';
        case SDLK_PERIOD: return shifted ? '>' : '.';
        case SDLK_SLASH: return shifted ? '?' : '/';
        case SDLK_BACKQUOTE: return shifted ? '~' : '`';
        case SDLK_KP_PLUS: return '+';
        case SDLK_KP_MINUS: return '-';
        case SDLK_KP_MULTIPLY: return '*';
        case SDLK_KP_DIVIDE: return '/';
        case SDLK_KP_PERIOD: return '.';
        case SDLK_KP_ENTER: return 13;
        default: return -1;
    }
}

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();

    bool smoke_test = false;
    int smoke_frames = 120;
    bool export_diagnostics_on_exit = false;
    std::string diagnostics_override;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke-test") {
            smoke_test = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            smoke_frames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--export-diagnostics" && i + 1 < argc) {
            export_diagnostics_on_exit = true;
            diagnostics_override = argv[++i];
        } else {
            positional.push_back(arg);
        }
    }

    if (smoke_test) {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path bundle_dir = executableDirectory(argv);
    const std::string boot_path =
        positional.size() >= 1 ? positional[0] : bundledPath(bundle_dir, "ternary-os.tboot");
    const std::string disk_path =
        positional.size() >= 2 ? positional[1] : bundledPath(bundle_dir, "ternary-os.tdisk");
    const std::string diagnostics_path =
        diagnostics_override.empty() ? bundledPath(bundle_dir, "diagnostics")
                                     : diagnostics_override;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = boot_path;
    config.disk_path = disk_path;
    config.profile_name = "minimum";
    config.debug_overlay = true;

    sandbox::host::TosRuntime runtime(config);
    std::string error;
    if (!runtime.loadImage(&error)) {
        std::cerr << error << "\n";
        SDL_Quit();
        return EXIT_FAILURE;
    }
    if (!runtime.start()) {
        std::cerr << "failed to start runtime\n";
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_Window* window = SDL_CreateWindow("OS 3",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          1920,
                                          1080,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    SDL_StartTextInput();

    TextureState texture;
    bool running = true;
    bool debug_overlay = false;
    long long mouse_buttons = 0;
    int rendered_frames = 0;
    bool smoke_failed = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_TEXTINPUT:
                    // Keydown handles the ASCII subset used by the guest OS. Keeping
                    // text input disabled here avoids duplicate characters on Windows.
                    break;
                case SDL_KEYDOWN: {
                    const SDL_Keycode key = event.key.keysym.sym;
                    const SDL_Keymod mods = static_cast<SDL_Keymod>(event.key.keysym.mod);
                    const bool host_command = hasHostModifier(mods);
                    if (host_command && key == SDLK_q) {
                        running = false;
                    } else if (host_command && key == SDLK_SPACE) {
                        if (runtime.paused()) runtime.resume();
                        else runtime.pause();
                    } else if (host_command && key == SDLK_r) {
                        if (!runtime.reset(&error)) std::cerr << error << "\n";
                        else if (!runtime.start()) std::cerr << "failed to restart runtime\n";
                    } else if (host_command && key == SDLK_d) {
                        if (!runtime.exportDiagnostics(diagnostics_path, &error)) {
                            std::cerr << error << "\n";
                        }
                    } else if (key == SDLK_F1) {
                        debug_overlay = !debug_overlay;
                    } else if (key == SDLK_BACKSPACE) {
                        runtime.pushKeyboardInput(8);
                    } else if (key == SDLK_RETURN) {
                        runtime.pushKeyboardInput(13);
                    } else if (key == SDLK_ESCAPE) {
                        runtime.pushKeyboardInput(27);
                    } else {
                        const long long ascii = asciiFromKey(key, mods);
                        if (ascii >= 0) runtime.pushKeyboardInput(ascii);
                    }
                    break;
                }
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) mouse_buttons |= 1;
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) mouse_buttons &= ~1LL;
                    break;
                default:
                    break;
            }
        }

        const sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        const auto [texture_width, texture_height] = textureDimensions(framebuffer);
        const SDL_Rect dest =
            letterboxRect(window_w, window_h, texture_width, texture_height);

        int mouse_x = 0;
        int mouse_y = 0;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        long long guest_x = 0;
        long long guest_y = 0;
        mapMouseToGuest(dest, framebuffer.width, framebuffer.height, mouse_x, mouse_y,
                        guest_x, guest_y);
        runtime.updateMouseState(guest_x, guest_y, mouse_buttons);

        if (!updateTexture(renderer, texture, framebuffer)) {
            std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << "\n";
            running = false;
            break;
        }

        SDL_SetRenderDrawColor(renderer, 5, 8, 20, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture.texture, nullptr, &dest);
        SDL_RenderPresent(renderer);

        const sandbox::host::TosRuntimeSnapshot snapshot = runtime.snapshot();
        SDL_SetWindowTitle(window,
                           statusTitle(snapshot, runtime.paused(), debug_overlay).c_str());
        if (smoke_test) {
            if (snapshot.status == sandbox::vm::VMStatus::TRAPPED) {
                std::cerr << "smoke test failed: VM trapped at PC " << snapshot.pc << "\n";
                smoke_failed = true;
                running = false;
            } else if (++rendered_frames >= smoke_frames) {
                std::cout << "SDL smoke test rendered " << rendered_frames << " frame(s)\n";
                running = false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (export_diagnostics_on_exit || smoke_failed) {
        if (!runtime.exportDiagnostics(diagnostics_path, &error)) {
            std::cerr << error << "\n";
            smoke_failed = true;
        }
    }

    runtime.shutdown();
    destroyTexture(texture);
    SDL_StopTextInput();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return smoke_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
