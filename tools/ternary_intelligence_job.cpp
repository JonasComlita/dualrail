// Windows-only bounded process supervisor. The server supplies the fixed compiler
// and arguments; neither task packages nor model tools can select an executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

static std::string json(const std::string& input) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += c;
    }
    return out + '"';
}
static std::wstring quote(const std::wstring& text) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L'"';
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
static int fail(const char* reason) {
    std::cout << "{\"infrastructureError\":" << json(reason) << ",\"win32Error\":" << GetLastError() << "}\n";
    return 2;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--probe") {
        std::cout << "{\"protocol\":\"ternary.windows-job.v1\",\"jobObjects\":true}\n"; return 0;
    }
    if (argc < 8) return fail("invalid supervisor arguments");
    unsigned long timeout = wcstoul(argv[1], nullptr, 10), memoryMiB = wcstoul(argv[2], nullptr, 10), processes = wcstoul(argv[3], nullptr, 10), outputLimit = wcstoul(argv[4], nullptr, 10);
    if (timeout < 1 || timeout > 120000 || memoryMiB < 8 || memoryMiB > 512 || processes < 1 || processes > 8 || outputLimit < 1 || outputLimit > 1048576) return fail("invalid supervisor limits");
    Handle job; job.value = CreateJobObjectW(nullptr, nullptr);
    if (!job.value) return fail("CreateJobObject");
    Handle parent;
    const wchar_t* parentId = _wgetenv(L"TI_SUPERVISOR_PARENT_PID");
    if (parentId) {
        parent.value = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(parentId, nullptr, 10));
        if (!parent.value) return fail("OpenProcess parent");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = processes;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memoryMiB) * 1024 * 1024;
    limits.JobMemoryLimit = limits.ProcessMemoryLimit;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return fail("SetInformationJobObject");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle outRead, outWrite, errRead, errWrite, input;
    if (!CreatePipe(&outRead.value, &outWrite.value, &sa, 0) || !CreatePipe(&errRead.value, &errWrite.value, &sa, 0)) return fail("CreatePipe");
    if (!SetHandleInformation(outRead.value, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(errRead.value, HANDLE_FLAG_INHERIT, 0)) return fail("SetHandleInformation");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) return fail("open NUL");
    std::wstring command;
    for (int i = 6; i < argc; ++i) { if (!command.empty()) command += L' '; command += quote(argv[i]); }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value; startup.hStdOutput = outWrite.value; startup.hStdError = errWrite.value;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(argv[6], command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, argv[5], &startup, &pi)) return fail("CreateProcess");
    Handle process, thread; process.value = pi.hProcess; thread.value = pi.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) { TerminateProcess(process.value, 3); return fail("AssignProcessToJobObject"); }
    CloseHandle(outWrite.value); outWrite.value = nullptr; CloseHandle(errWrite.value); errWrite.value = nullptr;
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) { TerminateJobObject(job.value, 3); return fail("ResumeThread"); }
    const ULONGLONG start = GetTickCount64();
    std::string stdoutText, stderrText, reason;
    auto drain = [&](HANDLE pipe, std::string& target) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buf[4096]; DWORD read = 0;
            if (!ReadFile(pipe, buf, std::min<DWORD>(sizeof(buf), available), &read, nullptr) || !read) break;
            size_t used = stdoutText.size() + stderrText.size();
            size_t allowed = used < outputLimit ? outputLimit - used : 0;
            target.append(buf, std::min<size_t>(read, allowed));
            if (read > allowed) { reason = "output_limit"; TerminateJobObject(job.value, 4); break; }
        }
    };
    while (true) {
        if (parent.value && WaitForSingleObject(parent.value, 0) == WAIT_OBJECT_0) { reason = "parent_exit"; TerminateJobObject(job.value, 7); WaitForSingleObject(process.value, 5000); break; }
        drain(outRead.value, stdoutText); drain(errRead.value, stderrText);
        if (WaitForSingleObject(process.value, 5) == WAIT_OBJECT_0) break;
        if (GetTickCount64() - start >= timeout) { reason = "timeout"; TerminateJobObject(job.value, 5); WaitForSingleObject(process.value, 5000); break; }
        if (!reason.empty()) { WaitForSingleObject(process.value, 5000); break; }
    }
    // Always stop descendants, including descendants left behind by a successful root.
    TerminateJobObject(job.value, 6);
    drain(outRead.value, stdoutText); drain(errRead.value, stderrText);
    DWORD exitCode = 0; GetExitCodeProcess(process.value, &exitCode);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
    QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation, &measured, sizeof(measured), nullptr);
    std::cout << "{\"exitCode\":" << exitCode << ",\"pid\":" << pi.dwProcessId
              << ",\"durationMs\":" << GetTickCount64() - start << ",\"peakMemoryBytes\":" << measured.PeakJobMemoryUsed
              << ",\"reason\":" << (reason.empty() ? "null" : json(reason))
              << ",\"stdout\":" << json(stdoutText) << ",\"stderr\":" << json(stderrText) << "}\n";
    return 0;
}
