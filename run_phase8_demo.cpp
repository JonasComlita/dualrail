#include "ternary_phase8.h"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

namespace {

void printHeader(const std::string& title) {
    std::cout << "\n\033[1;35m===============================================================\033[0m\n";
    std::cout << "\033[1;36m  " << title << "\033[0m\n";
    std::cout << "\033[1;35m===============================================================\033[0m\n";
}

void printStep(const std::string& msg) {
    std::cout << "\n\033[1;33m[*] " << msg << "\033[0m\n";
}

void printSuccess(const std::string& msg) {
    std::cout << "\033[1;32m[+] SUCCESS: " << msg << "\033[0m\n";
}

void printMetric(const std::string& label, const std::string& val) {
    std::cout << "    \033[1m" << std::left << std::setw(28) << label << ":\033[0m " << val << "\n";
}

void printStatusResult(const std::string& syscall_name, const sandbox::phase8::StatusResult& res) {
    std::string stat_str = (res.status == sandbox::phase8::T1_SUCCESS) ? "\033[1;32mSUCCESS (1)\033[0m" :
                           (res.status == sandbox::phase8::T1_PENDING) ? "\033[1;33mPENDING (0)\033[0m" :
                                                                         "\033[1;31mERROR (-1)\033[0m";
    std::cout << "    Syscall " << std::left << std::setw(10) << syscall_name 
              << " -> Status: " << std::setw(20) << stat_str 
              << " | Payload: " << std::setw(6) << res.payload 
              << " | Detail: " << res.detail << "\n";
}

} // namespace

int main() {
    using namespace sandbox::phase8;
    using namespace sandbox::compiler;

    sandbox::LongTriple::initPowTable();

    std::cout << "\033[2J\033[H"; // Clear screen and reset cursor
    std::cout << "\033[1;35m===============================================================\033[0m\n";
    std::cout << "\033[1;32m       Ternary OS3 Phase 8 Platform Substrate Demonstrator     \033[0m\n";
    std::cout << "\033[1;35m===============================================================\033[0m\n";
    std::cout << " This demonstrator validates the Phase 8 concrete C++ hardware\n";
    std::cout << " emulation substrate, memory allocations, process environments,\n";
    std::cout << " and filesystems before we route them into assembly kernel paths.\n";
    std::cout << "===============================================================\n";

    // -------------------------------------------------------------------------
    // 1. Device Tree & Block Device
    // -------------------------------------------------------------------------
    printHeader("1. Hardware Emulation & Device Tree Validation");

    printStep("Building default Device Tree (128-block geometry)...");
    DeviceTree dt = defaultDeviceTree(128);
    
    std::vector<std::string> dt_errors;
    StatusResult dt_valid = dt.validate(&dt_errors);
    if (dt_valid.ok()) {
        printSuccess("Device tree validation successfully completed.");
    } else {
        std::cerr << "[-] DT Validation Failed!\n";
        for (const auto& err : dt_errors) std::cerr << "    - " << err << "\n";
        return 1;
    }

    for (const auto& node : dt.nodes) {
        std::cout << "  - Node: \033[1m" << node.name << "\033[0m (" << node.compatible << ")\n";
        for (const auto& prop : node.properties) {
            std::cout << "      * Property: " << prop.first << " = " << prop.second << "\n";
        }
    }

    printStep("Initializing hardware block device (block0)...");
    BlockDevice block_dev(128);
    printMetric("Total Disk Capacity", std::to_string(block_dev.blockCount()) + " blocks");
    printMetric("Block Geometry Size", std::to_string(block_dev.blockWords()) + " words/block");

    // -------------------------------------------------------------------------
    // 2. Tiny File System Formatting and Inode Structures
    // -------------------------------------------------------------------------
    printHeader("2. Tiny Inode Filesystem Operations");

    printStep("Formatting disk image (block0)...");
    TinyFileSystem fs;
    StatusResult format_res = fs.format(block_dev);
    printStatusResult("format", format_res);

    printStep("Mounting block0 filesystem...");
    StatusResult mount_res = fs.mount(block_dev);
    printStatusResult("mount", mount_res);

    printStep("Creating system directories and files...");
    (void)fs.createFile("/bin", InodeKind::Directory);
    (void)fs.createFile("/etc", InodeKind::Directory);
    (void)fs.createFile("/etc/motd", InodeKind::File);
    (void)fs.createFile("/bin/init", InodeKind::Executable, true);

    std::vector<long long> motd_content = {72, 101, 108, 108, 111, 44, 32, 84, 101, 114, 110, 97, 114, 121, 32, 79, 83, 33}; // "Hello, Ternary OS!"
    (void)fs.writeFile("/etc/motd", motd_content);

    printStep("Attaching Executable Image Headers...");
    sandbox::vm::ExecutableImageHeader init_header;
    init_header.entry_virtual_pc = 127;
    init_header.text_pages = 2;
    init_header.data_pages = 1;
    init_header.stack_words = 64;
    (void)fs.writeFile("/bin/init", {90, 91, 92, 93}); // Init binary payload
    StatusResult exec_res = fs.markExecutable("/bin/init", init_header);
    printStatusResult("markExec", exec_res);

    printStep("Filesystem Layout Listing:");
    std::vector<std::string> paths_to_stat = {"/", "/bin", "/etc", "/etc/motd", "/bin/init"};
    for (const auto& path : paths_to_stat) {
        FileStat st;
        if (fs.stat(path, st).ok()) {
            std::string kind_str = (st.kind == InodeKind::Directory) ? "DIRECTORY" :
                                   (st.kind == InodeKind::Executable) ? "EXECUTABLE" : "FILE";
            std::cout << "  - Path: \033[1;36m" << std::left << std::setw(12) << path << "\033[0m"
                      << " | Inode: " << std::setw(2) << st.inode 
                      << " | Type: " << std::setw(11) << kind_str 
                      << " | Size: " << std::setw(3) << st.size_words << " words"
                      << " | Direct Blocks: " << st.direct_blocks << "\n";
        }
    }

    // -------------------------------------------------------------------------
    // 3. Syscalls, Dynamic Heaps, and Fork/Exec Lifecycle Facade
    // -------------------------------------------------------------------------
    printHeader("3. Process Lifecycles, Heaps, and Syscalls");

    printStep("Booting Phase 8 Kernel Facade...");
    Phase8Kernel kernel(128);
    StatusResult boot_res = kernel.boot();
    printStatusResult("boot", boot_res);

    constexpr int kMainPid = 1;
    printStep("Simulating open(), read(), and close() syscall sequence on Pid 1...");
    (void)kernel.fs().createFile("/etc", InodeKind::Directory);
    (void)kernel.fs().createFile("/etc/motd", InodeKind::File);
    (void)kernel.fs().writeFile("/etc/motd", motd_content);

    StatusResult open_res = kernel.sysOpen(kMainPid, "/etc/motd", false);
    printStatusResult("sysOpen", open_res);
    int fd = open_res.payload;

    std::vector<long long> read_buf;
    StatusResult read_res = kernel.sysRead(kMainPid, fd, 18, read_buf);
    printStatusResult("sysRead", read_res);
    
    std::cout << "    Read Buffer Decoded ASCII: \"\033[1;32m";
    for (long long word : read_buf) std::cout << static_cast<char>(word);
    std::cout << "\033[0m\"\n";

    StatusResult close_res = kernel.sysClose(kMainPid, fd);
    printStatusResult("sysClose", close_res);

    printStep("Demonstrating sbrk() dynamic heap allocations...");
    Process* proc = kernel.process(kMainPid);
    printMetric("Initial Heap Break Address", std::to_string(proc->heap_break));
    printMetric("Heap Bound Limit", std::to_string(proc->heap_limit));
    
    StatusResult sbrk_res = kernel.sysSbrk(kMainPid, 12);
    printStatusResult("sysSbrk", sbrk_res);
    printMetric("Grown Heap Break Address", std::to_string(proc->heap_break));

    printStep("Eager-copy fork() process creation...");
    proc->memory.resize(10, 0);
    proc->memory[5] = 999; // Seed memory value
    
    StatusResult fork_res = kernel.sysFork(kMainPid);
    printStatusResult("sysFork", fork_res);
    int child_pid = fork_res.payload;
    
    Process* child = kernel.process(child_pid);
    printMetric("Child PID", std::to_string(child->pid));
    printMetric("Child Parent PID", std::to_string(child->parent_pid));
    printMetric("Child Inherited Memory[5]", std::to_string(child->memory[5]));

    printStep("Disk-backed exec() program execution...");
    (void)kernel.fs().createFile("/bin", InodeKind::Directory);
    (void)kernel.fs().createFile("/bin/init", InodeKind::Executable, true);
    (void)kernel.fs().writeFile("/bin/init", {101, 102, 103});
    (void)kernel.fs().markExecutable("/bin/init", init_header);

    StatusResult exec_result = kernel.sysExec(child_pid, "/bin/init");
    printStatusResult("sysExec", exec_result);
    printMetric("Child Program Entry Point", std::to_string(child->exec_header.entry_virtual_pc));
    printMetric("Child Stack Words Hint", std::to_string(child->exec_header.stack_words));

    // -------------------------------------------------------------------------
    // 4. Memory Ordering Verification and Compiler Wrappers
    // -------------------------------------------------------------------------
    printHeader("4. Concurrency Models & Compiler Wrappers");

    printStep("Verifying SharedWord Acquire-Release fences...");
    SharedWord sh_word;
    sh_word.value = 40;
    printMetric("Initial Shared value", std::to_string(sh_word.tldr(SharedOrder::AcquireRelease)));
    
    StatusResult cas_fail = sh_word.tstr(50, 45, SharedOrder::AcquireRelease);
    printStatusResult("tstr (CAS Fail)", cas_fail);
    
    StatusResult cas_success = sh_word.tstr(50, 40, SharedOrder::AcquireRelease);
    printStatusResult("tstr (CAS Ok)", cas_success);
    printMetric("Updated Shared value", std::to_string(sh_word.tldr(SharedOrder::AcquireRelease)));

    printStep("Lowering compiler high-level wrappers...");
    const std::string trit_src = R"(
        fn main() -> t40 {
          let grown = sys_sbrk(9);
          let child = sys_fork();
          let status = sys_exec(0);
          return grown + child + status;
        }
    )";
    CompileResult compiled = compileSource("phase8_codegen.trit", trit_src);
    if (compiled.success) {
        printSuccess("High-level syscall wrappers successfully compiled into target instructions:");
        if (compiled.assembly.find("syscall 19") != std::string::npos) {
            std::cout << "    -> sys_sbrk wrapper successfully lowered to: \033[1;32msyscall 19\033[0m\n";
        }
        if (compiled.assembly.find("syscall 20") != std::string::npos) {
            std::cout << "    -> sys_fork wrapper successfully lowered to: \033[1;32msyscall 20\033[0m\n";
        }
        if (compiled.assembly.find("syscall 21") != std::string::npos) {
            std::cout << "    -> sys_exec wrapper successfully lowered to: \033[1;32msyscall 21\033[0m\n";
        }
    } else {
        std::cerr << "[-] Compiler codegen test failed!\n";
        for (const auto& diag : compiled.diagnostics) {
            std::cerr << "    Diagnostic: " << diag.format() << "\n";
        }
    }

    // -------------------------------------------------------------------------
    // 5. Boundary Statement
    // -------------------------------------------------------------------------
    std::cout << "\n\033[1;36m===============================================================\033[0m\n";
    std::cout << "\033[1;33m                   ARCHITECTURAL BOUNDARY REMINDER             \033[0m\n";
    std::cout << "\033[1;36m===============================================================\033[0m\n";
    std::cout << " Keep in mind: This is the concrete Phase 8 platform substrate/\n";
    std::cout << " facade and ABI reservation layer. The remaining work involves\n";
    std::cout << " routing these services into the hand-written assembly kernel\n";
    std::cout << " syscall path and replacing the host-side facade with VM-executed\n";
    std::cout << " kernel behavior. This demonstrator is a facade representation\n";
    std::cout << " and will be expanded as VM-executed kernel drivers evolve.\n";
    std::cout << "\033[1;36m===============================================================\033[0m\n\n";

    return 0;
}
