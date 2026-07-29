#include "ternary_consumer_shell.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++g_failures;
    std::cout << "FAIL: " << message << "\n";
}

bool directoryContains(
    const std::vector<sandbox::os::DirectoryEntry>& entries,
    const std::string& name) {

    for (const auto& entry : entries) {
        if (entry.name == name) return true;
    }
    return false;
}

void installDummyApps(sandbox::os::OSKernel& kernel) {
    const std::vector<long long> image = {900, 901, 902, 903};
    const auto header = sandbox::vm::makeExecutableHeaderV2(
        4, static_cast<int>(image.size()), 1, 63);
    for (const sandbox::os::ConsumerAppEntry& app :
         sandbox::os::ConsumerShell::defaultApps()) {
        expect(kernel.installExecutable(app.path, image, header).ok(),
               "consumer app executable installs: " + app.path);
    }
}

void testBootLoginDesktopAndShutdown() {
    std::cout << "[1] Consumer shell boot, login, apps, files, prefs, and shutdown\n";
    using namespace sandbox::os;

    OSKernel kernel(256);
    ConsumerShell shell(kernel);

    expect(shell.boot().ok(), "kernel boots into consumer shell");
    expect(shell.snapshot().boot_splash_seen, "boot splash is shown");
    expect(shell.snapshot().first_run_required, "fresh image requires first-run setup");
    expect(shell.installBaseExperience().ok(), "consumer base layout installs");
    installDummyApps(kernel);

    std::vector<DirectoryEntry> rootDir;
    expect(shell.listDirectory("/", rootDir).ok(), "root directory lists");
    expect(directoryContains(rootDir, "dev"), "base layout includes /dev");
    expect(directoryContains(rootDir, "system"), "base layout includes /system");
    expect(directoryContains(rootDir, "lib"), "base layout includes /lib");

    std::vector<DirectoryEntry> appsDir;
    expect(shell.listDirectory("/apps", appsDir).ok(), "app registry directory lists");
    expect(directoryContains(appsDir, "registry"), "app registry lives under /apps");

    expect(shell.completeFirstRun("ada", 777).ok(), "first-run creates user account");
    expect(!shell.firstRunRequired(), "first-run marker is cleared");
    expect(shell.login("ada", 777).ok(), "user logs in");

    ConsumerDesktopSnapshot desktop = shell.snapshot();
    expect(desktop.phase == ConsumerShellPhase::Desktop, "shell reaches desktop phase");
    expect(desktop.username == "ada" && desktop.home == "/home/ada",
           "desktop carries active user and home directory");
    expect(desktop.clock == "12:00", "desktop exposes taskbar clock");
    expect(desktop.launcher.size() >= 9, "launcher exposes essential consumer apps");
    expect(shell.launchApp("text_editor").ok(), "launcher starts text editor");
    expect(shell.closeWindow(shell.snapshot().active_window_id).ok(),
           "text editor window closes");

    StatusResult calcWindow = shell.launchApp("calculator");
    expect(calcWindow.ok(), "launcher starts calculator");
    StatusResult filesWindow = shell.launchApp("files");
    expect(filesWindow.ok(), "launcher starts file manager");
    desktop = shell.snapshot();
    expect(desktop.taskbar_titles.size() == 2, "taskbar shows open app windows");
    expect(shell.switchWindow(calcWindow.payload).ok(), "window switching focuses calculator");
    expect(shell.snapshot().active_window_id == calcWindow.payload,
           "active window id follows focus");

    expect(shell.createFolder("/home/ada/docs").ok(), "file manager creates a folder");
    expect(shell.writeUserFile("/home/ada/docs/readme", {84, 82, 73, 84}).ok(),
           "file manager writes a file through VFS");
    std::vector<DirectoryEntry> docs;
    expect(shell.listDirectory("/home/ada/docs", docs).ok(), "file manager lists home folder");
    expect(directoryContains(docs, "readme"), "new file appears in VFS listing");

    ConsumerSettingsInfo info = shell.settingsInfo();
    expect(info.display_width == 80 && info.display_height == 60,
           "settings reports display geometry");
    expect(info.disk_blocks == 256, "settings reports storage geometry");
    expect(info.user_count == 1 && info.active_user == "ada",
           "settings reports users and active account");

    expect(shell.setPreference("accent", "cyan").ok(), "preferences update");
    expect(shell.preference("accent") == "cyan", "preferences read back");

    expect(shell.closeWindow(filesWindow.payload).ok(), "user closes file manager");
    expect(shell.snapshot().taskbar_titles.size() == 1, "taskbar removes closed app");

    StatusResult settingsWindow = shell.launchApp("settings");
    expect(settingsWindow.ok(), "launcher starts settings");
    expect(shell.markNotResponding(settingsWindow.payload).ok(),
           "not-responding dialog is raised");
    expect(shell.lastDialog().kind == ConsumerDialogKind::NotResponding,
           "dialog records stopped app");
    expect(shell.forceCloseDialogApp().ok(), "user can force-close unresponsive app");

    StatusResult terminalWindow = shell.launchApp("terminal");
    expect(terminalWindow.ok(), "terminal launches as an app window");
    expect(shell.reportCrash(terminalWindow.payload, -9).ok(),
           "crash dialog is raised and app is closed");
    expect(shell.lastDialog().kind == ConsumerDialogKind::Crash,
           "dialog records crashed app");
    std::vector<long long> crashLog;
    expect(kernel.fs().readFile("/var/crash/latest", crashLog).ok(),
           "crash log is persisted");

    expect(shell.closeWindow(calcWindow.payload).ok(), "last app can close");
    expect(shell.snapshot().taskbar_titles.empty(), "taskbar is empty after closing apps");

    expect(shell.shutdown().ok(), "consumer shell shuts down cleanly");
    std::vector<long long> image = kernel.diskImage();
    expect(!image.empty(), "shutdown produces a persistent disk image");

    OSKernel rebooted(image);
    ConsumerShell resumed(rebooted);
    expect(resumed.boot().ok(), "rebooted shell mounts persisted disk");
    expect(resumed.login("ada", 777).ok(), "user logs in after reboot");
    expect(resumed.preference("accent") == "cyan",
           "user preferences survive reboot");
    std::vector<DirectoryEntry> rebootedDocs;
    expect(resumed.listDirectory("/home/ada/docs", rebootedDocs).ok(),
           "home directory survives reboot");
    expect(directoryContains(rebootedDocs, "readme"), "managed file survives reboot");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    testBootLoginDesktopAndShutdown();
    if (g_failures != 0) {
        std::cout << g_failures << " consumer shell productization failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Consumer shell productization tests passed\n";
    return EXIT_SUCCESS;
}
