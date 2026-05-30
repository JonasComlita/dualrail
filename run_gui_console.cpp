#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include "ternary_asm.h"
#include "ternary_vm.h"

// Global VM state accessible to the window paint loop
static sandbox::vm::VMState* g_vm = nullptr;

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

static std::thread g_vm_thread;
static std::atomic<bool> g_vm_thread_running{false};
static std::mutex g_vm_mutex;

#include <fstream>
#include "ternary_compiler.h"

namespace {

std::string readTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

sandbox::compiler::LinkResult compileApp(const std::string& app_name, int stack_words = 256) {
    using namespace sandbox::compiler;
    const std::string sdk = readTextFile("apps/os_sdk.trit");
    const std::string app = readTextFile("apps/" + app_name + ".trit");
    CompileResult compiled = compileSource(app_name + ".trit", sdk + "\n" + app);
    if (!compiled.success) {
        std::string err = "Failed to compile " + app_name + ":\n";
        for (const auto& diag : compiled.diagnostics) {
            err += diag.format() + "\n";
        }
        MessageBoxA(NULL, err.c_str(), "Compile Error", MB_ICONERROR);
    }
    LinkOptions options;
    options.stack_hint_words = stack_words;
    options.standalone_halt_on_exit = false;
    LinkResult linked = linkModules({compiled.object}, options);
    if (!linked.success) {
        MessageBoxA(NULL, ("Failed to link " + app_name).c_str(), "Link Error", MB_ICONERROR);
    }
    return linked;
}

void appendStoreWord(std::ostringstream& out, int addr, int offset, int value) {
    out << "    mov r1, " << value << "\n";
    out << "    mov r2, " << addr << "\n";
    out << "    store r1, r2, " << offset << "\n";
}

void appendStoreCString(std::ostringstream& out, int addr, const std::string& text) {
    for (int i = 0; i < static_cast<int>(text.size()); ++i) {
        appendStoreWord(out, addr, i, static_cast<unsigned char>(text[static_cast<std::size_t>(i)]));
    }
    appendStoreWord(out, addr, static_cast<int>(text.size()), 0);
}

std::string buildBootAssembly(
    const sandbox::compiler::LinkResult& desktop,
    const sandbox::compiler::LinkResult& calc,
    const sandbox::compiler::LinkResult& task,
    const sandbox::compiler::LinkResult& paint,
    int kDesktopPhys,
    int kCalcTextPpn,
    int kTaskTextPpn,
    int kPaintTextPpn) {

    std::ostringstream boot;
    boot << ".text\n";
    boot << "boot:\n";
    boot << "    mov sp, 16383\n";
    boot << "    call kernel_init\n";
    boot << "    mov r1, native_trap_entry\n";
    boot << "    csrw tvec, r1\n";

    // Create /bin
    appendStoreCString(boot, 10000, "/bin");
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10000\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_create\n";

    // 4. Desktop
    int kDesktopPpn = kDesktopPhys / sandbox::vm::MMU_PAGE_WORDS;
    appendStoreCString(boot, 10020, "/bin/desktop");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, desktop.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, desktop.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, desktop.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, desktop.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, desktop.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, desktop.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kDesktopPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 1\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    // 1. Calculator
    appendStoreCString(boot, 10020, "/bin/calculator");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, calc.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, calc.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, calc.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, calc.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, calc.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, calc.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kCalcTextPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 1\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    // 2. Task Manager
    appendStoreCString(boot, 10020, "/bin/task_manager");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, task.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, task.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, task.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, task.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, task.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, task.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kTaskTextPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 1\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    // 3. Paint
    appendStoreCString(boot, 10020, "/bin/paint");
    appendStoreWord(boot, 10100, 0, sandbox::vm::EXEC_MAGIC);
    appendStoreWord(boot, 10100, 1, sandbox::vm::EXEC_VERSION_V1);
    appendStoreWord(boot, 10100, 2, sandbox::vm::EXEC_ABI_VERSION_V1);
    appendStoreWord(boot, 10100, 3, paint.executable_header.entry_virtual_pc);
    appendStoreWord(boot, 10100, 4, paint.executable_header.text_pages);
    appendStoreWord(boot, 10100, 5, paint.executable_header.data_pages);
    appendStoreWord(boot, 10100, 6, paint.executable_header.stack_words);
    appendStoreWord(boot, 10100, 7, paint.executable_header.syscall_abi_version);
    appendStoreWord(boot, 10100, 8, paint.executable_header.flags);
    appendStoreWord(boot, 10100, 9, kPaintTextPpn);

    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 3\n";
    boot << "    call vfs_create\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 2\n";
    boot << "    call vfs_open\n";
    boot << "    mov r2, 10250\n";
    boot << "    store r13, r2, 0\n";
    boot << "    mov r13, 1\n";
    boot << "    load r14, r2, 0\n";
    boot << "    mov r15, 10100\n";
    boot << "    mov r16, 10\n";
    boot << "    call vfs_write\n";
    boot << "    mov r13, 1\n";
    boot << "    mov r2, 10250\n";
    boot << "    load r14, r2, 0\n";
    boot << "    call vfs_close\n";
    boot << "    mov r13, 0\n";
    boot << "    mov r14, 10020\n";
    boot << "    mov r15, 1\n";
    boot << "    mov r16, 0\n";
    boot << "    mov r17, 1\n";
    boot << "    mov r18, 128\n";
    boot << "    call app_register\n";

    // Setup desktop start context
    boot << "    mov r1, ctx_desktop\n";
    boot << "    csrw scratch, r1\n";
    boot << "    mov sp, 12000\n";
    boot << "    mov r1, " << kDesktopPhys << "\n";
    boot << "    csrw epc, r1\n";
    boot << "    mov r1, 35\n"; // user mode, interrupt disabled
    boot << "    csrw status, r1\n";
    boot << "    eret\n";
    boot << ".data\n";
    boot << ".org 64\n";
    boot << "ctx_desktop: .word 0, 35, 0, 0, 0, 0\n";
    boot << ".org 95\n";
    boot << ".word 12000\n";
    return boot.str();
}

} // namespace

// Structures for display and GDI resource caching
struct DisplayCache {
    int gpu_mode = -1;
    int gpu_page = -1;
    long long sprite_attr = -1;
    int sprite_x = -1;
    int sprite_y = -1;
    int width = -1;
    int height = -1;
    std::vector<sandbox::vm::TernaryValue> raw_vram;
    std::vector<long long> decoded_vram;
} g_display_cache;

struct GDICache {
    HDC memDC = NULL;
    HBITMAP memBitmap = NULL;
    HBITMAP oldBitmap = NULL;

    HBITMAP overlayBitmap = NULL;

    HFONT textFont = NULL;
    HFONT spriteFontText = NULL;
    HFONT spriteFontGfx = NULL;

    HBRUSH bgBrush = NULL;
    HBRUSH paletteBrushes[16] = { NULL };
    HPEN borderPen = NULL;

    HBITMAP gfxDIB = NULL;
    uint32_t* gfxDIBPixels = nullptr;

    int lastWidth = 0;
    int lastHeight = 0;
} g_gdi;

// Helper Cyberpunk color palette builder
static COLORREF getCyberColor(int idx) {
    switch (idx & 0x0F) {
        case 0:  return RGB(8, 16, 10);     // Deep Phosphor Black
        case 1:  return RGB(0, 255, 70);    // CRT Phosphor Green
        case 2:  return RGB(0, 100, 30);    // CRT Shadow Green
        case 3:  return RGB(255, 120, 0);   // Neon Amber
        case 4:  return RGB(100, 50, 0);    // CRT Shadow Amber
        case 5:  return RGB(0, 240, 255);   // Neon Cyan
        case 6:  return RGB(255, 0, 128);   // Cyber Hot Pink
        case 7:  return RGB(255, 255, 255); // Pure White
        case 8:  return RGB(60, 70, 60);    // Dark Shadow Green
        case 9:  return RGB(255, 80, 80);    // Neon Red
        case 10: return RGB(0, 255, 70);    // Light Green (used for desktop menu)
        case 11: return RGB(0, 240, 255);   // Neon Cyan (used for desktop title)
        case 12: return RGB(255, 0, 128);   // Cyber Hot Pink
        case 13: return RGB(255, 120, 0);   // Neon Amber
        case 14: return RGB(255, 255, 0);   // Bright Yellow (used for paint app)
        case 15: return RGB(255, 255, 255); // Bright White
        default: return RGB(0, 255, 70);
    }
}

void clearGdiCache() {
    if (g_gdi.memDC) {
        if (g_gdi.oldBitmap) {
            SelectObject(g_gdi.memDC, g_gdi.oldBitmap);
            g_gdi.oldBitmap = NULL;
        }
        DeleteDC(g_gdi.memDC);
        g_gdi.memDC = NULL;
    }
    if (g_gdi.memBitmap) {
        DeleteObject(g_gdi.memBitmap);
        g_gdi.memBitmap = NULL;
    }
    if (g_gdi.overlayBitmap) {
        DeleteObject(g_gdi.overlayBitmap);
        g_gdi.overlayBitmap = NULL;
    }
    if (g_gdi.textFont) {
        DeleteObject(g_gdi.textFont);
        g_gdi.textFont = NULL;
    }
    if (g_gdi.spriteFontText) {
        DeleteObject(g_gdi.spriteFontText);
        g_gdi.spriteFontText = NULL;
    }
    if (g_gdi.spriteFontGfx) {
        DeleteObject(g_gdi.spriteFontGfx);
        g_gdi.spriteFontGfx = NULL;
    }
    if (g_gdi.bgBrush) {
        DeleteObject(g_gdi.bgBrush);
        g_gdi.bgBrush = NULL;
    }
    for (int i = 0; i < 16; ++i) {
        if (g_gdi.paletteBrushes[i]) {
            DeleteObject(g_gdi.paletteBrushes[i]);
            g_gdi.paletteBrushes[i] = NULL;
        }
    }
    if (g_gdi.borderPen) {
        DeleteObject(g_gdi.borderPen);
        g_gdi.borderPen = NULL;
    }
    if (g_gdi.gfxDIB) {
        DeleteObject(g_gdi.gfxDIB);
        g_gdi.gfxDIB = NULL;
        g_gdi.gfxDIBPixels = nullptr;
    }
}

void updateGdiCache(HWND hwnd, HDC hdc, int width, int height) {
    if (width == g_gdi.lastWidth && height == g_gdi.lastHeight && g_gdi.memDC != NULL) {
        return;
    }

    clearGdiCache();

    g_gdi.lastWidth = width;
    g_gdi.lastHeight = height;

    g_gdi.memDC = CreateCompatibleDC(hdc);
    g_gdi.memBitmap = CreateCompatibleBitmap(hdc, width, height);
    g_gdi.oldBitmap = (HBITMAP)SelectObject(g_gdi.memDC, g_gdi.memBitmap);

    g_gdi.bgBrush = CreateSolidBrush(RGB(8, 16, 10));

    for (int i = 0; i < 16; ++i) {
        g_gdi.paletteBrushes[i] = CreateSolidBrush(getCyberColor(i));
    }

    // Text fonts
    double scaleTextX = (double)width / 80.0;
    double scaleTextY = (double)height / 25.0;
    g_gdi.textFont = CreateFontA(
        static_cast<int>(scaleTextY + 2), static_cast<int>(scaleTextX + 1),
        0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Consolas"
    );

    double sy_text = (double)height / 25.0;
    double sy_gfx = (double)height / 60.0;
    double sx = (double)width / 80.0;

    g_gdi.spriteFontText = CreateFontA(
        static_cast<int>(sy_text * 1.5), static_cast<int>(sx * 1.2),
        0, 0, FW_EXTRABOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Consolas"
    );
    g_gdi.spriteFontGfx = CreateFontA(
        static_cast<int>(sy_gfx * 1.5), static_cast<int>(sx * 1.2),
        0, 0, FW_EXTRABOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Consolas"
    );

    g_gdi.borderPen = CreatePen(PS_SOLID, 4, RGB(0, 255, 70));

    // DIB Section for GFX mode
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 80;
    bmi.bmiHeader.biHeight = -60; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_gdi.gfxDIB = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void**)&g_gdi.gfxDIBPixels, NULL, 0);

    // Static Overlay (Scanlines)
    HDC overlayDC = CreateCompatibleDC(hdc);
    g_gdi.overlayBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldOverlay = (HBITMAP)SelectObject(overlayDC, g_gdi.overlayBitmap);

    HBRUSH whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    RECT r = {0, 0, width, height};
    FillRect(overlayDC, &r, whiteBrush);
    DeleteObject(whiteBrush);

    HPEN scanPen = CreatePen(PS_SOLID, 1, RGB(180, 200, 180));
    HPEN oldScanPen = (HPEN)SelectObject(overlayDC, scanPen);
    for (int y = 0; y < height; y += 3) {
        MoveToEx(overlayDC, 0, y, NULL);
        LineTo(overlayDC, width, y);
    }
    SelectObject(overlayDC, oldScanPen);
    DeleteObject(scanPen);

    SelectObject(overlayDC, oldOverlay);
    DeleteDC(overlayDC);
}

bool checkDisplayDirty(HWND hwnd, int width, int height) {
    if (!g_vm) return false;

    std::lock_guard<std::mutex> lock(g_vm_mutex);
    bool dirty = false;

    int current_mode = g_vm->gpu_mode;
    int current_page = g_vm->gpu_page;
    long long current_sprite_attr = g_vm->sprite_attr;
    int current_sprite_x = g_vm->sprite_x;
    int current_sprite_y = g_vm->sprite_y;

    if (current_mode != g_display_cache.gpu_mode) {
        g_display_cache.gpu_mode = current_mode;
        dirty = true;
    }
    if (current_mode != 0 && current_page != g_display_cache.gpu_page) {
        g_display_cache.gpu_page = current_page;
        dirty = true;
    }
    if (current_sprite_attr != g_display_cache.sprite_attr) {
        g_display_cache.sprite_attr = current_sprite_attr;
        dirty = true;
    }
    if (current_sprite_attr != 0) {
        if (current_sprite_x != g_display_cache.sprite_x) {
            g_display_cache.sprite_x = current_sprite_x;
            dirty = true;
        }
        if (current_sprite_y != g_display_cache.sprite_y) {
            g_display_cache.sprite_y = current_sprite_y;
            dirty = true;
        }
    }
    if (width != g_display_cache.width || height != g_display_cache.height) {
        g_display_cache.width = width;
        g_display_cache.height = height;
        dirty = true;
    }

    int visible_base = (current_mode == 0) ? 60000 : ((current_page == 0) ? 50000 : 55000);
    int vram_size = (current_mode == 0) ? 2000 : 4800;

    if (g_display_cache.raw_vram.size() != static_cast<size_t>(vram_size)) {
        g_display_cache.raw_vram.assign(vram_size, sandbox::vm::TernaryValue::zero());
        g_display_cache.decoded_vram.assign(vram_size, 0);
        dirty = true;
    }

    for (int i = 0; i < vram_size; ++i) {
        int vram_addr = visible_base + i;
        if (vram_addr >= 0 && vram_addr < g_vm->dmem.size()) {
            const auto& current_val = g_vm->dmem.words[vram_addr];
            if (current_val != g_display_cache.raw_vram[i]) {
                g_display_cache.raw_vram[i] = current_val;
                g_display_cache.decoded_vram[i] = sandbox::vm::ops::toLong(current_val);
                dirty = true;
            }
        } else {
            if (g_display_cache.decoded_vram[i] != 0) {
                g_display_cache.raw_vram[i] = sandbox::vm::TernaryValue::zero();
                g_display_cache.decoded_vram[i] = 0;
                dirty = true;
            }
        }
    }

    return dirty;
}

// Window procedure to handle paint and ticks
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            SetTimer(hwnd, 1, 16, NULL); // ~60 FPS Timer
            return 0;
        }
        case WM_TIMER: {
            if (g_vm) {
                std::string current_syscall_out;
                int pc = 0;
                sandbox::vm::VMStatus status = sandbox::vm::VMStatus::HALTED;
                long long cycle_count = 0;
                sandbox::vm::PrivilegeMode privilege = sandbox::vm::PrivilegeMode::User;

                {
                    std::lock_guard<std::mutex> lock(g_vm_mutex);
                    pc = g_vm->pc;
                    status = g_vm->status;
                    cycle_count = g_vm->cycle_count;
                    privilege = g_vm->privilege;

                    static size_t last_pos = 0;
                    if (g_vm->syscall_buffer.size() > last_pos) {
                        current_syscall_out = g_vm->syscall_buffer.substr(last_pos);
                        last_pos = g_vm->syscall_buffer.size();
                    }
                }

                if (!current_syscall_out.empty()) {
                    std::cout << current_syscall_out << std::flush;
                }

                // Update window title with live diagnostic metadata
                wchar_t title_buf[256];
                swprintf_s(title_buf, L"Ternary VM | PC: %d | Status: %s | Cycles: %lld | Priv: %s",
                           pc,
                           (status == sandbox::vm::VMStatus::RUNNING) ? L"RUNNING" :
                           (status == sandbox::vm::VMStatus::HALTED) ? L"HALTED" : L"TRAPPED",
                           cycle_count,
                           (privilege == sandbox::vm::PrivilegeMode::Kernel) ? L"KERNEL" : L"USER");
                SetWindowTextW(hwnd, title_buf);

                RECT rect;
                GetClientRect(hwnd, &rect);
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                if (checkDisplayDirty(hwnd, width, height)) {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_vm) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                if (width > 0 && height > 0) {
                    std::lock_guard<std::mutex> lock(g_vm_mutex);
                    g_vm->mouse_x = (x * 80) / width;
                    g_vm->mouse_y = (g_vm->gpu_mode == 0) ? ((y * 25) / height) : ((y * 60) / height);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (g_vm) {
                std::lock_guard<std::mutex> lock(g_vm_mutex);
                g_vm->mouse_btn = 1;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_vm) {
                std::lock_guard<std::mutex> lock(g_vm_mutex);
                g_vm->mouse_btn = 0;
            }
            return 0;
        }
        case WM_CHAR: {
            if (g_vm) {
                std::lock_guard<std::mutex> lock(g_vm_mutex);
                g_vm->console_input.push_back(static_cast<long long>(wParam));
            }
            return 0;
        }
        case WM_SETCURSOR: {
            // Hide normal system pointer over client area to preserve Sprite custom visual pointer
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rect;
            GetClientRect(hwnd, &rect);
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;

            // Ensure GDI cache is up-to-date
            updateGdiCache(hwnd, hdc, width, height);

            // Fill base screen
            FillRect(g_gdi.memDC, &rect, g_gdi.bgBrush);

            if (g_vm) {
                // Ensure display cache is populated (e.g. if paint occurs before first timer tick)
                checkDisplayDirty(hwnd, width, height);

                if (g_display_cache.gpu_mode == 0) {
                    // 1. Text-Mode Tile Grid presentation (80 x 25 characters)
                    double scaleTextX = (double)width / 80.0;
                    double scaleTextY = (double)height / 25.0;

                    HFONT oldFont = (HFONT)SelectObject(g_gdi.memDC, g_gdi.textFont);
                    SetBkMode(g_gdi.memDC, TRANSPARENT);

                    for (int y = 0; y < 25; ++y) {
                        int run_start_x = -1;
                        int run_color_idx = -1;
                        std::string run_str;

                        for (int x = 0; x < 80; ++x) {
                            int idx = y * 80 + x;
                            long long val = g_display_cache.decoded_vram[idx];
                            char ch = static_cast<char>(val & 0xFF);
                            int color_idx = static_cast<int>((val >> 8) & 0x0F);

                            if (ch >= 32 && ch <= 126) {
                                if (run_start_x == -1) {
                                    run_start_x = x;
                                    run_color_idx = color_idx;
                                    run_str = ch;
                                } else if (color_idx == run_color_idx) {
                                    run_str += ch;
                                } else {
                                    COLORREF charColor = getCyberColor(run_color_idx);
                                    SetTextColor(g_gdi.memDC, charColor);
                                    int px = static_cast<int>(run_start_x * scaleTextX);
                                    int py = static_cast<int>(y * scaleTextY);
                                    TextOutA(g_gdi.memDC, px, py, run_str.c_str(), static_cast<int>(run_str.size()));

                                    run_start_x = x;
                                    run_color_idx = color_idx;
                                    run_str = ch;
                                }
                            } else {
                                if (run_start_x != -1) {
                                    COLORREF charColor = getCyberColor(run_color_idx);
                                    SetTextColor(g_gdi.memDC, charColor);
                                    int px = static_cast<int>(run_start_x * scaleTextX);
                                    int py = static_cast<int>(y * scaleTextY);
                                    TextOutA(g_gdi.memDC, px, py, run_str.c_str(), static_cast<int>(run_str.size()));
                                    run_start_x = -1;
                                    run_color_idx = -1;
                                    run_str.clear();
                                }
                            }
                        }

                        if (run_start_x != -1) {
                            COLORREF charColor = getCyberColor(run_color_idx);
                            SetTextColor(g_gdi.memDC, charColor);
                            int px = static_cast<int>(run_start_x * scaleTextX);
                            int py = static_cast<int>(y * scaleTextY);
                            TextOutA(g_gdi.memDC, px, py, run_str.c_str(), static_cast<int>(run_str.size()));
                        }
                    }

                    SelectObject(g_gdi.memDC, oldFont);

                } else {
                    // 2. Pixel Graphics mode (80 x 60 pixel grid) using DIB Section
                    if (g_gdi.gfxDIBPixels) {
                        for (int y = 0; y < 60; ++y) {
                            for (int x = 0; x < 80; ++x) {
                                int idx = y * 80 + x;
                                long long val = g_display_cache.decoded_vram[idx];
                                if (val != 0) {
                                    g_gdi.gfxDIBPixels[idx] = getCyberColor(static_cast<int>(val & 0x0F));
                                } else {
                                    g_gdi.gfxDIBPixels[idx] = RGB(8, 16, 10);
                                }
                            }
                        }

                        BITMAPINFO bmi = {};
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = 80;
                        bmi.bmiHeader.biHeight = -60; // top-down
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        StretchDIBits(
                            g_gdi.memDC,
                            0, 0, width, height,
                            0, 0, 80, 60,
                            g_gdi.gfxDIBPixels,
                            &bmi,
                            DIB_RGB_COLORS,
                            SRCCOPY
                        );
                    }
                }

                // 3. Hardware Sprite 0 Compositing
                if (g_display_cache.sprite_attr != 0) {
                    char glyph = static_cast<char>(g_display_cache.sprite_attr & 0xFF);
                    int color_idx = static_cast<int>((g_display_cache.sprite_attr >> 8) & 0x0F);
                    COLORREF spriteColor = getCyberColor(color_idx);

                    double sx = (double)width / 80.0;
                    double sy = (g_display_cache.gpu_mode == 0) ? ((double)height / 25.0) : ((double)height / 60.0);

                    HFONT hSpriteFont = (g_display_cache.gpu_mode == 0) ? g_gdi.spriteFontText : g_gdi.spriteFontGfx;
                    HFONT oldFontS = (HFONT)SelectObject(g_gdi.memDC, hSpriteFont);
                    SetBkMode(g_gdi.memDC, TRANSPARENT);
                    SetTextColor(g_gdi.memDC, spriteColor);

                    int px = static_cast<int>(g_display_cache.sprite_x * sx);
                    int py = static_cast<int>(g_display_cache.sprite_y * sy);

                    TextOutA(g_gdi.memDC, px, py, &glyph, 1);

                    SelectObject(g_gdi.memDC, oldFontS);
                }
            }

            // Apply CRT phosphor dynamic scanlines via static overlay
            if (g_gdi.overlayBitmap) {
                HDC overlayDC = CreateCompatibleDC(g_gdi.memDC);
                HBITMAP oldOverlay = (HBITMAP)SelectObject(overlayDC, g_gdi.overlayBitmap);
                BitBlt(g_gdi.memDC, 0, 0, width, height, overlayDC, 0, 0, SRCAND);
                SelectObject(overlayDC, oldOverlay);
                DeleteDC(overlayDC);
            }

            // High-tech active layout border
            HPEN oldPenB = (HPEN)SelectObject(g_gdi.memDC, g_gdi.borderPen);
            HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldBrush = (HBRUSH)SelectObject(g_gdi.memDC, nullBrush);
            Rectangle(g_gdi.memDC, 4, 4, width - 4, height - 4);
            SelectObject(g_gdi.memDC, oldBrush);
            SelectObject(g_gdi.memDC, oldPenB);

            // Present double buffer
            BitBlt(hdc, 0, 0, width, height, g_gdi.memDC, 0, 0, SRCCOPY);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY: {
            g_vm_thread_running = false;
            if (g_vm_thread.joinable()) {
                g_vm_thread.join();
            }
            clearGdiCache();
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    using namespace sandbox;
    using namespace sandbox::vm;

    LongTriple::initPowTable();

    // 1. Read files
    const std::string kernel = readTextFile("kernel.trit");
    const std::string trap = readTextFile("OS3/native_kernel_trap_stub.tasm");
    if (kernel.empty() || trap.empty()) {
        MessageBoxA(NULL, "Failed to read kernel.trit or trap stub.", "Error", MB_ICONERROR);
        return 1;
    }

    // 2. Compile kernel.trit
    using namespace sandbox::compiler;
    CompileResult compiled_kernel = compileSource("kernel.trit", kernel);
    if (!compiled_kernel.success) {
        std::string err = "Kernel compilation failed:\n";
        for (const auto& d : compiled_kernel.diagnostics) {
            err += d.format() + "\n";
        }
        MessageBoxA(NULL, err.c_str(), "Kernel Compile Error", MB_ICONERROR);
        return 1;
    }

    // 3. Compile apps
    LinkResult desktop = compileApp("desktop", 512);
    LinkResult calc = compileApp("calculator", 256);
    LinkResult task = compileApp("task_manager", 256);
    LinkResult paint = compileApp("paint", 256);

    if (!desktop.success || !calc.success || !task.success || !paint.success) {
        MessageBoxA(NULL, "Failed to compile/link one of the apps (desktop/calculator/task_manager/paint).", "Compile Error", MB_ICONERROR);
        return 1;
    }

    // 4. Build boot assembly
    constexpr int kDesktopPhys = 140000;
    constexpr int kCalcTextPpn = 8200;
    constexpr int kTaskTextPpn = 8300;
    constexpr int kPaintTextPpn = 8400;

    std::string boot_asm = buildBootAssembly(desktop, calc, task, paint, kDesktopPhys, kCalcTextPpn, kTaskTextPpn, kPaintTextPpn);

    // 5. Assemble boot + trap + kernel + desktop assembly
    std::string full_src = boot_asm + "\n" + trap + "\n" + compiled_kernel.assembly + "\n" +
                           ".text\n.org " + std::to_string(kDesktopPhys) + "\n" +
                           desktop.assembly + "\n";

    auto assembled = assembler::assemble(full_src);
    if (!assembled.success) {
        std::string err = "Boot image assembly failed:\n";
        for (const auto& e : assembled.errors) {
            err += "line " + std::to_string(e.line) + ": " + e.message + "\n";
        }
        MessageBoxA(NULL, err.c_str(), "Assembly Error", MB_ICONERROR);
        return 1;
    }

    // 6. Allocate VMState (262K IMEM, 1M DMEM)
    static VMState vm(262144, 1000000);
    if (!assembler::loadAndReset(vm, assembled)) {
        MessageBoxA(NULL, "Failed to load boot image into VM.", "Load Error", MB_ICONERROR);
        return 1;
    }

    // 7. Load individual app programs into their physical locations
    constexpr int kCalcTextPhys = kCalcTextPpn * sandbox::vm::MMU_PAGE_WORDS;
    constexpr int kTaskTextPhys = kTaskTextPpn * sandbox::vm::MMU_PAGE_WORDS;
    constexpr int kPaintTextPhys = kPaintTextPpn * sandbox::vm::MMU_PAGE_WORDS;

    if (!vm.imem.loadProgram(calc.assembled.program, kCalcTextPhys) ||
        !vm.imem.loadProgram(task.assembled.program, kTaskTextPhys) ||
        !vm.imem.loadProgram(paint.assembled.program, kPaintTextPhys)) {
        MessageBoxA(NULL, "Failed to load app programs into physical memory.", "Load Error", MB_ICONERROR);
        return 1;
    }

    g_vm = &vm;
    g_vm_thread_running = true;
    g_vm_thread = std::thread([]() {
        using namespace std::chrono;
        auto last_time = steady_clock::now();
        while (g_vm_thread_running) {
            if (g_vm && g_vm->status != sandbox::vm::VMStatus::HALTED) {
                auto now = steady_clock::now();
                auto elapsed = duration_cast<microseconds>(now - last_time).count();
                if (elapsed <= 0) {
                    std::this_thread::yield();
                    continue;
                }
                last_time = now;

                // 1 GHz = 1000 instructions per microsecond
                long long target_steps = elapsed * 1000;
                if (target_steps > 50000000) target_steps = 50000000; // Cap to prevent runaway issues

                long long run_steps = 0;
                {
                    std::lock_guard<std::mutex> lock(g_vm_mutex);
                    for (long long i = 0; i < target_steps; ++i) {
                        sandbox::vm::VMStatus stat = sandbox::vm::step(*g_vm);
                        run_steps++;
                        if (stat == sandbox::vm::VMStatus::HALTED || stat == sandbox::vm::VMStatus::TRAPPED) {
                            break;
                        }
                    }
                }
                // If we completed very quickly, yield slightly
                if (run_steps < target_steps) {
                    std::this_thread::sleep_for(milliseconds(1));
                } else {
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::sleep_for(milliseconds(5));
            }
        }
    });

    const wchar_t CLASS_NAME[] = L"TernaryVMConsoleWindowClass";
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = CLASS_NAME;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wcex)) {
        MessageBoxA(NULL, "Failed to register custom window class!", "Win32 Error", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Ternary VM Substrate Advanced Graphics Engine",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Failed to instantiate Win32 window!", "Win32 Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    LPSTR lpCmdLine = GetCommandLineA();
    HINSTANCE hInstance = GetModuleHandle(NULL);
    return WinMain(hInstance, NULL, lpCmdLine, SW_SHOWDEFAULT);
}
