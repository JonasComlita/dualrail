// =============================================================================
// ternary_consumer_shell.h - Productized Trit OS user experience facade
// =============================================================================
//
// This layer ties the Phase 8 kernel substrate into a coherent consumer shell:
// boot/login/first-run state, an /apps install registry, per-user homes and
// preferences, VFS-backed file management, settings summaries, app windows,
// crash dialogs, not-responding handling, and shutdown persistence.

#pragma once
#ifndef TERNARY_CONSUMER_SHELL_H
#define TERNARY_CONSUMER_SHELL_H

#include "ternary_os.h"

#include <optional>

namespace sandbox {
namespace os {

static constexpr int CONSUMER_APP_CAP_LAUNCH = 1;
static constexpr int CONSUMER_APP_CAP_FILES = 2;
static constexpr int CONSUMER_APP_CAP_SETTINGS = 4;
static constexpr int CONSUMER_APP_CAP_TERMINAL = 8;
static constexpr int CONSUMER_APP_CAP_SYSTEM = 16;

enum class ConsumerShellPhase {
    Off,
    BootSplash,
    FirstRun,
    Login,
    Desktop,
    Shutdown,
};

enum class ConsumerDialogKind {
    None,
    Crash,
    NotResponding,
};

struct ConsumerAppEntry {
    std::string id;
    std::string title;
    std::string path;
    int capabilities = CONSUMER_APP_CAP_LAUNCH;
    bool windowed = true;
};

struct ConsumerUserRecord {
    std::string username;
    long long password_hash = 0;
    std::string home;
    std::string shell;
};

struct ConsumerWindow {
    int window_id = -1;
    int pid = -1;
    std::string app_id;
    std::string title;
    bool open = false;
    bool active = false;
    bool responding = true;
};

struct ConsumerDialog {
    ConsumerDialogKind kind = ConsumerDialogKind::None;
    int window_id = -1;
    int pid = -1;
    std::string title;
    std::string message;
};

struct ConsumerDesktopSnapshot {
    ConsumerShellPhase phase = ConsumerShellPhase::Off;
    bool boot_splash_seen = false;
    bool first_run_required = false;
    bool logged_in = false;
    std::string username;
    std::string home;
    std::string clock;
    std::vector<ConsumerAppEntry> launcher;
    std::vector<std::string> taskbar_titles;
    int active_window_id = -1;
};

struct ConsumerSettingsInfo {
    int display_width = 0;
    int display_height = 0;
    int disk_blocks = 0;
    int max_processes = 0;
    int max_windows = 0;
    int open_windows = 0;
    int user_count = 0;
    std::string active_user;
};

class ConsumerShell {
public:
    explicit ConsumerShell(OSKernel& kernel)
        : kernel_(kernel) {}

    [[nodiscard]] static std::vector<ConsumerAppEntry> defaultApps() {
        return {
            {"calculator", "Calculator", "/bin/calculator", CONSUMER_APP_CAP_LAUNCH, true},
            {"tasks", "Task Manager", "/bin/task_manager", CONSUMER_APP_CAP_SYSTEM, true},
            {"paint", "Paint", "/bin/paint", CONSUMER_APP_CAP_LAUNCH, true},
            {"files", "Files", "/bin/file_manager", CONSUMER_APP_CAP_FILES, true},
            {"settings", "Settings", "/bin/settings", CONSUMER_APP_CAP_SETTINGS, true},
            {"terminal", "Terminal", "/bin/terminal", CONSUMER_APP_CAP_TERMINAL, true},
        };
    }

    [[nodiscard]] StatusResult boot() {
        StatusResult mounted = kernel_.boot();
        if (!mounted.ok()) return mounted;
        phase_ = ConsumerShellPhase::BootSplash;
        boot_splash_seen_ = true;
        if (firstRunRequired()) {
            phase_ = ConsumerShellPhase::FirstRun;
        } else {
            phase_ = ConsumerShellPhase::Login;
        }
        return StatusResult::success();
    }

    [[nodiscard]] StatusResult installBaseExperience() {
        return installBaseExperience(defaultApps());
    }

    [[nodiscard]] StatusResult installBaseExperience(
        const std::vector<ConsumerAppEntry>& apps) {

        for (const std::string& dir : {"/bin", "/apps", "/etc", "/home", "/tmp",
                                       "/var", "/var/log", "/var/crash"}) {
            StatusResult made = ensureDirectory(dir);
            if (!made.ok()) return made;
        }
        StatusResult registry = writeRegistry(apps);
        if (!registry.ok()) return registry;

        if (!fileExists("/etc/first_run")) {
            StatusResult firstRun = writeFileCreating("/etc/first_run", {1});
            if (!firstRun.ok()) return firstRun;
        }
        if (!fileExists("/etc/shell_prefs")) {
            StatusResult prefs = writeFileCreating(
                "/etc/shell_prefs",
                preferenceWords({{"accent", "green"}, {"scale", "1"}}));
            if (!prefs.ok()) return prefs;
        }
        return kernel_.shutdownSync();
    }

    [[nodiscard]] StatusResult completeFirstRun(
        const std::string& username,
        long long password_hash,
        const std::string& shell_path = "/bin/terminal") {

        if (username.empty()) return StatusResult::error(ERR_INVALID);
        StatusResult layout = installBaseExperience(registry());
        if (!layout.ok()) return layout;

        const std::string home = "/home/" + username;
        StatusResult homeDir = ensureDirectory(home);
        if (!homeDir.ok()) return homeDir;

        std::vector<ConsumerUserRecord> users = readUsers();
        auto existing = std::find_if(users.begin(), users.end(),
                                     [&](const ConsumerUserRecord& user) {
                                         return user.username == username;
                                     });
        if (existing == users.end()) {
            users.push_back(ConsumerUserRecord{username, password_hash, home, shell_path});
        } else {
            existing->password_hash = password_hash;
            existing->home = home;
            existing->shell = shell_path;
        }
        StatusResult wroteUsers = writeFileCreating("/etc/users", userWords(users));
        if (!wroteUsers.ok()) return wroteUsers;
        StatusResult prefs = writeUserPreferences(username, {{"accent", "green"},
                                                            {"density", "comfortable"}});
        if (!prefs.ok()) return prefs;
        StatusResult marker = writeFileCreating("/etc/first_run", {0});
        if (!marker.ok()) return marker;
        phase_ = ConsumerShellPhase::Login;
        return kernel_.shutdownSync();
    }

    [[nodiscard]] bool firstRunRequired() const {
        std::vector<long long> marker;
        if (!readFile("/etc/first_run", marker) || marker.empty()) return true;
        return marker[0] != 0;
    }

    [[nodiscard]] StatusResult login(const std::string& username, long long password_hash) {
        for (const ConsumerUserRecord& user : readUsers()) {
            if (user.username == username && user.password_hash == password_hash) {
                logged_in_ = true;
                active_user_ = user;
                phase_ = ConsumerShellPhase::Desktop;
                (void)ensureDirectory(user.home);
                return StatusResult::success();
            }
        }
        return StatusResult::error(ERR_INVALID);
    }

    [[nodiscard]] StatusResult launchApp(const std::string& app_id) {
        if (!logged_in_) return StatusResult::error(ERR_INVALID);
        std::optional<ConsumerAppEntry> app = findApp(app_id);
        if (!app) return StatusResult::error(ERR_NOT_FOUND);

        StatusResult child = kernel_.sysFork(session_pid_);
        if (!child.ok()) return child;
        const int pid = child.payload;
        StatusResult exec = kernel_.sysExec(pid, app->path);
        if (!exec.ok()) {
            (void)kernel_.sysKill(pid, vm::SIGNAL_KILL);
            (void)kernel_.sysWaitPid(session_pid_, pid);
            return exec;
        }
        StatusResult window = kernel_.createWindow(pid, 80, 25);
        if (!window.ok()) {
            (void)kernel_.sysKill(pid, vm::SIGNAL_KILL);
            (void)kernel_.sysWaitPid(session_pid_, pid);
            return window;
        }

        for (ConsumerWindow& openWindow : windows_) openWindow.active = false;
        windows_.push_back(ConsumerWindow{window.payload, pid, app->id, app->title,
                                          true, true, true});
        active_window_id_ = window.payload;
        return StatusResult::success(window.payload);
    }

    [[nodiscard]] StatusResult switchWindow(int window_id) {
        ConsumerWindow* target = findWindow(window_id);
        if (!target || !target->open) return StatusResult::error(ERR_NOT_FOUND);
        for (ConsumerWindow& window : windows_) window.active = false;
        target->active = true;
        active_window_id_ = target->window_id;
        return StatusResult::success(window_id);
    }

    [[nodiscard]] StatusResult closeWindow(int window_id) {
        ConsumerWindow* target = findWindow(window_id);
        if (!target || !target->open) return StatusResult::error(ERR_NOT_FOUND);
        (void)kernel_.sysKill(target->pid, vm::SIGNAL_KILL);
        (void)kernel_.sysWaitPid(session_pid_, target->pid);
        target->open = false;
        target->active = false;
        if (active_window_id_ == window_id) active_window_id_ = firstOpenWindowId();
        return StatusResult::success(window_id);
    }

    [[nodiscard]] StatusResult reportCrash(int window_id, int exit_code) {
        ConsumerWindow* target = findWindow(window_id);
        if (!target || !target->open) return StatusResult::error(ERR_NOT_FOUND);
        last_dialog_ = ConsumerDialog{
            ConsumerDialogKind::Crash,
            target->window_id,
            target->pid,
            target->title + " crashed",
            "Exit code " + std::to_string(exit_code),
        };
        StatusResult log = writeFileCreating(
            "/var/crash/latest",
            asciiWords(target->title + " crashed with " + std::to_string(exit_code)));
        if (!log.ok()) return log;
        return closeWindow(window_id);
    }

    [[nodiscard]] StatusResult markNotResponding(int window_id) {
        ConsumerWindow* target = findWindow(window_id);
        if (!target || !target->open) return StatusResult::error(ERR_NOT_FOUND);
        target->responding = false;
        (void)kernel_.sysSuspend(target->pid);
        last_dialog_ = ConsumerDialog{
            ConsumerDialogKind::NotResponding,
            target->window_id,
            target->pid,
            target->title + " stopped responding",
            "The app is not processing window events.",
        };
        return StatusResult::success(window_id);
    }

    [[nodiscard]] StatusResult forceCloseDialogApp() {
        if (last_dialog_.kind == ConsumerDialogKind::None) {
            return StatusResult::error(ERR_NOT_FOUND);
        }
        return closeWindow(last_dialog_.window_id);
    }

    [[nodiscard]] StatusResult listDirectory(
        const std::string& path,
        std::vector<DirectoryEntry>& out) const {

        return kernel_.sysReadDir(path, out);
    }

    [[nodiscard]] StatusResult createFolder(const std::string& path) {
        return ensureDirectory(path);
    }

    [[nodiscard]] StatusResult writeUserFile(
        const std::string& path,
        const std::vector<long long>& words) {

        return writeFileCreating(path, words);
    }

    [[nodiscard]] StatusResult setPreference(
        const std::string& key,
        const std::string& value) {

        if (!logged_in_ || key.empty()) return StatusResult::error(ERR_INVALID);
        std::map<std::string, std::string> prefs = readUserPreferences(active_user_.username);
        prefs[key] = value;
        return writeUserPreferences(active_user_.username, prefs);
    }

    [[nodiscard]] std::string preference(const std::string& key) const {
        if (!logged_in_) return {};
        std::map<std::string, std::string> prefs = readUserPreferences(active_user_.username);
        auto found = prefs.find(key);
        return found == prefs.end() ? std::string() : found->second;
    }

    [[nodiscard]] ConsumerSettingsInfo settingsInfo() const {
        const ProductionProfile& profile = kernel_.productionProfile();
        ConsumerSettingsInfo info;
        info.display_width = profile.framebuffer_width;
        info.display_height = profile.framebuffer_height;
        info.disk_blocks = profile.disk_blocks;
        info.max_processes = profile.max_processes;
        info.max_windows = profile.max_windows;
        info.open_windows = static_cast<int>(taskbarTitles().size());
        info.user_count = static_cast<int>(readUsers().size());
        info.active_user = active_user_.username;
        return info;
    }

    [[nodiscard]] ConsumerDesktopSnapshot snapshot() const {
        ConsumerDesktopSnapshot out;
        out.phase = phase_;
        out.boot_splash_seen = boot_splash_seen_;
        out.first_run_required = firstRunRequired();
        out.logged_in = logged_in_;
        out.username = active_user_.username;
        out.home = active_user_.home;
        out.clock = "12:00";
        out.launcher = registry();
        out.taskbar_titles = taskbarTitles();
        out.active_window_id = active_window_id_;
        return out;
    }

    [[nodiscard]] const ConsumerDialog& lastDialog() const {
        return last_dialog_;
    }

    [[nodiscard]] StatusResult shutdown() {
        StatusResult synced = kernel_.shutdownSync();
        if (!synced.ok()) return synced;
        phase_ = ConsumerShellPhase::Shutdown;
        return StatusResult::success();
    }

    [[nodiscard]] std::vector<ConsumerAppEntry> registry() const {
        std::vector<long long> words;
        if (!readFile("/apps/registry", words) || words.empty()) {
            return defaultApps();
        }
        return parseRegistry(words);
    }

private:
    OSKernel& kernel_;
    int session_pid_ = 1;
    bool logged_in_ = false;
    bool boot_splash_seen_ = false;
    ConsumerShellPhase phase_ = ConsumerShellPhase::Off;
    ConsumerUserRecord active_user_;
    std::vector<ConsumerWindow> windows_;
    int active_window_id_ = -1;
    ConsumerDialog last_dialog_;

    [[nodiscard]] static std::vector<std::string> splitComponents(const std::string& path) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }

    [[nodiscard]] static std::string parentPath(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return "/";
        return path.substr(0, slash);
    }

    static void appendString(std::vector<long long>& out, const std::string& text) {
        out.push_back(static_cast<long long>(text.size()));
        for (unsigned char c : text) out.push_back(static_cast<long long>(c));
    }

    [[nodiscard]] static bool takeString(
        const std::vector<long long>& words,
        std::size_t& pos,
        std::string& out) {

        if (pos >= words.size()) return false;
        const long long rawLen = words[pos++];
        if (rawLen < 0) return false;
        const std::size_t len = static_cast<std::size_t>(rawLen);
        if (pos + len > words.size()) return false;
        out.clear();
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(static_cast<char>(words[pos++]));
        }
        return true;
    }

    [[nodiscard]] static std::vector<long long> asciiWords(const std::string& text) {
        std::vector<long long> out;
        out.reserve(text.size());
        for (unsigned char c : text) out.push_back(static_cast<long long>(c));
        return out;
    }

    [[nodiscard]] static std::vector<long long> registryWords(
        const std::vector<ConsumerAppEntry>& apps) {

        std::vector<long long> out;
        out.push_back(static_cast<long long>(apps.size()));
        for (const ConsumerAppEntry& app : apps) {
            appendString(out, app.id);
            appendString(out, app.title);
            appendString(out, app.path);
            out.push_back(app.capabilities);
            out.push_back(app.windowed ? 1 : 0);
        }
        return out;
    }

    [[nodiscard]] static std::vector<ConsumerAppEntry> parseRegistry(
        const std::vector<long long>& words) {

        std::vector<ConsumerAppEntry> apps;
        if (words.empty() || words[0] < 0) return apps;
        std::size_t pos = 1;
        const int count = static_cast<int>(words[0]);
        for (int i = 0; i < count && pos < words.size(); ++i) {
            ConsumerAppEntry app;
            if (!takeString(words, pos, app.id)) break;
            if (!takeString(words, pos, app.title)) break;
            if (!takeString(words, pos, app.path)) break;
            if (pos + 2 > words.size()) break;
            app.capabilities = static_cast<int>(words[pos++]);
            app.windowed = words[pos++] != 0;
            apps.push_back(app);
        }
        return apps;
    }

    [[nodiscard]] static std::vector<long long> userWords(
        const std::vector<ConsumerUserRecord>& users) {

        std::vector<long long> out;
        for (const ConsumerUserRecord& user : users) {
            appendString(out, user.username);
            out.push_back(user.password_hash);
            appendString(out, user.home);
            appendString(out, user.shell);
        }
        return out;
    }

    [[nodiscard]] static std::vector<long long> preferenceWords(
        const std::map<std::string, std::string>& prefs) {

        std::vector<long long> out;
        out.push_back(static_cast<long long>(prefs.size()));
        for (const auto& [key, value] : prefs) {
            appendString(out, key);
            appendString(out, value);
        }
        return out;
    }

    [[nodiscard]] static std::map<std::string, std::string> parsePreferences(
        const std::vector<long long>& words) {

        std::map<std::string, std::string> prefs;
        if (words.empty() || words[0] < 0) return prefs;
        std::size_t pos = 1;
        const int count = static_cast<int>(words[0]);
        for (int i = 0; i < count && pos < words.size(); ++i) {
            std::string key;
            std::string value;
            if (!takeString(words, pos, key)) break;
            if (!takeString(words, pos, value)) break;
            prefs[key] = value;
        }
        return prefs;
    }

    [[nodiscard]] bool readFile(
        const std::string& path,
        std::vector<long long>& words) const {

        StatusResult read = kernel_.fs().readFile(path, words);
        return read.ok() || read.status == T1_PENDING;
    }

    [[nodiscard]] bool fileExists(const std::string& path) const {
        return kernel_.fs().lookup(path).ok();
    }

    [[nodiscard]] StatusResult ensureDirectory(const std::string& path) {
        if (path.empty() || path[0] != '/') return StatusResult::error(ERR_INVALID);
        if (path == "/") return StatusResult::success(0);
        std::string current;
        for (const std::string& part : splitComponents(path)) {
            current += "/";
            current += part;
            StatusResult found = kernel_.fs().lookup(current);
            if (found.ok()) {
                FileStat stat;
                StatusResult stated = kernel_.fs().stat(current, stat);
                if (!stated.ok() || stat.kind != InodeKind::Directory) {
                    return StatusResult::error(ERR_NOT_DIR);
                }
                continue;
            }
            StatusResult made = kernel_.fs().createFile(current, InodeKind::Directory);
            if (!made.ok() && made.detail != ERR_EXISTS) return made;
        }
        return StatusResult::success();
    }

    [[nodiscard]] StatusResult writeFileCreating(
        const std::string& path,
        const std::vector<long long>& words) {

        StatusResult parent = ensureDirectory(parentPath(path));
        if (!parent.ok()) return parent;
        StatusResult found = kernel_.fs().lookup(path);
        if (!found.ok()) {
            StatusResult created = kernel_.fs().createFile(path, InodeKind::File);
            if (!created.ok() && created.detail != ERR_EXISTS) return created;
        }
        return kernel_.fs().writeFile(path, words);
    }

    [[nodiscard]] StatusResult writeRegistry(
        const std::vector<ConsumerAppEntry>& apps) {

        return writeFileCreating("/apps/registry", registryWords(apps));
    }

    [[nodiscard]] std::vector<ConsumerUserRecord> readUsers() const {
        std::vector<long long> words;
        std::vector<ConsumerUserRecord> users;
        if (!readFile("/etc/users", words)) return users;
        std::size_t pos = 0;
        while (pos < words.size()) {
            ConsumerUserRecord user;
            if (!takeString(words, pos, user.username)) break;
            if (pos >= words.size()) break;
            user.password_hash = words[pos++];
            if (!takeString(words, pos, user.home)) break;
            if (!takeString(words, pos, user.shell)) break;
            users.push_back(user);
        }
        return users;
    }

    [[nodiscard]] std::string prefPath(const std::string& username) const {
        return "/home/" + username + "/.prefs";
    }

    [[nodiscard]] std::map<std::string, std::string> readUserPreferences(
        const std::string& username) const {

        std::vector<long long> words;
        if (!readFile(prefPath(username), words)) return {};
        return parsePreferences(words);
    }

    [[nodiscard]] StatusResult writeUserPreferences(
        const std::string& username,
        const std::map<std::string, std::string>& prefs) {

        StatusResult home = ensureDirectory("/home/" + username);
        if (!home.ok()) return home;
        return writeFileCreating(prefPath(username), preferenceWords(prefs));
    }

    [[nodiscard]] std::optional<ConsumerAppEntry> findApp(const std::string& id) const {
        for (const ConsumerAppEntry& app : registry()) {
            if (app.id == id) return app;
        }
        return std::nullopt;
    }

    [[nodiscard]] ConsumerWindow* findWindow(int window_id) {
        for (ConsumerWindow& window : windows_) {
            if (window.window_id == window_id) return &window;
        }
        return nullptr;
    }

    [[nodiscard]] int firstOpenWindowId() const {
        for (const ConsumerWindow& window : windows_) {
            if (window.open) return window.window_id;
        }
        return -1;
    }

    [[nodiscard]] std::vector<std::string> taskbarTitles() const {
        std::vector<std::string> titles;
        for (const ConsumerWindow& window : windows_) {
            if (window.open) titles.push_back(window.title);
        }
        return titles;
    }
};

} // namespace os
} // namespace sandbox

#endif // TERNARY_CONSUMER_SHELL_H
