#include "ternary_host_runtime.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TextureState {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

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

bool updateTexture(SDL_Renderer* renderer,
                   TextureState& texture,
                   const sandbox::host::TosFramebufferSnapshot& framebuffer) {
    if (!texture.texture ||
        texture.width != framebuffer.width ||
        texture.height != framebuffer.height) {
        destroyTexture(texture);
        texture.texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            framebuffer.width,
                                            framebuffer.height);
        if (!texture.texture) return false;
        SDL_SetTextureBlendMode(texture.texture, SDL_BLENDMODE_NONE);
        texture.width = framebuffer.width;
        texture.height = framebuffer.height;
    }

    std::vector<std::uint32_t> pixels(framebuffer.rgba.size(), 0);
    for (std::size_t i = 0; i < framebuffer.rgba.size(); ++i) {
        pixels[i] = toSdlAbgr(framebuffer.rgba[i]);
    }
    return SDL_UpdateTexture(texture.texture,
                             nullptr,
                             pixels.data(),
                             framebuffer.width * static_cast<int>(sizeof(std::uint32_t))) == 0;
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

} // namespace

int main(int argc, char** argv) {
    sandbox::LongTriple::initPowTable();

    bool smoke_test = false;
    int smoke_frames = 120;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke-test") {
            smoke_test = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            smoke_frames = std::max(1, std::atoi(argv[++i]));
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
    const std::string diagnostics_path = bundledPath(bundle_dir, "diagnostics");

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
                    runtime.pushTextInput(event.text.text);
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
                        runtime.pushKeyboardInput(10);
                    } else if (key == SDLK_ESCAPE) {
                        runtime.pushKeyboardInput(27);
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
        const SDL_Rect dest =
            letterboxRect(window_w, window_h, framebuffer.width, framebuffer.height);

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

    runtime.shutdown();
    destroyTexture(texture);
    SDL_StopTextInput();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return smoke_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
