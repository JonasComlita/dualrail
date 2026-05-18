#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ternary_asm.h"
#include "ternary_vm.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

// Global VM state accessible to the window paint loop
static sandbox::vm::VMState* g_vm = nullptr;

static const std::string g_animation_asm = R"(
    .text
    start:
        ; 1. Enter character mode
        mov r4, 2
        csrw console_ctrl, r4
        
        ; 2. Pre-render BIOS screen to Text VRAM at 60000!
        mov r1, 60000
        
        ; Print border '=' chars
        mov r3, 0 ; Start index
        mov r4, 80 ; End index
    draw_border_loop:
        mov r5, 61 ; '=' character
        mov r6, 1280 ; color 5 (Cyan)
        add r7, r5, r6
        
        add r8, r1, r3
        store r7, r8   ; store to Text VRAM Page
        
        mov r8, 1
        add r3, r3, r8
        tcmp r9, r3, r4
        brn r9, draw_border_loop
        
        ; Print "TERNARY OS v1.2"
        mov r5, 768 ; color 3 (Amber)
        
        mov r4, 84  ; T
        add r4, r4, r5
        mov r3, 85
        add r7, r1, r3
        store r4, r7
        
        mov r4, 69  ; E
        add r4, r4, r5
        mov r3, 86
        add r7, r1, r3
        store r4, r7
        
        mov r4, 82  ; R
        add r4, r4, r5
        mov r3, 87
        add r7, r1, r3
        store r4, r7
        
        mov r4, 78  ; N
        add r4, r4, r5
        mov r3, 88
        add r7, r1, r3
        store r4, r7
        
        mov r4, 65  ; A
        add r4, r4, r5
        mov r3, 89
        add r7, r1, r3
        store r4, r7
        
        mov r4, 82  ; R
        add r4, r4, r5
        mov r3, 90
        add r7, r1, r3
        store r4, r7
        
        mov r4, 89  ; Y
        add r4, r4, r5
        mov r3, 91
        add r7, r1, r3
        store r4, r7
        
        mov r4, 79  ; O
        add r4, r4, r5
        mov r3, 93
        add r7, r1, r3
        store r4, r7
        
        mov r4, 83  ; S
        add r4, r4, r5
        mov r3, 94
        add r7, r1, r3
        store r4, r7
        
        ; Print "CORE UP"
        mov r5, 256 ; color 1 (Green)
        mov r4, 67  ; C
        add r4, r4, r5
        mov r3, 165
        add r7, r1, r3
        store r4, r7
        
        mov r4, 79  ; O
        add r4, r4, r5
        mov r3, 166
        add r7, r1, r3
        store r4, r7
        
        mov r4, 82  ; R
        add r4, r4, r5
        mov r3, 167
        add r7, r1, r3
        store r4, r7
        
        mov r4, 69  ; E
        add r4, r4, r5
        mov r3, 168
        add r7, r1, r3
        store r4, r7
        
        mov r4, 85  ; U
        add r4, r4, r5
        mov r3, 170
        add r7, r1, r3
        store r4, r7
        
        mov r4, 80  ; P
        add r4, r4, r5
        mov r3, 171
        add r7, r1, r3
        store r4, r7
        
        ; Print "PRESS T TO PAINT"
        mov r5, 1280 ; color 5 (Cyan)
        mov r4, 80  ; P
        add r4, r4, r5
        mov r3, 245
        add r7, r1, r3
        store r4, r7
        
        mov r4, 82  ; R
        add r4, r4, r5
        mov r3, 246
        add r7, r1, r3
        store r4, r7
        
        mov r4, 69  ; E
        add r4, r4, r5
        mov r3, 247
        add r7, r1, r3
        store r4, r7
        
        mov r4, 83  ; S
        add r4, r4, r5
        mov r3, 248
        add r7, r1, r3
        store r4, r7
        
        mov r4, 83  ; S
        add r4, r4, r5
        mov r3, 249
        add r7, r1, r3
        store r4, r7
        
        mov r4, 84  ; T
        add r4, r4, r5
        mov r3, 251
        add r7, r1, r3
        store r4, r7
        
        mov r4, 84  ; T
        add r4, r4, r5
        mov r3, 253
        add r7, r1, r3
        store r4, r7
        
        mov r4, 79  ; O
        add r4, r4, r5
        mov r3, 254
        add r7, r1, r3
        store r4, r7
        
        mov r4, 80  ; P
        add r4, r4, r5
        mov r3, 256
        add r7, r1, r3
        store r4, r7
        
        mov r4, 65  ; A
        add r4, r4, r5
        mov r3, 257
        add r7, r1, r3
        store r4, r7
        
        mov r4, 73  ; I
        add r4, r4, r5
        mov r3, 258
        add r7, r1, r3
        store r4, r7
        
        mov r4, 78  ; N
        add r4, r4, r5
        mov r3, 259
        add r7, r1, r3
        store r4, r7
        
        mov r4, 84  ; T
        add r4, r4, r5
        mov r3, 260
        add r7, r1, r3
        store r4, r7
        
        ; 3. Initialize VRAM mode to Graphics Mode (1)
        mov r4, 1
        csrw gpu_mode, r4

    loop_top:
        ; 4. Move hardware sprite to current mouse coordinates!
        csrr r10, mouse_x
        csrr r11, mouse_y
        csrr r12, mouse_btn
        
        csrw sprite_x, r10
        csrw sprite_y, r11
        
        ; Sprite Attribute: character code 43 ('+'), color code 5 (Neon Cyan)
        ; 43 + (5 * 256) = 43 + 1280 = 1323
        mov r13, 1323
        csrw sprite_attr, r13
        
        ; 5. Check keyboard input for Mode Toggle ('t' / 'T' / Spacebar)
        csrr r20, console_in_ctrl
        mov r21, 0
        tcmp r22, r20, r21
        brp r22, handle_keyboard
        jmp check_draw
        
    handle_keyboard:
        csrr r23, console_in   ; read key character
        
        ; Consume the character from the buffer!
        mov r24, 1
        csrw console_in_ctrl, r24
        
        mov r24, 32            ; spacebar ASCII
        tcmp r25, r23, r24
        brz r25, clear_canvas
        
        mov r24, 116           ; 't' ASCII
        tcmp r25, r23, r24
        brz r25, toggle_mode
        
        mov r24, 84            ; 'T' ASCII
        tcmp r25, r23, r24
        brz r25, toggle_mode
        
        jmp check_draw
        
    toggle_mode:
        ; Toggle gpu_mode (1 - gpu_mode)
        csrr r2, gpu_mode
        mov r3, 1
        sub r4, r3, r2
        csrw gpu_mode, r4
        jmp check_draw
        
    clear_canvas:
        mov r15, 0
        csrw gpu_color, r15
        mov r16, 1
        csrw gpu_cmd, r16 ; Clear Page B
        
        csrr r2, gpu_page
        mov r3, 1
        sub r4, r3, r2
        csrw gpu_page, r4 ; Swap visible page
        
        mov r16, 1
        csrw gpu_cmd, r16 ; Clear Page A
        jmp check_draw
        
    check_draw:
        ; 6. Only paint in Graphics Mode (gpu_mode == 1)
        csrr r2, gpu_mode
        mov r3, 0
        tcmp r4, r2, r3
        brz r4, loop_top      ; If text mode, skip drawing trail
        
        ; If left click is active (mouse_btn == 1), paint!
        mov r13, 1
        tcmp r14, r12, r13
        brz r14, draw_thick_brush
        jmp loop_top
        
    draw_thick_brush:
        mov r15, 1
        sub r21, r10, r15
        add r22, r10, r15
        sub r23, r11, r15
        add r24, r11, r15
        
        csrw gpu_x1, r21
        csrw gpu_x2, r22
        csrw gpu_y1, r23
        csrw gpu_y2, r24
        
        mov r16, 6
        csrw gpu_color, r16
        
        mov r17, 2
        csrw gpu_cmd, r17
        
        ; Page flip to show drawing!
        csrr r2, gpu_page
        mov r3, 1
        sub r4, r3, r2
        csrw gpu_page, r4
        jmp loop_top
)";

// Helper Cyberpunk color palette builder
static COLORREF getCyberColor(int idx) {
    switch (idx) {
        case 0:  return RGB(8, 16, 10);     // Deep Phosphor Black
        case 1:  return RGB(0, 255, 70);    // CRT Phosphor Green
        case 2:  return RGB(0, 100, 30);    // CRT Shadow Green
        case 3:  return RGB(255, 120, 0);   // Neon Amber
        case 4:  return RGB(100, 50, 0);    // CRT Shadow Amber
        case 5:  return RGB(0, 240, 255);   // Neon Cyan
        case 6:  return RGB(255, 0, 128);   // Cyber Hot Pink
        case 7:  return RGB(255, 255, 255); // Pure White
        default: return RGB(8, 16, 10);
    }
}

// Window procedure to handle paint and ticks
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            SetTimer(hwnd, 1, 16, NULL); // ~60 FPS Timer
            return 0;
        }
        case WM_TIMER: {
            if (g_vm && g_vm->status != sandbox::vm::VMStatus::HALTED) {
                // Execute VM instructions per tick to support real-time inputs
                for (int i = 0; i < 4000; ++i) {
                    sandbox::vm::VMStatus stat = sandbox::vm::step(*g_vm);
                    if (stat == sandbox::vm::VMStatus::HALTED || stat == sandbox::vm::VMStatus::TRAPPED) {
                        break;
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
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
                    g_vm->mouse_x = (x * 80) / width;
                    g_vm->mouse_y = (g_vm->gpu_mode == 0) ? ((y * 25) / height) : ((y * 60) / height);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (g_vm) {
                g_vm->mouse_btn = 1;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_vm) {
                g_vm->mouse_btn = 0;
            }
            return 0;
        }
        case WM_CHAR: {
            if (g_vm) {
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

            // Double Buffering
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            // Fill base screen
            HBRUSH bgBrush = CreateSolidBrush(RGB(8, 16, 10));
            FillRect(memDC, &rect, bgBrush);
            DeleteObject(bgBrush);

            if (g_vm) {
                // Determine VRAM presenting base address based on display mode
                int visible_base = (g_vm->gpu_mode == 0) ? 60000 : ((g_vm->gpu_page == 0) ? 50000 : 55000);

                if (g_vm->gpu_mode == 0) {
                    // 1. Text-Mode Tile Grid presentation (80 x 25 characters)
                    double scaleTextX = (double)width / 80.0;
                    double scaleTextY = (double)height / 25.0;

                    // Choose standard monospaced Consolas font mapped to scale
                    HFONT hFont = CreateFontA(
                        static_cast<int>(scaleTextY + 2), static_cast<int>(scaleTextX + 1), 
                        0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                        DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Consolas"
                    );
                    HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
                    SetBkMode(memDC, TRANSPARENT);

                    for (int y = 0; y < 25; ++y) {
                        for (int x = 0; x < 80; ++x) {
                            int vram_addr = visible_base + y * 80 + x;
                            long long val = 0;
                            if (vram_addr >= 0 && vram_addr < g_vm->dmem.size()) {
                                val = sandbox::vm::ops::toLong(g_vm->dmem.words[vram_addr]);
                            }

                            char ch = static_cast<char>(val & 0xFF);
                            if (ch >= 32 && ch <= 126) {
                                int color_idx = static_cast<int>((val >> 8) & 0x0F);
                                COLORREF charColor = getCyberColor(color_idx);
                                SetTextColor(memDC, charColor);

                                TextOutA(memDC, static_cast<int>(x * scaleTextX), static_cast<int>(y * scaleTextY), &ch, 1);
                            }
                        }
                    }

                    SelectObject(memDC, oldFont);
                    DeleteObject(hFont);

                } else {
                    // 2. Pixel Graphics mode (80 x 60 pixel grid)
                    double scaleX = (double)width / 80.0;
                    double scaleY = (double)height / 60.0;

                    for (int y = 0; y < 60; ++y) {
                        for (int x = 0; x < 80; ++x) {
                            int vram_addr = visible_base + y * 80 + x;
                            long long val = 0;
                            if (vram_addr >= 0 && vram_addr < g_vm->dmem.size()) {
                                val = sandbox::vm::ops::toLong(g_vm->dmem.words[vram_addr]);
                            }

                            if (val != 0) {
                                COLORREF color = getCyberColor(static_cast<int>(val & 0x0F));
                                HBRUSH cellBrush = CreateSolidBrush(color);
                                RECT cellRect = { 
                                    (int)(x * scaleX), (int)(y * scaleY), 
                                    (int)((x + 1) * scaleX) + 1, (int)((y + 1) * scaleY) + 1 
                                };
                                FillRect(memDC, &cellRect, cellBrush);
                                DeleteObject(cellBrush);
                            }
                        }
                    }
                }

                // 3. Hardware Sprite 0 Compositing
                if (g_vm->sprite_attr != 0) {
                    char glyph = static_cast<char>(g_vm->sprite_attr & 0xFF);
                    int color_idx = static_cast<int>((g_vm->sprite_attr >> 8) & 0x0F);
                    COLORREF spriteColor = getCyberColor(color_idx);

                    double sx = (g_vm->gpu_mode == 0) ? ((double)width / 80.0) : ((double)width / 80.0);
                    double sy = (g_vm->gpu_mode == 0) ? ((double)height / 25.0) : ((double)height / 60.0);

                    HFONT spriteFont = CreateFontA(
                        static_cast<int>(sy * 1.5), static_cast<int>(sx * 1.2), 
                        0, 0, FW_EXTRABOLD, FALSE, FALSE, FALSE, 
                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                        DEFAULT_QUALITY, MONO_FONT | FF_DONTCARE, "Consolas"
                    );
                    HFONT oldFontS = (HFONT)SelectObject(memDC, spriteFont);
                    SetBkMode(memDC, TRANSPARENT);
                    SetTextColor(memDC, spriteColor);

                    int px = static_cast<int>(g_vm->sprite_x * sx);
                    int py = static_cast<int>(g_vm->sprite_y * sy);

                    TextOutA(memDC, px, py, &glyph, 1);

                    SelectObject(memDC, oldFontS);
                    DeleteObject(spriteFont);
                }
            }

            // Apply CRT phosphor dynamic scanlines
            for (int y = 0; y < height; y += 3) {
                HPEN scanPen = CreatePen(PS_SOLID, 1, RGB(4, 10, 5));
                HPEN oldPen = (HPEN)SelectObject(memDC, scanPen);
                MoveToEx(memDC, 0, y, NULL);
                LineTo(memDC, width, y);
                SelectObject(memDC, oldPen);
                DeleteObject(scanPen);
            }

            // High-tech active layout border
            HPEN borderPen = CreatePen(PS_SOLID, 4, RGB(0, 255, 70));
            HPEN oldPenB = (HPEN)SelectObject(memDC, borderPen);
            HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, nullBrush);
            Rectangle(memDC, 4, 4, width - 4, height - 4);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPenB);
            DeleteObject(borderPen);

            // Present double buffer
            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY: {
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

    std::cout << "[Host] Compiling Ternary Assembly..." << std::endl;
    auto assembled = assembler::assemble(g_animation_asm);
    if (!assembled.success) {
        std::string err_msg = "Ternary assembly compilation failed:\n";
        for (const auto& err : assembled.errors) {
            err_msg += err.format() + "\n";
        }
        std::cerr << err_msg << std::endl;
        MessageBoxA(NULL, err_msg.c_str(), "Assembler Error", MB_ICONERROR);
        return 1;
    }

    // Allocate 64K DMEM buffer
    VMState vm(65536, 65536);
    if (!assembler::loadAndReset(vm, assembled)) {
        MessageBoxA(NULL, "Failed to load assembled image into Instruction Memory!", "VM Loader Error", MB_ICONERROR);
        return 1;
    }
    g_vm = &vm;

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
