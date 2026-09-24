#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc > 1 ? argv[1] : L"sleep";
    if (mode == L"sleep") { Sleep(60000); return 0; }
    if (mode == L"signal") { std::ofstream("started.pid") << GetCurrentProcessId(); Sleep(60000); return 0; }
    if (mode == L"flood") { while (true) { std::cout << std::string(4096, 'x') << std::flush; } }
    if (mode == L"memory") {
        std::vector<void*> blocks;
        for (int i = 0; i < 512; ++i) {
            void* block = VirtualAlloc(nullptr, 1024*1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!block) { std::cout << "allocation-denied:" << blocks.size(); return 0; }
            memset(block, 1, 1024*1024); blocks.push_back(block);
        }
        std::cout << "unbounded-allocation"; return 1;
    }
    if (mode == L"child") {
        std::wstring command = L"\"" + std::wstring(argv[0]) + L"\" sleep";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        if (!CreateProcessW(argv[0], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) { std::cout << "child-denied"; return 0; }
        std::cout << "child-pid:" << child.dwProcessId << std::flush;
        CloseHandle(child.hThread); CloseHandle(child.hProcess); return 0;
    }
    return 2;
}
