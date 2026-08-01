#include "ternary_host_runtime.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
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
    std::vector<std::uint32_t> pixels;
};

struct RenderMetrics {
    std::uint64_t dirty_pixels = 0;
    int dirty_rects = 0;
    double render_ms = 0.0;
    double fps = 0.0;
    bool texture_updated = false;
    bool skipped = false;
};

struct TextRenderCache {
    std::vector<long long> words;
    long long sprite_x = 0;
    long long sprite_y = 0;
    long long sprite_attr = 0;
    bool valid = false;
};

struct PixelRenderCache {
    std::vector<long long> words;
    bool valid = false;
};

struct FrameRenderState {
    TextureState texture;
    sandbox::host::TosFramebufferMode mode =
        sandbox::host::TosFramebufferMode::Text80x25;
    TextRenderCache text;
    PixelRenderCache pixels;
    bool valid = false;
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
    state.pixels.clear();
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

template <typename Framebuffer>
std::pair<int, int> textureDimensions(const Framebuffer& framebuffer) {
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

bool ensureTexture(SDL_Renderer* renderer,
                   TextureState& texture,
                   int width,
                   int height,
                   bool& recreated) {
    recreated = false;
    if (width <= 0 || height <= 0) return false;
    if (texture.texture && texture.width == width && texture.height == height) {
        return true;
    }

    destroyTexture(texture);
    texture.texture = SDL_CreateTexture(renderer,
                                        SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        width,
                                        height);
    if (!texture.texture) return false;
    SDL_SetTextureBlendMode(texture.texture, SDL_BLENDMODE_NONE);
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(static_cast<std::size_t>(width * height),
                          toSdlAbgr(kTextBackgroundRgba));
    recreated = true;
    return true;
}

long long framebufferWordAt(const sandbox::host::TosFramebufferMemorySnapshot& framebuffer,
                            int index) {
    if (index < 0 || index >= static_cast<int>(framebuffer.words.size())) return 0;
    return framebuffer.words[static_cast<std::size_t>(index)];
}

char glyphFromTextWord(long long value) {
    const char ch = static_cast<char>(value & 0xff);
    return (ch >= 32 && ch <= 126) ? ch : ' ';
}

std::uint32_t colorFromTextWord(long long value) {
    const int color = static_cast<int>((value >> 8) & 0x0f);
    return sandbox::host::detail::paletteColor(color);
}

std::uint32_t colorFromGraphicsWord(long long value) {
    return value == 0 ? sandbox::host::detail::paletteColor(0)
                      : sandbox::host::detail::paletteColor(static_cast<int>(value & 0x0f));
}

void addDirtyRect(std::vector<SDL_Rect>& rects,
                  RenderMetrics& metrics,
                  int x,
                  int y,
                  int w,
                  int h) {
    if (w <= 0 || h <= 0) return;
    rects.push_back(SDL_Rect{x, y, w, h});
    metrics.dirty_pixels += static_cast<std::uint64_t>(w) *
                            static_cast<std::uint64_t>(h);
}

bool uploadDirtyRects(TextureState& texture, const std::vector<SDL_Rect>& rects) {
    for (const SDL_Rect& rect : rects) {
        const std::uint32_t* src =
            texture.pixels.data() +
            static_cast<std::size_t>(rect.y * texture.width + rect.x);
        if (SDL_UpdateTexture(texture.texture,
                              &rect,
                              src,
                              texture.width * static_cast<int>(sizeof(std::uint32_t))) != 0) {
            return false;
        }
    }
    return true;
}

void renderTextCell(TextureState& texture,
                    const sandbox::host::TosFramebufferMemorySnapshot& framebuffer,
                    int cell_x,
                    int cell_y) {
    const int index = cell_y * framebuffer.width + cell_x;
    const long long word = framebufferWordAt(framebuffer, index);
    const char ch = glyphFromTextWord(word);
    const std::uint32_t fg = colorFromTextWord(word);
    const int px = cell_x * kTextCellWidth;
    const int py = cell_y * kTextCellHeight;
    const bool is_space = ch == ' ';
    const std::uint32_t bg =
        is_space && fg != kTextBackgroundRgba ? fg : kTextBackgroundRgba;
    fillRect(texture.pixels,
             texture.width,
             texture.height,
             px,
             py,
             kTextCellWidth,
             kTextCellHeight,
             toSdlAbgr(bg));
    if (!is_space) {
        drawGlyph(texture.pixels,
                  texture.width,
                  texture.height,
                  px,
                  py,
                  ch,
                  toSdlAbgr(fg));
    }
}

bool spriteVisible(long long sprite_attr) {
    const char ch = static_cast<char>(sprite_attr & 0xff);
    return ch >= 32 && ch <= 126;
}

void markTextCell(std::vector<std::uint8_t>& dirty_cells,
                  int width,
                  int height,
                  int cell_x,
                  int cell_y) {
    if (cell_x < 0 || cell_x >= width || cell_y < 0 || cell_y >= height) return;
    dirty_cells[static_cast<std::size_t>(cell_y * width + cell_x)] = 1;
}

void drawTextSprite(TextureState& texture,
                    const sandbox::host::TosFramebufferMemorySnapshot& framebuffer) {
    if (!spriteVisible(framebuffer.sprite_attr)) return;
    const char sprite_ch = static_cast<char>(framebuffer.sprite_attr & 0xff);
    const int sprite_color = static_cast<int>((framebuffer.sprite_attr >> 8) & 0x0f);
    drawGlyph(texture.pixels,
              texture.width,
              texture.height,
              static_cast<int>(framebuffer.sprite_x) * kTextCellWidth,
              static_cast<int>(framebuffer.sprite_y) * kTextCellHeight,
              sprite_ch,
              toSdlAbgr(sandbox::host::detail::paletteColor(sprite_color)));
}

void updateTextCache(TextRenderCache& cache,
                     const sandbox::host::TosFramebufferMemorySnapshot& framebuffer) {
    const int cells = framebuffer.width * framebuffer.height;
    cache.words.assign(static_cast<std::size_t>(cells), 0);
    for (int i = 0; i < cells; ++i) {
        cache.words[static_cast<std::size_t>(i)] = framebufferWordAt(framebuffer, i);
    }
    cache.sprite_x = framebuffer.sprite_x;
    cache.sprite_y = framebuffer.sprite_y;
    cache.sprite_attr = framebuffer.sprite_attr;
    cache.valid = true;
}

void renderTextDirty(TextureState& texture,
                     TextRenderCache& cache,
                     const sandbox::host::TosFramebufferMemorySnapshot& framebuffer,
                     bool full_render,
                     std::vector<SDL_Rect>& rects,
                     RenderMetrics& metrics) {
    const int cells = framebuffer.width * framebuffer.height;
    std::vector<std::uint8_t> dirty_cells(static_cast<std::size_t>(cells),
                                          full_render ? 1 : 0);

    if (!full_render) {
        if (!cache.valid || static_cast<int>(cache.words.size()) != cells) {
            std::fill(dirty_cells.begin(), dirty_cells.end(), 1);
        } else {
            for (int i = 0; i < cells; ++i) {
                if (framebufferWordAt(framebuffer, i) != cache.words[static_cast<std::size_t>(i)]) {
                    dirty_cells[static_cast<std::size_t>(i)] = 1;
                }
            }
            if (spriteVisible(cache.sprite_attr)) {
                markTextCell(dirty_cells,
                             framebuffer.width,
                             framebuffer.height,
                             static_cast<int>(cache.sprite_x),
                             static_cast<int>(cache.sprite_y));
            }
            if (spriteVisible(framebuffer.sprite_attr)) {
                markTextCell(dirty_cells,
                             framebuffer.width,
                             framebuffer.height,
                             static_cast<int>(framebuffer.sprite_x),
                             static_cast<int>(framebuffer.sprite_y));
            }
        }
    }

    for (int cy = 0; cy < framebuffer.height; ++cy) {
        int run_start = -1;
        for (int cx = 0; cx <= framebuffer.width; ++cx) {
            const bool dirty =
                cx < framebuffer.width &&
                dirty_cells[static_cast<std::size_t>(cy * framebuffer.width + cx)] != 0;
            if (dirty && run_start < 0) {
                run_start = cx;
            } else if (!dirty && run_start >= 0) {
                for (int draw_x = run_start; draw_x < cx; ++draw_x) {
                    renderTextCell(texture, framebuffer, draw_x, cy);
                }
                addDirtyRect(rects,
                             metrics,
                             run_start * kTextCellWidth,
                             cy * kTextCellHeight,
                             (cx - run_start) * kTextCellWidth,
                             kTextCellHeight);
                run_start = -1;
            }
        }
    }

    drawTextSprite(texture, framebuffer);
    updateTextCache(cache, framebuffer);
}

void updatePixelCache(PixelRenderCache& cache,
                      const sandbox::host::TosFramebufferMemorySnapshot& framebuffer) {
    const int count = framebuffer.width * framebuffer.height;
    cache.words.assign(static_cast<std::size_t>(count), 0);
    for (int i = 0; i < count; ++i) {
        cache.words[static_cast<std::size_t>(i)] = framebufferWordAt(framebuffer, i);
    }
    cache.valid = true;
}

void renderPixelsDirty(TextureState& texture,
                       PixelRenderCache& cache,
                       const sandbox::host::TosFramebufferMemorySnapshot& framebuffer,
                       bool full_render,
                       std::vector<SDL_Rect>& rects,
                       RenderMetrics& metrics) {
    const int width = framebuffer.width;
    const int height = framebuffer.height;
    const int count = width * height;
    if (full_render || !cache.valid || static_cast<int>(cache.words.size()) != count) {
        for (int i = 0; i < count; ++i) {
            texture.pixels[static_cast<std::size_t>(i)] =
                toSdlAbgr(colorFromGraphicsWord(framebufferWordAt(framebuffer, i)));
        }
        addDirtyRect(rects, metrics, 0, 0, width, height);
        updatePixelCache(cache, framebuffer);
        return;
    }

    for (int y = 0; y < height; ++y) {
        int run_start = -1;
        for (int x = 0; x <= width; ++x) {
            bool dirty = false;
            if (x < width) {
                const int index = y * width + x;
                dirty = framebufferWordAt(framebuffer, index) !=
                        cache.words[static_cast<std::size_t>(index)];
                if (dirty) {
                    texture.pixels[static_cast<std::size_t>(index)] =
                        toSdlAbgr(colorFromGraphicsWord(framebufferWordAt(framebuffer, index)));
                }
            }
            if (dirty && run_start < 0) {
                run_start = x;
            } else if (!dirty && run_start >= 0) {
                addDirtyRect(rects, metrics, run_start, y, x - run_start, 1);
                run_start = -1;
            }
        }
    }

    updatePixelCache(cache, framebuffer);
}

bool updateTexture(SDL_Renderer* renderer,
                   FrameRenderState& render_state,
                   const sandbox::host::TosFramebufferMemorySnapshot& framebuffer,
                   RenderMetrics& metrics) {
    using clock = std::chrono::steady_clock;
    metrics = RenderMetrics{};
    const auto started = clock::now();

    const auto [texture_width, texture_height] = textureDimensions(framebuffer);
    bool recreated = false;
    if (!ensureTexture(renderer,
                       render_state.texture,
                       texture_width,
                       texture_height,
                       recreated)) {
        return false;
    }

    const bool mode_changed =
        !render_state.valid || render_state.mode != framebuffer.mode || recreated;
    if (mode_changed) {
        render_state.text.valid = false;
        render_state.pixels.valid = false;
    }

    if (!framebuffer.changed && !mode_changed) {
        metrics.skipped = true;
        const auto finished = clock::now();
        metrics.render_ms =
            std::chrono::duration<double, std::milli>(finished - started).count();
        return true;
    }

    std::vector<SDL_Rect> dirty_rects;
    if (framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25) {
        renderTextDirty(render_state.texture,
                        render_state.text,
                        framebuffer,
                        mode_changed,
                        dirty_rects,
                        metrics);
    } else {
        renderPixelsDirty(render_state.texture,
                          render_state.pixels,
                          framebuffer,
                          mode_changed,
                          dirty_rects,
                          metrics);
    }

    metrics.dirty_rects = static_cast<int>(dirty_rects.size());
    metrics.texture_updated = !dirty_rects.empty();
    if (!uploadDirtyRects(render_state.texture, dirty_rects)) return false;
    render_state.mode = framebuffer.mode;
    render_state.valid = true;

    const auto finished = clock::now();
    metrics.render_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    return true;
}

std::string statusTitle(const sandbox::host::TosRuntimeSnapshot& snapshot,
                        bool paused,
                        bool debug_overlay,
                        const RenderMetrics& metrics) {
    std::ostringstream out;
    out << "OS 3";
    if (debug_overlay) {
        out << " | PC " << snapshot.pc
            << " | " << sandbox::vm::vmStatusToString(snapshot.status)
            << " | cycles " << snapshot.cycles
            << " | mode " << snapshot.gpu_mode
            << " | fps " << std::fixed << std::setprecision(1) << metrics.fps
            << " | dirty " << metrics.dirty_pixels << "/" << metrics.dirty_rects
            << " | render " << std::setprecision(2) << metrics.render_ms << "ms";
        if (paused) out << " | paused";
        if (!snapshot.image_version.empty()) out << " | " << snapshot.image_version;
    }
    return out.str();
}

std::string metricsOverlayText(const RenderMetrics& metrics) {
    std::ostringstream out;
    out << "FPS " << std::fixed << std::setprecision(1) << metrics.fps
        << "  DIRTY " << metrics.dirty_pixels << "/" << metrics.dirty_rects
        << "  RENDER " << std::setprecision(2) << metrics.render_ms << "MS";
    return out.str();
}

void drawDebugGlyph(SDL_Renderer* renderer,
                    int x,
                    int y,
                    char ch,
                    int scale,
                    SDL_Color color) {
    const auto rows = glyphRows(ch);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int row = 0; row < kGlyphHeight; ++row) {
        for (int col = 0; col < kGlyphWidth; ++col) {
            if ((rows[static_cast<std::size_t>(row)] & (1 << (kGlyphWidth - col - 1))) == 0) {
                continue;
            }
            SDL_Rect pixel{x + col * scale, y + row * scale, scale, scale};
            SDL_RenderFillRect(renderer, &pixel);
        }
    }
}

void drawDebugText(SDL_Renderer* renderer,
                   int x,
                   int y,
                   const std::string& text,
                   int scale,
                   SDL_Color color) {
    const int advance = (kGlyphWidth + 1) * scale;
    int pen_x = x;
    for (char ch : text) {
        if (ch != ' ') {
            drawDebugGlyph(renderer, pen_x, y, ch, scale, color);
        }
        pen_x += advance;
    }
}

void drawDebugOverlay(SDL_Renderer* renderer, const RenderMetrics& metrics) {
    const std::string text = metricsOverlayText(metrics);
    const int scale = 2;
    const int advance = (kGlyphWidth + 1) * scale;
    SDL_Rect background{8,
                        8,
                        static_cast<int>(text.size()) * advance + 10,
                        kGlyphHeight * scale + 10};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_RenderFillRect(renderer, &background);
    drawDebugText(renderer, 13, 13, text, scale, SDL_Color{232, 244, 255, 255});
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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
    config.record_syscall_trace = export_diagnostics_on_exit;

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

    FrameRenderState render_state;
    RenderMetrics metrics;
    bool running = true;
    bool debug_overlay = false;
    long long mouse_buttons = 0;
    int rendered_frames = 0;
    bool smoke_failed = false;
    bool force_present = true;
    std::uint64_t framebuffer_revision = 0;
    int guest_frame_w = 80;
    int guest_frame_h = 25;
    int source_w = 80 * kTextCellWidth;
    int source_h = 25 * kTextCellHeight;

    using Clock = std::chrono::steady_clock;
    const auto present_interval = std::chrono::microseconds(16667);
    auto next_present = Clock::now();
    auto fps_window_start = Clock::now();
    int fps_window_frames = 0;
    double fps = 0.0;

    auto notePresentedFrame = [&](Clock::time_point now) {
        ++fps_window_frames;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - fps_window_start);
        if (elapsed.count() >= 1000000) {
            fps = (static_cast<double>(fps_window_frames) * 1000000.0) /
                  static_cast<double>(elapsed.count());
            fps_window_frames = 0;
            fps_window_start = now;
        }
    };

    auto scheduleNextPresent = [&](Clock::time_point now) {
        if (next_present > now) return;
        do {
            next_present += present_interval;
        } while (next_present <= now);
    };

    auto resetRenderCache = [&]() {
        destroyTexture(render_state.texture);
        render_state = FrameRenderState{};
        framebuffer_revision = 0;
        force_present = true;
        next_present = Clock::now();
    };

    auto handleEvent = [&](const SDL_Event& event) {
        switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                    event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    force_present = true;
                }
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
                    force_present = true;
                } else if (host_command && key == SDLK_r) {
                    if (!runtime.reset(&error)) {
                        std::cerr << error << "\n";
                    } else if (!runtime.start()) {
                        std::cerr << "failed to restart runtime\n";
                    } else {
                        resetRenderCache();
                    }
                } else if (host_command && key == SDLK_d) {
                    if (!runtime.exportDiagnostics(diagnostics_path, &error)) {
                        std::cerr << error << "\n";
                    }
                } else if (key == SDLK_F1) {
                    debug_overlay = !debug_overlay;
                    force_present = true;
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
    };

    while (running) {
        SDL_Event event;
        const auto before_wait = Clock::now();
        int wait_ms = 0;
        if (before_wait < next_present) {
            wait_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    next_present - before_wait)
                    .count());
            if (wait_ms <= 0) wait_ms = 1;
        }
        if (wait_ms > 0 && SDL_WaitEventTimeout(&event, wait_ms)) {
            handleEvent(event);
        }
        while (SDL_PollEvent(&event)) {
            handleEvent(event);
        }
        if (!running) break;

        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        SDL_Rect dest = letterboxRect(window_w, window_h, source_w, source_h);

        int mouse_x = 0;
        int mouse_y = 0;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        long long guest_x = 0;
        long long guest_y = 0;
        mapMouseToGuest(dest, guest_frame_w, guest_frame_h, mouse_x, mouse_y, guest_x, guest_y);
        runtime.updateMouseState(guest_x, guest_y, mouse_buttons);

        const auto now = Clock::now();
        if (now < next_present) {
            continue;
        }

        sandbox::host::TosFramebufferMemorySnapshot framebuffer =
            runtime.readFramebufferMemory(framebuffer_revision);
        const auto [texture_width, texture_height] = textureDimensions(framebuffer);
        guest_frame_w = framebuffer.width;
        guest_frame_h = framebuffer.height;
        source_w = texture_width;
        source_h = texture_height;
        dest = letterboxRect(window_w, window_h, source_w, source_h);

        if (!updateTexture(renderer, render_state, framebuffer, metrics)) {
            std::cerr << "SDL_UpdateTexture failed: " << SDL_GetError() << "\n";
            running = false;
            break;
        }
        if (framebuffer.changed) framebuffer_revision = framebuffer.revision;
        metrics.fps = fps;

        const bool should_present =
            render_state.texture.texture != nullptr &&
            (metrics.texture_updated || force_present || debug_overlay || smoke_test);
        if (should_present) {
            const auto present_time = Clock::now();
            notePresentedFrame(present_time);
            metrics.fps = fps;

            SDL_SetRenderDrawColor(renderer, 5, 8, 20, 255);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, render_state.texture.texture, nullptr, &dest);
            if (debug_overlay) {
                drawDebugOverlay(renderer, metrics);
            }
            SDL_RenderPresent(renderer);
            force_present = false;

            const sandbox::host::TosRuntimeSnapshot snapshot = runtime.snapshot();
            SDL_SetWindowTitle(window,
                               statusTitle(snapshot, runtime.paused(), debug_overlay, metrics)
                                   .c_str());
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
        } else {
            force_present = false;
        }

        scheduleNextPresent(Clock::now());
    }

    if (export_diagnostics_on_exit || smoke_failed) {
        if (!runtime.exportDiagnostics(diagnostics_path, &error)) {
            std::cerr << error << "\n";
            smoke_failed = true;
        }
    }

    runtime.shutdown();
    destroyTexture(render_state.texture);
    SDL_StopTextInput();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return smoke_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
