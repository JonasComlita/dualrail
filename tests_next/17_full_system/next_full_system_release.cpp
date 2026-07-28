#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_host_runtime.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef TRIT_BUILD_TOS_IMAGE_PATH
#define TRIT_BUILD_TOS_IMAGE_PATH "build/build_tos_image.exe"
#endif

namespace {

using tests_next::TestCase;
using tests_next::TestContext;

constexpr const char* kReleaseVersion = "tests_next_release";
constexpr int kReleaseDiskBlocks = 32768;

struct SparseDiskRecord {
    int block = -1;
    std::vector<long long> words;
};

struct ReleaseRegistryEntry {
    std::string id;
    std::string title;
    std::string path;
    int capability_mask = 0;
    bool windowed = false;
};

struct NativeVfsView {
    bool valid = false;
    int next_dirent = 0;
    int next_extent = 0;
    std::vector<long long> inodes;
    std::vector<long long> dirents;
    std::vector<long long> names;
    std::vector<long long> extents;
    std::vector<long long> data;
};

struct ReleaseFixture {
    tests_next::TempWorkspace workspace{"trit_next_release"};
    std::filesystem::path boot_path;
    std::filesystem::path disk_path;
    std::filesystem::path diagnostics_path;
    sandbox::host::TosBootImage image;
    std::vector<SparseDiskRecord> disk_records;
    bool attempted = false;
    bool ok = false;
    std::string error;

    ReleaseFixture()
        : boot_path(workspace.path() / "ternary-os.tboot"),
          disk_path(workspace.path() / "ternary-os.tdisk"),
          diagnostics_path(workspace.path() / "diagnostics") {}
};

std::string quotePath(const std::filesystem::path& path) {
    std::string text = path.string();
    std::replace(text.begin(), text.end(), '/', '\\');
    return "\"" + text + "\"";
}

std::string nativePath(const std::filesystem::path& path) {
    std::string text = path.string();
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
}

std::string buildImageCommand(const std::filesystem::path& builder,
                              const std::filesystem::path& boot_path,
                              const std::filesystem::path& disk_path) {
#ifdef _WIN32
    return "cmd /C \"\"" + nativePath(builder) + "\" " +
           quotePath(boot_path) + " " +
           quotePath(disk_path) + " " +
           kReleaseVersion + "\"";
#else
    return quotePath(builder) + " " +
           quotePath(boot_path) + " " +
           quotePath(disk_path) + " " +
           kReleaseVersion;
#endif
}

bool readSparseDiskRecords(const std::filesystem::path& path,
                           std::vector<SparseDiskRecord>& records,
                           std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        error = "failed to open sparse disk: " + path.string();
        return false;
    }

    std::uint64_t magic = 0;
    int count = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in.good()) {
        error = "sparse disk header is truncated";
        return false;
    }
    if (magic != sandbox::host::TOS_SPARSE_DISK_MAGIC || count < 0) {
        error = "sparse disk header is invalid";
        return false;
    }

    records.clear();
    records.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        SparseDiskRecord record;
        record.words.assign(
            static_cast<std::size_t>(sandbox::vm::STORAGE_BLOCK_WORDS),
            0);
        in.read(reinterpret_cast<char*>(&record.block), sizeof(record.block));
        for (int word = 0;
             word < sandbox::vm::STORAGE_BLOCK_WORDS;
             ++word) {
            in.read(reinterpret_cast<char*>(&record.words[static_cast<std::size_t>(word)]),
                    sizeof(long long));
        }
        if (!in.good()) {
            error = "sparse disk block record is truncated";
            return false;
        }
        records.push_back(std::move(record));
    }
    return true;
}

std::vector<long long> flattenDiskRecords(const std::vector<SparseDiskRecord>& records) {
    std::vector<long long> words;
    for (const SparseDiskRecord& record : records) {
        words.insert(words.end(), record.words.begin(), record.words.end());
    }
    return words;
}

std::vector<long long> expandDiskRecords(const std::vector<SparseDiskRecord>& records,
                                         int block_count) {
    std::vector<long long> words(
        static_cast<std::size_t>(
            block_count * sandbox::vm::STORAGE_BLOCK_WORDS),
        0);
    for (const SparseDiskRecord& record : records) {
        if (record.block < 0 || record.block >= block_count) continue;
        const std::size_t base =
            static_cast<std::size_t>(
                record.block * sandbox::vm::STORAGE_BLOCK_WORDS);
        const std::size_t count = std::min(
            record.words.size(),
            static_cast<std::size_t>(sandbox::vm::STORAGE_BLOCK_WORDS));
        std::copy(record.words.begin(), record.words.begin() + count,
                  words.begin() + static_cast<std::vector<long long>::difference_type>(base));
    }
    return words;
}

std::vector<long long> asciiWords(const std::string& text) {
    std::vector<long long> words;
    words.reserve(text.size());
    for (unsigned char ch : text) words.push_back(static_cast<long long>(ch));
    return words;
}

bool containsSequence(const std::vector<long long>& haystack,
                      const std::vector<long long>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end()) != haystack.end();
}

bool containsAscii(const std::vector<long long>& haystack, const std::string& text) {
    return containsSequence(haystack, asciiWords(text));
}

std::string asciiFromWords(const std::vector<long long>& words) {
    std::string text;
    text.reserve(words.size());
    for (long long word : words) {
        if (word >= 0 && word <= 255) {
            text.push_back(static_cast<char>(word));
        }
    }
    return text;
}

bool takeRegistryString(const std::vector<long long>& words,
                        std::size_t& pos,
                        std::string& out) {
    if (pos >= words.size() || words[pos] < 0) return false;
    const std::size_t length = static_cast<std::size_t>(words[pos++]);
    if (length > words.size() - pos) return false;
    out.clear();
    out.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        const long long word = words[pos++];
        if (word < 0 || word > 255) return false;
        out.push_back(static_cast<char>(word));
    }
    return true;
}

std::vector<ReleaseRegistryEntry> parseReleaseRegistry(
    const std::vector<long long>& words) {
    std::vector<ReleaseRegistryEntry> entries;
    if (words.empty() || words[0] < 0) return entries;

    const int count = static_cast<int>(words[0]);
    std::size_t pos = 1;
    entries.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count && pos < words.size(); ++i) {
        ReleaseRegistryEntry entry;
        if (!takeRegistryString(words, pos, entry.id)) break;
        if (!takeRegistryString(words, pos, entry.title)) break;
        if (!takeRegistryString(words, pos, entry.path)) break;
        if (pos + 2 > words.size()) break;
        entry.capability_mask = static_cast<int>(words[pos++]);
        entry.windowed = words[pos++] != 0;
        entries.push_back(std::move(entry));
    }
    return entries;
}

const ReleaseRegistryEntry* findRegistryEntry(
    const std::vector<ReleaseRegistryEntry>& entries,
    const std::string& id) {
    for (const ReleaseRegistryEntry& entry : entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

std::vector<long long> nativeDiskRange(const std::vector<long long>& disk_image,
                                       int first_block,
                                       int word_count) {
    std::vector<long long> words(static_cast<std::size_t>(word_count), 0);
    const std::size_t first =
        static_cast<std::size_t>(first_block * sandbox::os::BLOCK_WORDS);
    for (int i = 0; i < word_count; ++i) {
        const std::size_t src = first + static_cast<std::size_t>(i);
        if (src >= disk_image.size()) break;
        words[static_cast<std::size_t>(i)] = disk_image[src];
    }
    return words;
}

NativeVfsView decodeNativeVfs(const std::vector<long long>& disk_image) {
    NativeVfsView view;
    if (disk_image.size() < static_cast<std::size_t>(sandbox::os::BLOCK_WORDS)) {
        return view;
    }
    if (disk_image[0] != sandbox::os::NATIVE_VFS_MAGIC ||
        disk_image[1] != sandbox::os::NATIVE_VFS_VERSION ||
        disk_image[2] != sandbox::os::BLOCK_WORDS) {
        return view;
    }
    view.next_dirent = static_cast<int>(disk_image[9]);
    view.next_extent = static_cast<int>(disk_image[10]);
    view.inodes = nativeDiskRange(
        disk_image, sandbox::os::NATIVE_VFS_DISK_INODE_BLOCK,
        sandbox::os::NATIVE_VFS_MAX_INODES * sandbox::os::NATIVE_VFS_INODE_WORDS);
    view.dirents = nativeDiskRange(
        disk_image, sandbox::os::NATIVE_VFS_DISK_DIRENT_BLOCK,
        sandbox::os::NATIVE_VFS_MAX_DIRENTS * sandbox::os::NATIVE_VFS_DIRENT_WORDS);
    view.names = nativeDiskRange(
        disk_image, sandbox::os::NATIVE_VFS_DISK_DIRENT_NAME_BLOCK,
        sandbox::os::NATIVE_VFS_MAX_DIRENTS * sandbox::os::NATIVE_VFS_MAX_NAME_WORDS);
    view.extents = nativeDiskRange(
        disk_image, sandbox::os::NATIVE_VFS_DISK_EXTENT_BLOCK,
        sandbox::os::NATIVE_VFS_MAX_EXTENTS * sandbox::os::NATIVE_VFS_EXTENT_WORDS);
    view.data = nativeDiskRange(
        disk_image, sandbox::os::NATIVE_VFS_DISK_DATA_BLOCK,
        sandbox::os::NATIVE_VFS_PAYLOAD_WORDS);
    view.valid = true;
    return view;
}

std::vector<std::string> nativePathComponents(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') ++pos;
        const std::size_t begin = pos;
        while (pos < path.size() && path[pos] != '/') ++pos;
        if (pos > begin) parts.push_back(path.substr(begin, pos - begin));
    }
    return parts;
}

bool nativeDirentNameEquals(const NativeVfsView& view,
                            int slot,
                            const std::string& name) {
    const int dirent_base = slot * sandbox::os::NATIVE_VFS_DIRENT_WORDS;
    if (dirent_base + 5 >= static_cast<int>(view.dirents.size())) return false;
    const int length = static_cast<int>(view.dirents[dirent_base + 3]);
    if (length != static_cast<int>(name.size())) return false;
    const int name_base = slot * sandbox::os::NATIVE_VFS_MAX_NAME_WORDS;
    if (name_base + length > static_cast<int>(view.names.size())) return false;
    for (int i = 0; i < length; ++i) {
        if (view.names[static_cast<std::size_t>(name_base + i)] !=
            static_cast<unsigned char>(name[static_cast<std::size_t>(i)])) {
            return false;
        }
    }
    return true;
}

int nativeLookup(const NativeVfsView& view, const std::string& path) {
    if (!view.valid || path.empty() || path[0] != '/') return -1;
    int inode = 0;
    for (const std::string& part : nativePathComponents(path)) {
        int found = -1;
        for (int slot = 0; slot < view.next_dirent; ++slot) {
            const int base = slot * sandbox::os::NATIVE_VFS_DIRENT_WORDS;
            if (base + 5 >= static_cast<int>(view.dirents.size())) break;
            if (view.dirents[base + 5] <= 0) continue;
            if (view.dirents[base + 1] != inode) continue;
            if (!nativeDirentNameEquals(view, slot, part)) continue;
            found = static_cast<int>(view.dirents[base + 4]);
            break;
        }
        if (found < 0) return -1;
        inode = found;
    }
    return inode;
}

bool readNativeFile(const NativeVfsView& view,
                    const std::string& path,
                    std::vector<long long>& out) {
    const int inode = nativeLookup(view, path);
    if (inode <= 0 || inode >= sandbox::os::NATIVE_VFS_MAX_INODES) return false;
    const int inode_base = inode * sandbox::os::NATIVE_VFS_INODE_WORDS;
    if (inode_base + 7 >= static_cast<int>(view.inodes.size())) return false;
    const int kind = static_cast<int>(view.inodes[inode_base + 0]);
    if (kind != sandbox::os::NATIVE_KIND_FILE &&
        kind != sandbox::os::NATIVE_KIND_EXEC) {
        return false;
    }
    const int size_words = static_cast<int>(view.inodes[inode_base + 2]);
    if (size_words < 0) return false;
    out.assign(static_cast<std::size_t>(size_words), 0);

    for (int slot = 0; slot < view.next_extent; ++slot) {
        const int base = slot * sandbox::os::NATIVE_VFS_EXTENT_WORDS;
        if (base + 5 >= static_cast<int>(view.extents.size())) break;
        if (view.extents[base + 5] <= 0) continue;
        if (view.extents[base + 1] != inode) continue;
        const int logical = static_cast<int>(view.extents[base + 2]);
        const int length = static_cast<int>(view.extents[base + 3]);
        const int data_offset =
            static_cast<int>(view.extents[base + 4]) - sandbox::os::NATIVE_VFS_DATA_BASE;
        if (logical < 0 || length < 0 || data_offset < 0) return false;
        for (int i = 0; i < length && logical + i < size_words; ++i) {
            const int src = data_offset + i;
            if (src < 0 || src >= static_cast<int>(view.data.size())) return false;
            out[static_cast<std::size_t>(logical + i)] =
                view.data[static_cast<std::size_t>(src)];
        }
    }
    return true;
}

bool nativeDiskBlockHasNonZeroWord(const std::vector<long long>& disk_image,
                                   int block,
                                   int word_limit) {
    if (block < 0 || word_limit <= 0) return false;
    const int limit = std::min(word_limit, sandbox::os::BLOCK_WORDS);
    const std::size_t base =
        static_cast<std::size_t>(block * sandbox::os::BLOCK_WORDS);
    if (base >= disk_image.size()) return false;
    for (int i = 0; i < limit; ++i) {
        const std::size_t index = base + static_cast<std::size_t>(i);
        if (index >= disk_image.size()) return false;
        if (disk_image[index] != 0) return true;
    }
    return false;
}

std::vector<std::string> jsonObjectBlocksInArray(const std::string& text,
                                                 const std::string& array_key) {
    std::vector<std::string> objects;
    const std::string marker = "\"" + array_key + "\"";
    const std::size_t key_pos = text.find(marker);
    if (key_pos == std::string::npos) return objects;
    const std::size_t array_begin = text.find('[', key_pos + marker.size());
    if (array_begin == std::string::npos) return objects;

    int array_depth = 1;
    int object_depth = 0;
    std::size_t object_begin = std::string::npos;
    for (std::size_t pos = array_begin + 1; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '[' && object_depth == 0) {
            ++array_depth;
        } else if (ch == ']' && object_depth == 0) {
            --array_depth;
            if (array_depth == 0) break;
        } else if (ch == '{') {
            if (object_depth == 0) object_begin = pos;
            ++object_depth;
        } else if (ch == '}') {
            --object_depth;
            if (object_depth == 0 && object_begin != std::string::npos) {
                objects.push_back(text.substr(object_begin, pos - object_begin + 1));
                object_begin = std::string::npos;
            }
        }
    }
    return objects;
}

long long jsonIntValue(const std::string& object,
                       const std::string& key,
                       long long missing = -1) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_pos = object.find(marker);
    if (key_pos == std::string::npos) return missing;
    const std::size_t colon = object.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return missing;
    std::size_t pos = colon + 1;
    while (pos < object.size() &&
           std::isspace(static_cast<unsigned char>(object[pos]))) {
        ++pos;
    }
    long long sign = 1;
    if (pos < object.size() && object[pos] == '-') {
        sign = -1;
        ++pos;
    }
    if (pos >= object.size() ||
        !std::isdigit(static_cast<unsigned char>(object[pos]))) {
        return missing;
    }
    long long value = 0;
    while (pos < object.size() &&
           std::isdigit(static_cast<unsigned char>(object[pos]))) {
        value = value * 10 + (object[pos] - '0');
        ++pos;
    }
    return sign * value;
}

long long appLaunchCountForInode(const std::string& registry_json, int inode) {
    for (const std::string& object :
         jsonObjectBlocksInArray(registry_json, "entries")) {
        if (jsonIntValue(object, "inode") == inode) {
            return jsonIntValue(object, "launch_count");
        }
    }
    return -1;
}

std::string processObjectForPid(const std::string& process_table_json, int pid) {
    for (const std::string& object :
         jsonObjectBlocksInArray(process_table_json, "slots")) {
        if (jsonIntValue(object, "pid") == pid) return object;
    }
    return {};
}

std::string tailText(const std::string& text, std::size_t max_chars) {
    return text.size() > max_chars ? text.substr(text.size() - max_chars) : text;
}

std::string textAround(const std::string& text,
                       const std::string& needle,
                       std::size_t max_chars) {
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) return tailText(text, max_chars);
    const std::size_t half = max_chars / 2;
    const std::size_t begin = pos > half ? pos - half : 0;
    const std::size_t end = std::min(text.size(), begin + max_chars);
    return text.substr(begin, end - begin);
}

std::string framebufferRow(const sandbox::host::TosFramebufferSnapshot& framebuffer,
                           int row) {
    if (row < 0 || row >= framebuffer.height || framebuffer.width <= 0) return {};
    const std::size_t begin = static_cast<std::size_t>(row * framebuffer.width);
    const std::size_t end = begin + static_cast<std::size_t>(framebuffer.width);
    if (end > framebuffer.glyphs.size()) return {};

    std::string line;
    line.reserve(static_cast<std::size_t>(framebuffer.width));
    for (std::size_t i = begin; i < end; ++i) {
        const char glyph = framebuffer.glyphs[i];
        line.push_back(glyph == '\0' ? ' ' : glyph);
    }
    return line;
}

std::string framebufferText(const sandbox::host::TosFramebufferSnapshot& framebuffer) {
    std::string text;
    for (int row = 0; row < framebuffer.height; ++row) {
        text += framebufferRow(framebuffer, row);
        text.push_back('\n');
    }
    return text;
}

int visibleGlyphCount(const sandbox::host::TosFramebufferSnapshot& framebuffer) {
    int count = 0;
    for (char glyph : framebuffer.glyphs) {
        if (glyph != '\0' && glyph != ' ') ++count;
    }
    return count;
}

bool runUntilFramebufferContains(TestContext& ctx,
                                 sandbox::host::TosRuntime& runtime,
                                 const std::string& needle,
                                 int max_steps,
                                 int chunk_steps,
                                 const std::string& message) {
    int executed = 0;
    sandbox::host::TosRuntimeSnapshot last_snapshot;
    sandbox::host::TosFramebufferSnapshot last_framebuffer;
    std::string last_frame;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        last_snapshot = runtime.snapshot();
        last_framebuffer = runtime.readFramebuffer();
        last_frame = framebufferText(last_framebuffer);
        if (last_frame.find(needle) != std::string::npos) return true;
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " missing='" + needle + "' after steps=" +
             std::to_string(executed) +
             " gpu_mode=" + std::to_string(last_snapshot.gpu_mode) +
             " framebuffer_mode=" +
             (last_framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60
                  ? "graphics"
                  : "text") +
             " size=" + std::to_string(last_framebuffer.width) + "x" +
             std::to_string(last_framebuffer.height) +
             " frame_tail='" + tailText(last_frame, 900) + "'");
    return false;
}

bool runUntilDiagnosticFileContains(TestContext& ctx,
                                    sandbox::host::TosRuntime& runtime,
                                    const std::filesystem::path& diagnostics_path,
                                    const std::string& filename,
                                    const std::string& needle,
                                    int max_steps,
                                    int chunk_steps,
                                    const std::string& message) {
    int executed = 0;
    std::string error;
    std::string last_text;
    std::vector<std::string> samples;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        if (!runtime.exportDiagnostics(diagnostics_path.string(), &error)) {
            ctx.fail(message + " diagnostic export failed: " + error);
            return false;
        }
        last_text = tests_next::readText(diagnostics_path / filename);
        if (last_text.find(needle) != std::string::npos) return true;
        samples.push_back("steps=" + std::to_string(executed) + " " +
                          tests_next::readText(diagnostics_path / "vm_state.txt"));
        if (samples.size() > 6) {
            samples.erase(samples.begin());
        }
        if (result.steps <= 0) break;
    }

    const std::string tail =
        last_text.size() > 240 ? last_text.substr(last_text.size() - 240) : last_text;
    const std::string process_table =
        tests_next::readText(diagnostics_path / "process_table.json");
    ctx.fail(message + " missing='" + needle + "' in " + filename +
             " after steps=" + std::to_string(executed) +
             " tail='" + tail +
             "' vm_state='" +
             tests_next::readText(diagnostics_path / "vm_state.txt") +
             "' process_header='" +
             tailText(process_table.substr(0, std::min<std::size_t>(process_table.size(), 140)),
                      140) +
             "' pid_106='" +
             textAround(process_table, "\"pid\": 106", 900) +
             "' samples='" +
             tailText([&samples]() {
                 std::string joined;
                 for (const std::string& sample : samples) {
                     joined += sample;
                     joined += "\n---\n";
                 }
                 return joined;
             }(), 2600) +
             "'");
    return false;
}

bool runUntilAppLaunchCountAtLeast(TestContext& ctx,
                                   sandbox::host::TosRuntime& runtime,
                                   const std::filesystem::path& diagnostics_path,
                                   int inode,
                                   long long min_launch_count,
                                   int max_steps,
                                   int chunk_steps,
                                   const std::string& message) {
    int executed = 0;
    std::string error;
    long long last_count = -1;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        if (!runtime.exportDiagnostics(diagnostics_path.string(), &error)) {
            ctx.fail(message + " diagnostic export failed: " + error);
            return false;
        }
        const std::string registry_json =
            tests_next::readText(diagnostics_path / "app_registry.json");
        last_count = appLaunchCountForInode(registry_json, inode);
        if (last_count >= min_launch_count) return true;
        if (result.steps <= 0) break;
    }

    const std::string process_table =
        tests_next::readText(diagnostics_path / "process_table.json");
    const std::string framebuffer =
        tests_next::readText(diagnostics_path / "framebuffer_snapshot.txt");
    const std::string syscall_trace =
        tests_next::readText(diagnostics_path / "syscall_trace.jsonl");
    const std::string guest_log =
        tests_next::readText(diagnostics_path / "guest.log");
    ctx.fail(message + " inode=" + std::to_string(inode) +
             " launch_count=" + std::to_string(last_count) +
             " want_at_least=" + std::to_string(min_launch_count) +
             " after steps=" + std::to_string(executed) +
             " vm_state='" +
             tests_next::readText(diagnostics_path / "vm_state.txt") +
             "' process_header='" +
             tailText(process_table.substr(0, std::min<std::size_t>(process_table.size(), 140)),
                      140) +
             "' pid_106='" +
             textAround(process_table, "\"pid\": 106", 900) +
             "' syscall_trace='" +
             tailText(syscall_trace, 1200) +
             "' guest_log='" +
             tailText(guest_log, 1200) +
             "' framebuffer='" +
             tailText(framebuffer, 900) +
             "'");
    return false;
}

bool runUntilPaintGraphicsReady(TestContext& ctx,
                                sandbox::host::TosRuntime& runtime,
                                int max_steps,
                                int chunk_steps,
                                const std::string& message) {
    int executed = 0;
    sandbox::host::TosRuntimeSnapshot last_snapshot;
    sandbox::host::TosFramebufferSnapshot last_framebuffer;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        last_snapshot = runtime.snapshot();
        last_framebuffer = runtime.readFramebuffer();
        if (last_snapshot.gpu_mode == 1 &&
            last_framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60 &&
            last_framebuffer.width == 80 &&
            last_framebuffer.height == 60 &&
            last_framebuffer.sprite_attr == 43 + 14 * 256) {
            return true;
        }
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " gpu_mode=" + std::to_string(last_snapshot.gpu_mode) +
             " framebuffer_mode=" +
             (last_framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60
                  ? "graphics"
                  : "text") +
             " size=" + std::to_string(last_framebuffer.width) + "x" +
             std::to_string(last_framebuffer.height) +
             " sprite_attr=" + std::to_string(last_framebuffer.sprite_attr) +
             " after steps=" + std::to_string(executed));
    return false;
}

bool runUntilGraphicsWordEquals(TestContext& ctx,
                                sandbox::host::TosRuntime& runtime,
                                std::size_t index,
                                long long expected,
                                int max_steps,
                                int chunk_steps,
                                const std::string& message) {
    int executed = 0;
    long long last_value = -999999;
    sandbox::host::TosFramebufferMemorySnapshot raw;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        raw = runtime.readFramebufferMemory();
        if (raw.mode == sandbox::host::TosFramebufferMode::Graphics80x60 &&
            index < raw.words.size()) {
            last_value = raw.words[index];
            if (last_value == expected) return true;
        }
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " index=" + std::to_string(index) +
             " value=" + std::to_string(last_value) +
             " expected=" + std::to_string(expected) +
             " mode=" +
             (raw.mode == sandbox::host::TosFramebufferMode::Graphics80x60
                  ? "graphics"
                  : "text") +
             " words=" + std::to_string(raw.words.size()) +
             " after steps=" + std::to_string(executed));
    return false;
}

bool runUntilSpriteAttrEquals(TestContext& ctx,
                              sandbox::host::TosRuntime& runtime,
                              long long expected,
                              int max_steps,
                              int chunk_steps,
                              const std::string& message) {
    int executed = 0;
    sandbox::host::TosFramebufferSnapshot framebuffer;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        framebuffer = runtime.readFramebuffer();
        if (framebuffer.sprite_attr == expected) return true;
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " sprite_attr=" +
             std::to_string(framebuffer.sprite_attr) +
             " expected=" + std::to_string(expected) +
             " after steps=" + std::to_string(executed));
    return false;
}

bool runUntilProcessFieldEquals(TestContext& ctx,
                                sandbox::host::TosRuntime& runtime,
                                const std::filesystem::path& diagnostics_path,
                                int pid,
                                const std::string& field,
                                long long expected,
                                int max_steps,
                                int chunk_steps,
                                const std::string& message) {
    int executed = 0;
    std::string error;
    std::string last_table;
    std::string last_object;
    long long last_value = -999999;
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }

        if (!runtime.exportDiagnostics(diagnostics_path.string(), &error)) {
            ctx.fail(message + " diagnostic export failed: " + error);
            return false;
        }
        last_table = tests_next::readText(diagnostics_path / "process_table.json");
        last_object = processObjectForPid(last_table, pid);
        if (!last_object.empty()) {
            last_value = jsonIntValue(last_object, field, -999999);
            if (last_value == expected) return true;
        }
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " pid=" + std::to_string(pid) +
             " field=" + field +
             " value=" + std::to_string(last_value) +
             " want=" + std::to_string(expected) +
             " after steps=" + std::to_string(executed) +
             " process='" + tailText(last_object.empty() ? last_table : last_object, 1200) +
             "' crash_report='" +
             tailText(tests_next::readText(diagnostics_path / "crash_report.txt"), 1200) +
             "'");
    return false;
}

bool runUntilGuestRebootCountAtLeast(TestContext& ctx,
                                     sandbox::host::TosRuntime& runtime,
                                     std::uint64_t min_reboot_count,
                                     int max_steps,
                                     int chunk_steps,
                                     const std::string& message) {
    int executed = 0;
    sandbox::host::TosRuntimeSnapshot last = runtime.snapshot();
    while (executed < max_steps) {
        const int step_count = std::min(chunk_steps, max_steps - executed);
        const auto result = runtime.runForSteps(step_count);
        executed += result.steps;
        if (result.trapped()) {
            ctx.fail(message + " trapped: " + result.description);
            return false;
        }
        last = runtime.snapshot();
        if (last.guest_reboot_count >= min_reboot_count) return true;
        if (result.steps <= 0) break;
    }

    ctx.fail(message + " guest_reboot_count=" +
             std::to_string(last.guest_reboot_count) +
             " want_at_least=" + std::to_string(min_reboot_count) +
             " boot_generation=" + std::to_string(last.boot_generation) +
             " after steps=" + std::to_string(executed));
    return false;
}

bool driveReleaseLoginToDesktop(TestContext& ctx, sandbox::host::TosRuntime& runtime) {
    if (!runUntilFramebufferContains(ctx, runtime, "OS 3 SECURE",
                                     8000000, 500000,
                                     "release reaches login screen")) {
        return false;
    }
    std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "USER: ADMIN", "release login screen shows admin user");
    ctx.contains(frame, "PASS:", "release login screen shows passcode field");

    runtime.pushKeyboardInput('7');
    runtime.pushKeyboardInput('7');
    runtime.pushKeyboardInput('7');
    runtime.pushKeyboardInput(13);
    if (!runUntilFramebufferContains(ctx, runtime, "OS 3 - DESKTOP",
                                     2500000, 250000,
                                     "release passcode reaches desktop")) {
        return false;
    }
    frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "LAUNCH PAD", "release desktop shows launcher");
    ctx.contains(frame, "1. CALC", "release desktop lists calculator hotkey");
    ctx.contains(frame, "KEYS 1-6 LAUNCH", "release desktop exposes hotkey instructions");
    return ctx.failures() == 0;
}

const sandbox::host::TosAppManifestEntry* findApp(
    const sandbox::host::TosBootImage& image,
    const std::string& name) {
    for (const auto& app : image.manifest.apps) {
        if (app.name == name) return &app;
    }
    return nullptr;
}

const sandbox::host::TosImageSection* findSection(
    const sandbox::host::TosBootImage& image,
    const std::string& name) {
    for (const auto& section : image.manifest.sections) {
        if (section.name == name) return &section;
    }
    return nullptr;
}

ReleaseFixture& fixtureRef() {
    static ReleaseFixture fixture;
    return fixture;
}

bool ensureFixture(TestContext& ctx) {
    ReleaseFixture& fixture = fixtureRef();
    if (fixture.attempted) {
        if (!fixture.ok) ctx.fail(fixture.error);
        return fixture.ok;
    }
    fixture.attempted = true;

    const std::filesystem::path builder(TRIT_BUILD_TOS_IMAGE_PATH);
    ctx.check(std::filesystem::exists(builder),
              "build_tos_image executable exists at configured path");
    if (!std::filesystem::exists(builder)) {
        fixture.error = "missing build_tos_image executable: " + builder.string();
        ctx.fail(fixture.error);
        return false;
    }

    const std::string command =
        buildImageCommand(builder, fixture.boot_path, fixture.disk_path);
    const int exit_code = std::system(command.c_str());
    if (exit_code != 0) {
        fixture.error = "build_tos_image failed with exit code " + std::to_string(exit_code);
        ctx.fail(fixture.error);
        return false;
    }
    ctx.check(std::filesystem::exists(fixture.boot_path),
              "release .tboot artifact is written");
    ctx.check(std::filesystem::exists(fixture.disk_path),
              "release .tdisk artifact is written");

    if (!sandbox::host::readBootImageFile(fixture.boot_path.string(),
                                          fixture.image,
                                          &fixture.error)) {
        ctx.fail("failed to read release boot image: " + fixture.error);
        return false;
    }
    if (!readSparseDiskRecords(fixture.disk_path,
                               fixture.disk_records,
                               fixture.error)) {
        ctx.fail(fixture.error);
        return false;
    }

    fixture.ok = true;
    return true;
}

void releaseArtifactManifest(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    const ReleaseFixture& fixture = fixtureRef();
    const sandbox::host::TosBootImage& image = fixture.image;

    ctx.equal(image.manifest.format_version, sandbox::host::TOS_BOOT_FORMAT_VERSION,
              "release boot image uses the current format");
    ctx.equal(image.manifest.image_version, std::string(kReleaseVersion),
              "release boot image records the requested version");
    ctx.equal(image.manifest.profile_name, std::string("minimum"),
              "release boot image uses the minimum profile");
    ctx.check(!image.program.empty(), "release boot image has executable text");
    ctx.check(image.manifest.boot_entry >= 0 &&
                  image.manifest.boot_entry < static_cast<int>(image.program.size()),
              "boot entry is inside the executable text");
    ctx.check(image.rootfs_words.empty(),
              "release boot image keeps mutable rootfs in the companion .tdisk");

    const sandbox::host::TosImageSection* kernel = findSection(image, "kernel");
    ctx.check(kernel != nullptr, "release manifest has a kernel section");
    if (kernel != nullptr) {
        ctx.equal(kernel->path, std::string("/kernel"), "kernel section path is stable");
        ctx.equal(kernel->kind, std::string("kernel"), "kernel section kind is stable");
        ctx.check((kernel->flags & sandbox::host::TOS_IMAGE_SECTION_KERNEL) != 0,
                  "kernel section carries kernel flag");
    }

    ctx.check(image.manifest.apps.size() >= 50,
              "release boot image records bundled app metadata");
    ctx.equal(image.manifest.sections.size(),
              image.manifest.apps.size() + 1,
              "section table contains kernel plus one section per bundled app");

    std::set<int> text_ppns;
    for (const auto& app : image.manifest.apps) {
        ctx.check(!app.name.empty() && !app.path.empty(),
                  "app manifest entry has name and path");
        ctx.check(app.text_ppn > 0, app.name + " has a text PPN");
        ctx.check(text_ppns.insert(app.text_ppn).second,
                  app.name + " text PPN is unique");
        ctx.check(findSection(image, app.name) != nullptr,
                  app.name + " has a matching section entry");
    }

    for (const std::string& gui_app :
         {"desktop", "terminal", "files", "settings", "tasks",
          "calculator", "paint", "text_editor", "about", "help"}) {
        const auto* app = findApp(image, gui_app);
        ctx.check(app != nullptr, "GUI app appears in release manifest: " + gui_app);
        if (app != nullptr) {
            ctx.equal(app->stack_words, 1024,
                      gui_app + " release stack hint is preserved");
            ctx.check(app->text_pages > 0 && app->data_pages > 0,
                      gui_app + " release image metadata has text/data pages");
        }
    }
}

void releaseDiskRootMetadata(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    const ReleaseFixture& fixture = fixtureRef();
    ctx.check(!fixture.disk_records.empty(),
              "release sparse disk has non-zero block records");
    ctx.equal(fixture.disk_records.front().block, 0,
              "first sparse disk record is the superblock");
    ctx.check(!fixture.disk_records.front().words.empty() &&
                  fixture.disk_records.front().words[0] == sandbox::os::NATIVE_VFS_MAGIC,
              "release disk superblock uses native VFS magic");

    const std::vector<long long> disk_words = flattenDiskRecords(fixture.disk_records);
    ctx.check(containsAscii(disk_words, "Ternary OS " + std::string(kReleaseVersion) + "\n"),
              "release disk embeds /etc/release version");
    ctx.check(containsAscii(disk_words, "base-system installed\n"),
              "release disk embeds package status");
    ctx.check(containsAscii(disk_words, "sessiond starts login and desktop sessions\n"),
              "release disk embeds service manifest");
    ctx.check(containsAscii(disk_words, "/bin/desktop desktop desktop\n"),
              "release disk embeds bin manifest desktop entry");
    ctx.check(containsAscii(disk_words, "PROFILE=minimum\n"),
              "release disk embeds OS profile metadata");
    ctx.check(containsAscii(disk_words, "os3.cfg"),
              "release disk embeds the default Settings config path");
    ctx.check(containsSequence(disk_words, std::vector<long long>{7, 1, 8, 0}),
              "release disk embeds default Settings config words");
}

void releaseRootfsRegistryDecoding(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    const ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");

    std::vector<long long> registry_words;
    ctx.check(readNativeFile(root, "/apps/registry", registry_words),
              "release root exposes /apps/registry as a real VFS file");
    const std::vector<ReleaseRegistryEntry> registry =
        parseReleaseRegistry(registry_words);
    ctx.equal(static_cast<int>(registry.size()), 10,
              "release GUI registry decodes the expected launcher app count");

    const std::vector<std::pair<std::string, std::string>> expected_gui = {
        {"desktop", "/bin/desktop"},
        {"terminal", "/bin/terminal"},
        {"files", "/bin/file_manager"},
        {"settings", "/bin/settings"},
        {"tasks", "/bin/task_manager"},
        {"calculator", "/bin/calculator"},
        {"paint", "/bin/paint"},
        {"text_editor", "/bin/text_editor"},
        {"about", "/bin/about"},
        {"help", "/bin/help"},
    };
    for (const auto& [id, path] : expected_gui) {
        const ReleaseRegistryEntry* entry = findRegistryEntry(registry, id);
        ctx.check(entry != nullptr, "release GUI registry includes " + id);
        if (entry == nullptr) continue;
        ctx.equal(entry->path, path, id + " registry path matches release /bin path");
        ctx.equal(entry->capability_mask, 1,
                  id + " registry capability marker is preserved");
        ctx.check(entry->windowed, id + " registry marks launcher app as windowed");
    }

    std::vector<long long> bin_manifest_words;
    ctx.check(readNativeFile(root, "/system/bin.manifest", bin_manifest_words),
              "release root exposes /system/bin.manifest");
    const std::string bin_manifest = asciiFromWords(bin_manifest_words);
    ctx.contains(bin_manifest, "/bin/shell shell shell\n",
                 "bin manifest lists the shell executable");
    ctx.contains(bin_manifest, "/bin/terminal terminal terminal\n",
                 "bin manifest lists the terminal executable");
    ctx.contains(bin_manifest, "/bin/sync sync sync\n",
                 "bin manifest lists the sync command");
    ctx.contains(bin_manifest, "/bin/reboot reboot reboot\n",
                 "bin manifest lists the reboot command");
    ctx.contains(bin_manifest, "/bin/shutdown shutdown shutdown\n",
                 "bin manifest lists the shutdown command");

    std::vector<long long> settings_metadata_words;
    ctx.check(readNativeFile(root, "/apps/settings.app", settings_metadata_words),
              "release root exposes GUI app metadata sidecars");
    const std::string settings_metadata = asciiFromWords(settings_metadata_words);
    ctx.contains(settings_metadata, "id=settings\n",
                 "settings app metadata records the launcher id");
    ctx.contains(settings_metadata, "title=Settings\n",
                 "settings app metadata records the visible title");
    ctx.contains(settings_metadata, "path=/bin/settings\n",
                 "settings app metadata records the executable path");
    ctx.contains(settings_metadata, "windowed=1\n",
                 "settings app metadata records windowed launch behavior");
}

void releaseCliCommandDescriptors(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    const ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");

    const std::vector<std::pair<std::string, int>> expected_commands = {
        {"/bin/shell", 256},
        {"/bin/sh", 256},
        {"/bin/sync", 128},
        {"/bin/reboot", 128},
        {"/bin/shutdown", 128},
        {"/bin/crash", 128},
    };
    for (const auto& [path, stack_words] : expected_commands) {
        std::vector<long long> descriptor;
        ctx.check(readNativeFile(root, path, descriptor),
                  path + " descriptor is readable from the release rootfs");
        if (descriptor.empty()) continue;

        ctx.check(static_cast<int>(descriptor.size()) >=
                      sandbox::os::NATIVE_EXEC_DESC_V2_WORDS,
                  path + " uses the v2 disk-backed executable descriptor");
        if (static_cast<int>(descriptor.size()) <
            sandbox::os::NATIVE_EXEC_DESC_V2_WORDS) {
            continue;
        }
        ctx.equal(descriptor[sandbox::vm::EXEC_HEADER_MAGIC],
                  static_cast<long long>(sandbox::vm::EXEC_MAGIC),
                  path + " descriptor magic matches executable format");
        ctx.equal(descriptor[sandbox::vm::EXEC_HEADER_VERSION],
                  static_cast<long long>(sandbox::vm::EXEC_VERSION_V1),
                  path + " descriptor version is v1");
        ctx.equal(descriptor[sandbox::vm::EXEC_HEADER_ABI_VERSION],
                  static_cast<long long>(sandbox::vm::EXEC_ABI_VERSION_V1),
                  path + " descriptor ABI version is v1");
        ctx.check(descriptor[sandbox::vm::EXEC_HEADER_ENTRY_PC] >= 0,
                  path + " entry PC is non-negative");
        ctx.check(descriptor[sandbox::vm::EXEC_HEADER_TEXT_PAGES] > 0,
                  path + " has text pages");
        ctx.check(descriptor[sandbox::vm::EXEC_HEADER_DATA_PAGES] > 0,
                  path + " has data pages");
        ctx.equal(descriptor[sandbox::vm::EXEC_HEADER_STACK_WORDS],
                  static_cast<long long>(stack_words),
                  path + " preserves the release stack class");
        ctx.equal(descriptor[sandbox::vm::EXEC_HEADER_SYSCALL_ABI_VERSION],
                  static_cast<long long>(sandbox::vm::EXEC_SYSCALL_ABI_VERSION_V1),
                  path + " descriptor syscall ABI version is v1");

        const int text_ppn =
            static_cast<int>(descriptor[sandbox::vm::EXEC_HEADER_WORDS]);
        const int text_disk_block =
            static_cast<int>(descriptor[sandbox::vm::EXEC_HEADER_WORDS + 1]);
        const int text_words =
            static_cast<int>(descriptor[sandbox::vm::EXEC_HEADER_WORDS + 2]);
        const int text_pages =
            static_cast<int>(descriptor[sandbox::vm::EXEC_HEADER_TEXT_PAGES]);
        ctx.check(text_ppn > 0, path + " descriptor records a text PPN");
        ctx.check(text_disk_block >= sandbox::os::NATIVE_VFS_REQUIRED_BLOCKS,
                  path + " text lives after the native VFS metadata area");
        ctx.check(text_words > 0, path + " descriptor records text word count");
        ctx.check(text_words <= text_pages * sandbox::os::BLOCK_WORDS,
                  path + " text word count fits in declared text pages");
        const int text_blocks =
            (text_words + sandbox::os::BLOCK_WORDS - 1) / sandbox::os::BLOCK_WORDS;
        ctx.check(text_disk_block + text_blocks <= kReleaseDiskBlocks,
                  path + " text blocks stay inside the release disk");
        ctx.check(nativeDiskBlockHasNonZeroWord(disk_image, text_disk_block, text_words),
                  path + " first text block contains executable words");
    }
}

void releaseRuntimeBootDiagnostics(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release boot and disk images");
    if (!error.empty()) ctx.fail("runtime load detail: " + error);
    sandbox::host::TosRuntimeSnapshot before = runtime.snapshot();
    ctx.equal(before.image_version, std::string(kReleaseVersion),
              "runtime snapshot reports release image version");
    ctx.equal(before.disk_path, fixture.disk_path.string(),
              "runtime snapshot reports companion disk path");
    ctx.check(before.allocated_disk_blocks > 0,
              "runtime attaches non-empty release disk");

    const auto result = runtime.runForSteps(10000);
    ctx.check(result.steps > 0, "runtime executes release boot steps");
    ctx.check(!result.trapped(), "release boot does not trap during focused smoke run");

    ctx.check(runtime.exportDiagnostics(fixture.diagnostics_path.string(), &error),
              "runtime exports release diagnostics");
    if (!error.empty()) ctx.fail("diagnostic export detail: " + error);
    ctx.check(std::filesystem::exists(fixture.diagnostics_path / "manifest.json"),
              "diagnostics include manifest.json");
    ctx.check(std::filesystem::exists(fixture.diagnostics_path / "manifest.txt"),
              "diagnostics include manifest.txt");
    ctx.check(std::filesystem::exists(fixture.diagnostics_path / "vm_state.txt"),
              "diagnostics include VM state");

    const std::string manifest_json =
        tests_next::readText(fixture.diagnostics_path / "manifest.json");
    ctx.contains(manifest_json, kReleaseVersion,
                 "diagnostic manifest includes release version");
    ctx.contains(manifest_json, "\"name\": \"desktop\"",
                 "diagnostic manifest includes desktop app entry");
    ctx.contains(manifest_json, "\"path\": \"/bin/calculator\"",
                 "diagnostic manifest includes calculator path");

    ctx.check(runtime.reset(&error), "runtime resets against the same release disk");
    if (!error.empty()) ctx.fail("runtime reset detail: " + error);
    const auto reset_snapshot = runtime.snapshot();
    ctx.equal(reset_snapshot.image_version, std::string(kReleaseVersion),
              "runtime reset preserves release image metadata");
    ctx.equal(reset_snapshot.disk_path, fixture.disk_path.string(),
              "runtime reset keeps the mutable companion disk");
}

void releaseRuntimeInteractiveFramebuffer(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for interactive smoke");
    if (!error.empty()) ctx.fail("runtime load detail: " + error);

    const auto boot_result = runtime.runForSteps(10000);
    ctx.check(boot_result.steps > 0, "interactive smoke executes boot steps");
    ctx.check(!boot_result.trapped(), "interactive smoke boot does not trap");

    const sandbox::host::TosFramebufferSnapshot framebuffer = runtime.readFramebuffer();
    ctx.check(framebuffer.mode == sandbox::host::TosFramebufferMode::Text80x25,
              "release boot exposes text framebuffer mode");
    ctx.equal(framebuffer.width, 80, "release boot text framebuffer width");
    ctx.equal(framebuffer.height, 25, "release boot text framebuffer height");
    ctx.check(framebuffer.glyphs.size() >= 80u * 25u,
              "release boot text framebuffer has visible cells");
    ctx.contains(framebufferRow(framebuffer, 0), "OS 3",
                 "release boot framebuffer announces OS identity");
    ctx.check(visibleGlyphCount(framebuffer) >= 10,
              "release boot framebuffer contains visible text");

    const sandbox::host::TosFramebufferMemorySnapshot raw =
        runtime.readFramebufferMemory();
    ctx.check(raw.changed && raw.mode == sandbox::host::TosFramebufferMode::Text80x25,
              "raw framebuffer read captures initial release boot frame");
    ctx.equal(raw.words.size(), static_cast<std::size_t>(80 * 25),
              "raw framebuffer read covers the visible text cells");

    const sandbox::host::TosRuntimeSnapshot before_input = runtime.snapshot();
    runtime.pushTextInput("help\n");
    runtime.updateMouseState(12, 9, 1);
    const auto input_result = runtime.runForSteps(20000);
    ctx.check(input_result.steps > 0, "runtime advances after host input is queued");
    ctx.check(!input_result.trapped(), "queued keyboard and mouse input do not trap release runtime");
    const sandbox::host::TosRuntimeSnapshot after_input = runtime.snapshot();
    ctx.check(after_input.cycles > before_input.cycles,
              "runtime cycle count advances after host input");
    ctx.equal(after_input.image_version, std::string(kReleaseVersion),
              "interactive smoke preserves release image version");

    const sandbox::host::TosFramebufferMemorySnapshot after_raw =
        runtime.readFramebufferMemory(raw.revision);
    if (after_raw.changed) {
        ctx.check(!after_raw.words.empty(),
                  "changed release framebuffer includes updated words");
    } else {
        ctx.check(after_raw.words.empty(),
                  "unchanged release framebuffer suppresses duplicate raw words");
    }

    const std::filesystem::path interactive_diagnostics =
        fixture.diagnostics_path / "interactive";
    ctx.check(runtime.exportDiagnostics(interactive_diagnostics.string(), &error),
              "interactive smoke exports release diagnostics");
    if (!error.empty()) ctx.fail("interactive diagnostic export detail: " + error);
    ctx.check(std::filesystem::exists(interactive_diagnostics / "framebuffer_snapshot.txt"),
              "interactive diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(interactive_diagnostics / "process_table.json"),
              "interactive diagnostics include process table");
    ctx.check(std::filesystem::exists(interactive_diagnostics / "crash_report.txt"),
              "interactive diagnostics include crash report shell");
}

void releaseLoginDesktopAppLaunch(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for login flow");
    if (!error.empty()) ctx.fail("runtime load detail: " + error);

    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('1');
    if (!runUntilFramebufferContains(ctx, runtime, "CALCULATOR",
                                     5500000, 500000,
                                     "release calculator hotkey launches app")) {
        return;
    }
    std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "[7][8][9][/]",
                 "release calculator app draws keypad after launch");
    const sandbox::host::TosRuntimeSnapshot after_launch = runtime.snapshot();
    ctx.check(after_launch.cycles > before_launch.cycles,
              "runtime advances while launching calculator");
    ctx.check(!after_launch.disk_path.empty(),
              "login/app flow remains attached to mutable release disk");

    const std::filesystem::path flow_diagnostics =
        fixture.diagnostics_path / "login_desktop_app";
    ctx.check(runtime.exportDiagnostics(flow_diagnostics.string(), &error),
              "login/app flow exports release diagnostics");
    if (!error.empty()) ctx.fail("login/app diagnostic export detail: " + error);
    ctx.check(std::filesystem::exists(flow_diagnostics / "framebuffer_snapshot.txt"),
              "login/app diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(flow_diagnostics / "process_table.json"),
              "login/app diagnostics include process table");
}

void releaseTaskManagerLaunchScreen(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int task_inode = nativeLookup(root, "/bin/task_manager");
    ctx.check(task_inode > 0, "release root resolves /bin/task_manager inode");
    if (task_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for task manager launch");
    if (!error.empty()) ctx.fail("task manager launch runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path task_diagnostics =
        fixture.diagnostics_path / "task_manager_launch";
    ctx.check(runtime.exportDiagnostics(task_diagnostics.string(), &error),
              "task manager launch exports baseline diagnostics");
    if (!error.empty()) ctx.fail("task manager launch baseline diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(task_diagnostics / "app_registry.json"),
              "baseline diagnostics expose app registry rows");
    const std::string baseline_registry =
        tests_next::readText(task_diagnostics / "app_registry.json");
    const long long baseline_task =
        appLaunchCountForInode(baseline_registry, task_inode);
    ctx.check(baseline_task >= 0,
              "app registry includes task manager descriptor row");
    if (baseline_task < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('2');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, task_diagnostics, task_inode,
            baseline_task + 1, 6500000, 500000,
            "release desktop hotkey launches Task Manager app registry row")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "SYSTEM TASKS",
                                     3500000, 250000,
                                     "release task manager paints its dashboard")) {
        return;
    }
    const sandbox::host::TosRuntimeSnapshot after_launch = runtime.snapshot();
    ctx.check(after_launch.cycles > before_launch.cycles,
              "task manager launch advances runtime cycles");
    const std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "PID",
                 "task manager dashboard shows process id column");
    ctx.contains(frame, "STATE",
                 "task manager dashboard shows process state column");
    ctx.contains(frame, "PRI",
                 "task manager dashboard shows process priority column");
    ctx.contains(frame, "QUO",
                 "task manager dashboard shows process quota column");

    ctx.check(runtime.exportDiagnostics(task_diagnostics.string(), &error),
              "task manager launch exports final diagnostics");
    if (!error.empty()) ctx.fail("task manager launch final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(task_diagnostics / "framebuffer_snapshot.txt"),
              "task manager diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(task_diagnostics / "process_table.json"),
              "task manager diagnostics include process table");
}

void releasePaintLaunchGraphicsCanvas(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int paint_inode = nativeLookup(root, "/bin/paint");
    ctx.check(paint_inode > 0, "release root resolves /bin/paint inode");
    if (paint_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for paint launch");
    if (!error.empty()) ctx.fail("paint launch runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path paint_diagnostics =
        fixture.diagnostics_path / "paint_launch";
    ctx.check(runtime.exportDiagnostics(paint_diagnostics.string(), &error),
              "paint launch exports baseline diagnostics");
    if (!error.empty()) ctx.fail("paint launch baseline diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(paint_diagnostics / "app_registry.json"),
              "baseline diagnostics expose app registry rows");
    const std::string baseline_registry =
        tests_next::readText(paint_diagnostics / "app_registry.json");
    const long long baseline_paint =
        appLaunchCountForInode(baseline_registry, paint_inode);
    ctx.check(baseline_paint >= 0,
              "app registry includes paint descriptor row");
    if (baseline_paint < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('3');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, paint_diagnostics, paint_inode,
            baseline_paint + 1, 6500000, 500000,
            "release desktop hotkey launches Paint app registry row")) {
        return;
    }
    if (!runUntilPaintGraphicsReady(ctx, runtime, 3500000, 250000,
                                    "release paint reaches graphics canvas mode")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_launch = runtime.snapshot();
    ctx.check(after_launch.cycles > before_launch.cycles,
              "paint launch advances runtime cycles");
    ctx.equal(after_launch.gpu_mode, 1LL,
              "paint launch switches TosRuntime into graphics mode");

    const sandbox::host::TosFramebufferSnapshot framebuffer =
        runtime.readFramebuffer();
    ctx.check(framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
              "paint launch exposes a graphics framebuffer");
    ctx.equal(framebuffer.width, 80, "paint graphics framebuffer width");
    ctx.equal(framebuffer.height, 60, "paint graphics framebuffer height");
    ctx.equal(framebuffer.sprite_attr, 43LL + 14LL * 256LL,
              "paint publishes a yellow plus cursor sprite");

    runtime.updateMouseState(12, 12, 1);
    constexpr std::size_t kPaintProbePixel = 12u * 80u + 12u;
    if (!runUntilGraphicsWordEquals(ctx, runtime, kPaintProbePixel, 14,
                                    2500000, 250000,
                                    "release paint mouse input draws on the canvas")) {
        return;
    }
    runtime.updateMouseState(12, 12, 0);

    const sandbox::host::TosFramebufferMemorySnapshot raw =
        runtime.readFramebufferMemory();
    ctx.check(raw.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
              "paint raw framebuffer remains in graphics mode after drawing");
    ctx.check(kPaintProbePixel < raw.words.size() &&
                  raw.words[kPaintProbePixel] == 14,
              "paint raw framebuffer stores the active color at the probed canvas pixel");

    ctx.check(runtime.exportDiagnostics(paint_diagnostics.string(), &error),
              "paint launch exports final diagnostics");
    if (!error.empty()) ctx.fail("paint launch final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(paint_diagnostics / "framebuffer_snapshot.txt"),
              "paint diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(paint_diagnostics / "process_table.json"),
              "paint diagnostics include process table");
    const std::string vm_state =
        tests_next::readText(paint_diagnostics / "vm_state.txt");
    ctx.contains(vm_state, "gpu_mode=1",
                 "paint diagnostics record graphics mode");
    const std::string process_table =
        tests_next::readText(paint_diagnostics / "process_table.json");
    ctx.contains(process_table, "\"pid\": 103",
                 "diagnostics expose spawned paint pid 103");
    ctx.contains(process_table, "\"parent_pid\": 1",
                 "paint launch remains parented by the desktop");
}

void releasePaintInputClearCycle(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int paint_inode = nativeLookup(root, "/bin/paint");
    ctx.check(paint_inode > 0, "release root resolves /bin/paint inode");
    if (paint_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for paint input cycle");
    if (!error.empty()) ctx.fail("paint input cycle runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path paint_diagnostics =
        fixture.diagnostics_path / "paint_input_clear_cycle";
    ctx.check(runtime.exportDiagnostics(paint_diagnostics.string(), &error),
              "paint input cycle exports baseline diagnostics");
    if (!error.empty()) ctx.fail("paint input cycle baseline diagnostic detail: " + error);
    const std::string baseline_registry =
        tests_next::readText(paint_diagnostics / "app_registry.json");
    const long long baseline_paint =
        appLaunchCountForInode(baseline_registry, paint_inode);
    ctx.check(baseline_paint >= 0,
              "app registry includes paint descriptor row for input cycle");
    if (baseline_paint < 0) return;

    runtime.pushKeyboardInput('3');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, paint_diagnostics, paint_inode,
            baseline_paint + 1, 6500000, 500000,
            "release desktop hotkey launches Paint before input cycle")) {
        return;
    }
    if (!runUntilPaintGraphicsReady(ctx, runtime, 3500000, 250000,
                                    "release paint input cycle reaches graphics mode")) {
        return;
    }

    runtime.pushKeyboardInput('2');
    if (!runUntilSpriteAttrEquals(ctx, runtime, 43LL + 10LL * 256LL,
                                  3500000, 250000,
                                  "release paint keyboard color hotkey selects green")) {
        return;
    }

    constexpr std::size_t kPaintCyclePixel = 16u * 80u + 16u;
    runtime.updateMouseState(16, 16, 1);
    if (!runUntilGraphicsWordEquals(ctx, runtime, kPaintCyclePixel, 10,
                                    3000000, 250000,
                                    "release paint draws with keyboard-selected green")) {
        return;
    }

    runtime.updateMouseState(16, 16, 0);
    runtime.pushKeyboardInput('c');
    if (!runUntilGraphicsWordEquals(ctx, runtime, kPaintCyclePixel, 0,
                                    3500000, 250000,
                                    "release paint clear hotkey erases the canvas pixel")) {
        return;
    }

    const sandbox::host::TosFramebufferSnapshot framebuffer =
        runtime.readFramebuffer();
    ctx.check(framebuffer.mode == sandbox::host::TosFramebufferMode::Graphics80x60,
              "paint input cycle remains in graphics framebuffer mode");
    ctx.equal(framebuffer.sprite_attr, 43LL + 10LL * 256LL,
              "paint input cycle preserves selected green cursor sprite");

    ctx.check(runtime.exportDiagnostics(paint_diagnostics.string(), &error),
              "paint input cycle exports final diagnostics");
    if (!error.empty()) ctx.fail("paint input cycle final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(paint_diagnostics / "framebuffer_snapshot.txt"),
              "paint input cycle diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(paint_diagnostics / "process_table.json"),
              "paint input cycle diagnostics include process table");
    const std::string vm_state =
        tests_next::readText(paint_diagnostics / "vm_state.txt");
    ctx.contains(vm_state, "gpu_mode=1",
                 "paint input cycle diagnostics record graphics mode");
    const std::string process_table =
        tests_next::readText(paint_diagnostics / "process_table.json");
    ctx.contains(process_table, "\"pid\": 103",
                 "paint input cycle diagnostics expose spawned paint pid 103");
    ctx.contains(process_table, "\"parent_pid\": 1",
                 "paint input cycle remains parented by the desktop");
}

void releasePaintExitDesktopRecovery(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int paint_inode = nativeLookup(root, "/bin/paint");
    ctx.check(paint_inode > 0, "release root resolves /bin/paint inode");
    const int files_inode = nativeLookup(root, "/bin/file_manager");
    ctx.check(files_inode > 0, "release root resolves /bin/file_manager inode");
    if (paint_inode <= 0 || files_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for paint exit recovery");
    if (!error.empty()) ctx.fail("paint exit recovery runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path exit_diagnostics =
        fixture.diagnostics_path / "paint_exit_desktop_recovery";
    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "paint exit recovery exports baseline diagnostics");
    if (!error.empty()) ctx.fail("paint exit recovery baseline diagnostic detail: " + error);
    const std::string baseline_registry =
        tests_next::readText(exit_diagnostics / "app_registry.json");
    const long long baseline_paint =
        appLaunchCountForInode(baseline_registry, paint_inode);
    const long long baseline_files =
        appLaunchCountForInode(baseline_registry, files_inode);
    ctx.check(baseline_paint >= 0,
              "app registry includes paint descriptor row for exit recovery");
    ctx.check(baseline_files >= 0,
              "app registry includes file manager descriptor row for exit recovery");
    if (baseline_paint < 0 || baseline_files < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('3');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, paint_inode,
            baseline_paint + 1, 6500000, 500000,
            "release desktop hotkey launches Paint before exit recovery")) {
        return;
    }
    if (!runUntilPaintGraphicsReady(ctx, runtime, 3500000, 250000,
                                    "release paint exit recovery reaches graphics mode")) {
        return;
    }
    ctx.equal(runtime.snapshot().gpu_mode, 1LL,
              "paint exit recovery starts from graphics mode");

    runtime.pushKeyboardInput('x');
    if (!runUntilProcessFieldEquals(ctx, runtime, exit_diagnostics, 103,
                                    "state", 7, 5000000, 250000,
                                    "release paint exit leaves pid 103 as a zombie")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "OS 3 - DESKTOP",
                                     3500000, 250000,
                                     "desktop redraws after Paint exits")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "PAINT",
                                     3500000, 250000,
                                     "desktop launcher labels redraw after Paint exits")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_exit = runtime.snapshot();
    ctx.check(after_exit.cycles > before_launch.cycles,
              "paint exit recovery advances runtime cycles");
    ctx.equal(after_exit.gpu_mode, 0LL,
              "desktop recovery restores text framebuffer mode after Paint exit");
    const std::string desktop_frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(desktop_frame, "OS 3 - DESKTOP",
                 "desktop recovery frame contains the launcher title");
    ctx.contains(desktop_frame, "PAINT",
                 "desktop recovery frame redraws launcher app labels");

    runtime.pushKeyboardInput('4');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, files_inode,
            baseline_files + 1, 6500000, 500000,
            "desktop accepts File Manager hotkey after Paint exit")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "FILE MANAGER",
                                     3500000, 250000,
                                     "File Manager paints after Paint exit recovery")) {
        return;
    }

    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "paint exit recovery exports final diagnostics");
    if (!error.empty()) ctx.fail("paint exit recovery final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(exit_diagnostics / "framebuffer_snapshot.txt"),
              "paint exit recovery diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(exit_diagnostics / "process_table.json"),
              "paint exit recovery diagnostics include process table");
    const std::string vm_state =
        tests_next::readText(exit_diagnostics / "vm_state.txt");
    ctx.contains(vm_state, "gpu_mode=0",
                 "paint exit recovery diagnostics record text mode after File Manager launch");
    const std::string process_table =
        tests_next::readText(exit_diagnostics / "process_table.json");
    const std::string paint_process = processObjectForPid(process_table, 103);
    ctx.check(!paint_process.empty(),
              "paint exit recovery diagnostics keep exited paint pid inspectable");
    if (!paint_process.empty()) {
        ctx.equal(jsonIntValue(paint_process, "state", -1), 7,
                  "paint exit recovery leaves paint pid as zombie");
        ctx.equal(jsonIntValue(paint_process, "parent_pid", -1), 1,
                  "paint exit recovery keeps paint parented by desktop");
    }
    const std::string files_process = processObjectForPid(process_table, 104);
    ctx.check(!files_process.empty(),
              "paint exit recovery diagnostics expose launched File Manager pid 104");
    if (!files_process.empty()) {
        ctx.equal(jsonIntValue(files_process, "parent_pid", -1), 1,
                  "File Manager after Paint exit remains parented by the desktop");
    }
}

void releaseFileManagerLaunchScreen(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int files_inode = nativeLookup(root, "/bin/file_manager");
    ctx.check(files_inode > 0, "release root resolves /bin/file_manager inode");
    if (files_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for file manager launch");
    if (!error.empty()) ctx.fail("file manager launch runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path files_diagnostics =
        fixture.diagnostics_path / "file_manager_launch";
    ctx.check(runtime.exportDiagnostics(files_diagnostics.string(), &error),
              "file manager launch exports baseline diagnostics");
    if (!error.empty()) ctx.fail("file manager launch baseline diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(files_diagnostics / "app_registry.json"),
              "baseline diagnostics expose app registry rows");
    const std::string baseline_registry =
        tests_next::readText(files_diagnostics / "app_registry.json");
    const long long baseline_files =
        appLaunchCountForInode(baseline_registry, files_inode);
    ctx.check(baseline_files >= 0,
              "app registry includes file manager descriptor row");
    if (baseline_files < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('4');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, files_diagnostics, files_inode,
            baseline_files + 1, 6500000, 500000,
            "release desktop hotkey launches File Manager app registry row")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "FILE MANAGER",
                                     3500000, 250000,
                                     "release file manager paints its browser")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "bin",
                                     5000000, 250000,
                                     "release file manager lists the root bin directory")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "apps",
                                     5000000, 250000,
                                     "release file manager lists the root apps directory")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_launch = runtime.snapshot();
    ctx.check(after_launch.cycles > before_launch.cycles,
              "file manager launch advances runtime cycles");
    const std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "CLOSE",
                 "file manager browser paints its close footer");
    ctx.contains(frame, "bin",
                 "file manager browser lists the root bin directory");
    ctx.contains(frame, "apps",
                 "file manager browser lists the root apps directory");

    ctx.check(runtime.exportDiagnostics(files_diagnostics.string(), &error),
              "file manager launch exports final diagnostics");
    if (!error.empty()) ctx.fail("file manager launch final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(files_diagnostics / "framebuffer_snapshot.txt"),
              "file manager diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(files_diagnostics / "process_table.json"),
              "file manager diagnostics include process table");
    const std::string process_table =
        tests_next::readText(files_diagnostics / "process_table.json");
    ctx.contains(process_table, "\"pid\": 104",
                 "diagnostics expose spawned file manager pid 104");
    ctx.contains(process_table, "\"parent_pid\": 1",
                 "file manager launch remains parented by the desktop");
}

void releaseFileManagerExitDesktopRecovery(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int files_inode = nativeLookup(root, "/bin/file_manager");
    ctx.check(files_inode > 0, "release root resolves /bin/file_manager inode");
    const int settings_inode = nativeLookup(root, "/bin/settings");
    ctx.check(settings_inode > 0, "release root resolves /bin/settings inode");
    if (files_inode <= 0 || settings_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error),
              "runtime loads release image for file manager exit recovery");
    if (!error.empty()) ctx.fail("file manager exit recovery runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path exit_diagnostics =
        fixture.diagnostics_path / "file_manager_exit_desktop_recovery";
    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "file manager exit recovery exports baseline diagnostics");
    if (!error.empty()) {
        ctx.fail("file manager exit recovery baseline diagnostic detail: " + error);
    }
    const std::string baseline_registry =
        tests_next::readText(exit_diagnostics / "app_registry.json");
    const long long baseline_files =
        appLaunchCountForInode(baseline_registry, files_inode);
    const long long baseline_settings =
        appLaunchCountForInode(baseline_registry, settings_inode);
    ctx.check(baseline_files >= 0,
              "app registry includes file manager descriptor row for exit recovery");
    ctx.check(baseline_settings >= 0,
              "app registry includes settings descriptor row for exit recovery");
    if (baseline_files < 0 || baseline_settings < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('4');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, files_inode,
            baseline_files + 1, 6500000, 500000,
            "release desktop hotkey launches File Manager before exit recovery")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "FILE MANAGER",
                                     3500000, 250000,
                                     "release file manager paints before exit recovery")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "bin",
                                     5000000, 250000,
                                     "release file manager lists bin before exit recovery")) {
        return;
    }

    runtime.pushKeyboardInput('x');
    if (!runUntilProcessFieldEquals(ctx, runtime, exit_diagnostics, 104,
                                    "state", 7, 5000000, 250000,
                                    "release file manager exit leaves pid 104 as a zombie")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "OS 3 - DESKTOP",
                                     3500000, 250000,
                                     "desktop redraws after File Manager exits")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "FILES",
                                     3500000, 250000,
                                     "desktop launcher labels redraw after File Manager exits")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_exit = runtime.snapshot();
    ctx.check(after_exit.cycles > before_launch.cycles,
              "file manager exit recovery advances runtime cycles");
    ctx.equal(after_exit.gpu_mode, 0LL,
              "file manager exit recovery keeps the desktop in text framebuffer mode");
    const std::string desktop_frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(desktop_frame, "OS 3 - DESKTOP",
                 "file manager exit recovery frame contains the launcher title");
    ctx.contains(desktop_frame, "FILES",
                 "file manager exit recovery frame redraws launcher app labels");

    runtime.pushKeyboardInput('5');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, settings_inode,
            baseline_settings + 1, 6500000, 500000,
            "desktop accepts Settings hotkey after File Manager exit")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "BRIGHTNESS",
                                     3500000, 250000,
                                     "Settings paints after File Manager exit recovery")) {
        return;
    }

    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "file manager exit recovery exports final diagnostics");
    if (!error.empty()) ctx.fail("file manager exit recovery final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(exit_diagnostics / "framebuffer_snapshot.txt"),
              "file manager exit recovery diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(exit_diagnostics / "process_table.json"),
              "file manager exit recovery diagnostics include process table");
    const std::string vm_state =
        tests_next::readText(exit_diagnostics / "vm_state.txt");
    ctx.contains(vm_state, "gpu_mode=0",
                 "file manager exit recovery diagnostics record text mode");
    const std::string process_table =
        tests_next::readText(exit_diagnostics / "process_table.json");
    const std::string files_process = processObjectForPid(process_table, 104);
    ctx.check(!files_process.empty(),
              "file manager exit recovery diagnostics keep exited pid 104 inspectable");
    if (!files_process.empty()) {
        ctx.equal(jsonIntValue(files_process, "state", -1), 7,
                  "file manager exit recovery leaves pid 104 as zombie");
        ctx.equal(jsonIntValue(files_process, "parent_pid", -1), 1,
                  "file manager exit recovery keeps file manager parented by desktop");
    }
    const std::string settings_process = processObjectForPid(process_table, 105);
    ctx.check(!settings_process.empty(),
              "file manager exit recovery diagnostics expose launched Settings pid 105");
    if (!settings_process.empty()) {
        ctx.equal(jsonIntValue(settings_process, "parent_pid", -1), 1,
                  "Settings after File Manager exit remains parented by the desktop");
    }
}

void releaseSettingsLaunchScreen(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for settings launch");
    if (!error.empty()) ctx.fail("settings launch runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('5');
    if (!runUntilFramebufferContains(ctx, runtime, "BRIGHTNESS",
                                     3500000, 250000,
                                     "release settings hotkey renders display tab")) {
        return;
    }

    std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "OS 3 - SETTINGS", "settings app paints its title");
    ctx.contains(frame, "[DISPLAY]", "settings app paints tab bar");
    ctx.contains(frame, "[ESC] CLOSE", "settings app paints close hint");

    const sandbox::host::TosRuntimeSnapshot after_launch = runtime.snapshot();
    ctx.check(after_launch.cycles > before_launch.cycles,
              "settings launch advances runtime cycles");
    ctx.equal(static_cast<long long>(after_launch.block_device_stats.writes),
              static_cast<long long>(before_launch.block_device_stats.writes),
              "settings launch does not write the release disk before an explicit save");

    const std::filesystem::path settings_diagnostics =
        fixture.diagnostics_path / "settings_launch";
    ctx.check(runtime.exportDiagnostics(settings_diagnostics.string(), &error),
              "settings launch exports release diagnostics");
    if (!error.empty()) ctx.fail("settings launch diagnostic export detail: " + error);
    ctx.check(std::filesystem::exists(settings_diagnostics / "framebuffer_snapshot.txt"),
              "settings launch diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(settings_diagnostics / "process_table.json"),
              "settings launch diagnostics include process table");

    const std::string process_table =
        tests_next::readText(settings_diagnostics / "process_table.json");
    ctx.contains(process_table, "\"pid\": 105",
                 "diagnostics expose spawned settings pid 105");
    ctx.contains(process_table, "\"parent_pid\": 1",
                 "settings launch remains parented by the desktop");
}

void releaseSettingsExitDesktopRecovery(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int settings_inode = nativeLookup(root, "/bin/settings");
    ctx.check(settings_inode > 0, "release root resolves /bin/settings inode");
    const int task_inode = nativeLookup(root, "/bin/task_manager");
    ctx.check(task_inode > 0, "release root resolves /bin/task_manager inode");
    if (settings_inode <= 0 || task_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error),
              "runtime loads release image for settings exit recovery");
    if (!error.empty()) ctx.fail("settings exit recovery runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path exit_diagnostics =
        fixture.diagnostics_path / "settings_exit_desktop_recovery";
    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "settings exit recovery exports baseline diagnostics");
    if (!error.empty()) ctx.fail("settings exit recovery baseline diagnostic detail: " + error);
    const std::string baseline_registry =
        tests_next::readText(exit_diagnostics / "app_registry.json");
    const long long baseline_settings =
        appLaunchCountForInode(baseline_registry, settings_inode);
    const long long baseline_task =
        appLaunchCountForInode(baseline_registry, task_inode);
    ctx.check(baseline_settings >= 0,
              "app registry includes settings descriptor row for exit recovery");
    ctx.check(baseline_task >= 0,
              "app registry includes task manager descriptor row for exit recovery");
    if (baseline_settings < 0 || baseline_task < 0) return;

    const sandbox::host::TosRuntimeSnapshot before_launch = runtime.snapshot();
    runtime.pushKeyboardInput('5');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, settings_inode,
            baseline_settings + 1, 6500000, 500000,
            "release desktop hotkey launches Settings before exit recovery")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "BRIGHTNESS",
                                     3500000, 250000,
                                     "release settings paints before exit recovery")) {
        return;
    }

    runtime.pushKeyboardInput('x');
    if (!runUntilProcessFieldEquals(ctx, runtime, exit_diagnostics, 105,
                                    "state", 7, 5000000, 250000,
                                    "release settings exit leaves pid 105 as a zombie")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "OS 3 - DESKTOP",
                                     3500000, 250000,
                                     "desktop redraws after Settings exits")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "SETT",
                                     3500000, 250000,
                                     "desktop launcher labels redraw after Settings exits")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_exit = runtime.snapshot();
    ctx.check(after_exit.cycles > before_launch.cycles,
              "settings exit recovery advances runtime cycles");
    ctx.equal(after_exit.gpu_mode, 0LL,
              "settings exit recovery keeps the desktop in text framebuffer mode");
    const std::string desktop_frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(desktop_frame, "OS 3 - DESKTOP",
                 "settings exit recovery frame contains the launcher title");
    ctx.contains(desktop_frame, "SETT",
                 "settings exit recovery frame redraws launcher app labels");

    runtime.pushKeyboardInput('2');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, exit_diagnostics, task_inode,
            baseline_task + 1, 6500000, 500000,
            "desktop accepts Task Manager hotkey after Settings exit")) {
        return;
    }
    if (!runUntilFramebufferContains(ctx, runtime, "SYSTEM TASKS",
                                     3500000, 250000,
                                     "Task Manager paints after Settings exit recovery")) {
        return;
    }

    ctx.check(runtime.exportDiagnostics(exit_diagnostics.string(), &error),
              "settings exit recovery exports final diagnostics");
    if (!error.empty()) ctx.fail("settings exit recovery final diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(exit_diagnostics / "framebuffer_snapshot.txt"),
              "settings exit recovery diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(exit_diagnostics / "process_table.json"),
              "settings exit recovery diagnostics include process table");
    const std::string vm_state =
        tests_next::readText(exit_diagnostics / "vm_state.txt");
    ctx.contains(vm_state, "gpu_mode=0",
                 "settings exit recovery diagnostics record text mode");
    const std::string process_table =
        tests_next::readText(exit_diagnostics / "process_table.json");
    const std::string settings_process = processObjectForPid(process_table, 105);
    ctx.check(!settings_process.empty(),
              "settings exit recovery diagnostics keep exited pid 105 inspectable");
    if (!settings_process.empty()) {
        ctx.equal(jsonIntValue(settings_process, "state", -1), 7,
                  "settings exit recovery leaves pid 105 as zombie");
        ctx.equal(jsonIntValue(settings_process, "parent_pid", -1), 1,
                  "settings exit recovery keeps Settings parented by desktop");
    }
    const std::string task_process = processObjectForPid(process_table, 102);
    ctx.check(!task_process.empty(),
              "settings exit recovery diagnostics expose launched Task Manager pid 102");
    if (!task_process.empty()) {
        ctx.equal(jsonIntValue(task_process, "parent_pid", -1), 1,
                  "Task Manager after Settings exit remains parented by the desktop");
    }
}

void releaseSettingsPersistenceReadback(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for settings persistence");
    if (!error.empty()) ctx.fail("settings persistence runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    runtime.pushKeyboardInput('5');
    if (!runUntilFramebufferContains(ctx, runtime, "BRIGHTNESS",
                                     3500000, 250000,
                                     "settings persistence reaches display tab")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot before_save = runtime.snapshot();
    runtime.pushKeyboardInput('+');
    if (!runUntilFramebufferContains(ctx, runtime, "[===========-]",
                                     10000000, 500000,
                                     "settings persistence raises brightness")) {
        return;
    }
    std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "SAVED OK", "settings persistence reports a successful save");

    const sandbox::host::TosRuntimeSnapshot after_save = runtime.snapshot();
    ctx.check(after_save.block_device_stats.writes >
                  before_save.block_device_stats.writes,
              "settings save writes the mutable release disk");
    ctx.check(after_save.block_device_stats.flushes >
                  before_save.block_device_stats.flushes ||
                  after_save.block_device_stats.dirty_flushes >
                      before_save.block_device_stats.dirty_flushes,
              "settings save flushes dirty release disk data");
    ctx.equal(static_cast<long long>(after_save.pending_disk_writes), 0,
              "settings fsync drains pending release disk writes");

    std::vector<SparseDiskRecord> saved_records;
    std::string disk_error;
    ctx.check(readSparseDiskRecords(fixture.disk_path, saved_records, disk_error),
              "settings save leaves readable sparse disk records");
    if (!disk_error.empty()) ctx.fail("settings persistence disk read detail: " + disk_error);
    const std::vector<long long> saved_words = flattenDiskRecords(saved_records);
    ctx.check(containsSequence(saved_words, std::vector<long long>{7, 1, 9, 0}),
              "release disk stores updated Settings config words");

    ctx.check(runtime.reset(&error), "runtime resets after settings save");
    if (!error.empty()) ctx.fail("settings persistence reset detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    runtime.pushKeyboardInput('5');
    if (!runUntilFramebufferContains(ctx, runtime, "[===========-]",
                                     10000000, 500000,
                                     "settings persistence reads brightness after reset")) {
        const sandbox::host::TosFramebufferSnapshot readback_frame =
            runtime.readFramebuffer();
        ctx.fail("settings persistence readback row 12='" +
                 framebufferRow(readback_frame, 12) + "'");
        return;
    }
    frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "OS 3 - SETTINGS",
                 "settings persistence relaunch paints Settings after reset");
    ctx.contains(frame, "BRIGHTNESS",
                 "settings persistence relaunch paints the display tab");

    const std::filesystem::path settings_diagnostics =
        fixture.diagnostics_path / "settings_persistence";
    ctx.check(runtime.exportDiagnostics(settings_diagnostics.string(), &error),
              "settings persistence exports release diagnostics");
    if (!error.empty()) ctx.fail("settings persistence diagnostic export detail: " + error);
    ctx.check(std::filesystem::exists(settings_diagnostics / "framebuffer_snapshot.txt"),
              "settings persistence diagnostics include framebuffer snapshot");
    ctx.check(std::filesystem::exists(settings_diagnostics / "process_table.json"),
              "settings persistence diagnostics include process table");
}

void releaseTerminalLaunchResetDiagnostics(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error), "runtime loads release image for terminal reset");
    if (!error.empty()) ctx.fail("terminal reset runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path terminal_diagnostics =
        fixture.diagnostics_path / "terminal_launch_reset";
    runtime.pushKeyboardInput('6');
    if (!runUntilDiagnosticFileContains(ctx, runtime, terminal_diagnostics,
                                        "process_table.json", "\"pid\": 106",
                                        6500000, 500000,
                                        "release terminal hotkey spawns terminal process")) {
        return;
    }

    const std::string process_table =
        tests_next::readText(terminal_diagnostics / "process_table.json");
    ctx.contains(process_table, "\"pid\": 106",
                 "terminal diagnostics expose spawned terminal pid 106");
    ctx.contains(process_table, "\"parent_pid\": 1",
                 "terminal launch remains parented by the desktop");

    ctx.check(std::filesystem::exists(terminal_diagnostics / "crash_report.txt"),
              "terminal launch diagnostics include crash report shell");
    const std::string crash_report =
        tests_next::readText(terminal_diagnostics / "crash_report.txt");
    ctx.contains(crash_report, "status=", "crash report records VM status");
    ctx.contains(crash_report, "trap=", "crash report records trap word");
    ctx.contains(crash_report, "cause=", "crash report records cause");
    ctx.contains(crash_report, "current_pid=",
                 "crash report records current process id");

    const sandbox::host::TosRuntimeSnapshot before_reset = runtime.snapshot();
    ctx.check(runtime.reset(&error), "runtime resets after terminal launch diagnostics");
    if (!error.empty()) ctx.fail("terminal reset detail: " + error);
    const sandbox::host::TosRuntimeSnapshot after_reset = runtime.snapshot();
    ctx.equal(after_reset.image_version, std::string(kReleaseVersion),
              "terminal reset preserves release image version");
    ctx.equal(after_reset.disk_path, before_reset.disk_path,
              "terminal reset keeps the mutable release disk");

    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;
    const std::string frame = framebufferText(runtime.readFramebuffer());
    ctx.contains(frame, "OS 3 - DESKTOP",
                 "release returns to desktop after terminal reset");
}

void releaseTerminalShellRebootCommand(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int terminal_inode = nativeLookup(root, "/bin/terminal");
    const int shell_inode = nativeLookup(root, "/bin/shell");
    const int reboot_inode = nativeLookup(root, "/bin/reboot");
    ctx.check(terminal_inode > 0, "release root resolves /bin/terminal inode");
    ctx.check(shell_inode > 0, "release root resolves /bin/shell inode");
    ctx.check(reboot_inode > 0, "release root resolves /bin/reboot inode");
    if (terminal_inode <= 0 || shell_inode <= 0 || reboot_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error),
              "runtime loads release image for terminal command flow");
    if (!error.empty()) ctx.fail("terminal command runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path command_diagnostics =
        fixture.diagnostics_path / "terminal_shell_reboot_command";
    ctx.check(runtime.exportDiagnostics(command_diagnostics.string(), &error),
              "terminal command flow exports baseline diagnostics");
    if (!error.empty()) ctx.fail("terminal command baseline diagnostic detail: " + error);
    ctx.check(std::filesystem::exists(command_diagnostics / "app_registry.json"),
              "diagnostics expose kernel app registry launch counters");

    const std::string baseline_registry =
        tests_next::readText(command_diagnostics / "app_registry.json");
    const long long baseline_terminal =
        appLaunchCountForInode(baseline_registry, terminal_inode);
    const long long baseline_shell =
        appLaunchCountForInode(baseline_registry, shell_inode);
    const long long baseline_reboot =
        appLaunchCountForInode(baseline_registry, reboot_inode);
    ctx.check(baseline_terminal >= 0,
              "app registry includes terminal descriptor row");
    ctx.check(baseline_shell >= 0,
              "app registry includes shell descriptor row");
    ctx.check(baseline_reboot >= 0,
              "app registry includes reboot descriptor row");
    if (baseline_terminal < 0 || baseline_shell < 0 || baseline_reboot < 0) return;

    runtime.pushKeyboardInput('6');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, command_diagnostics, terminal_inode,
            baseline_terminal + 1, 6500000, 500000,
            "release desktop hotkey launches Terminal app registry row")) {
        return;
    }
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, command_diagnostics, shell_inode,
            baseline_shell + 1, 6500000, 500000,
            "release Terminal execs shell app registry row")) {
        return;
    }
    if (!runUntilDiagnosticFileContains(ctx, runtime, command_diagnostics,
                                        "guest.log", "TRIT SHELL",
                                        50000000, 1000000,
                                        "release shell reaches interactive prompt")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot before_reboot = runtime.snapshot();
    ctx.check(before_reboot.boot_generation == 1 &&
                  before_reboot.guest_reboot_count == 0,
              "terminal command starts in the original guest boot generation");

    runtime.pushTextInput("reboot");
    runtime.pushKeyboardInput(13);
    if (!runUntilGuestRebootCountAtLeast(
            ctx, runtime, before_reboot.guest_reboot_count + 1,
            10000000, 500000,
            "release shell command completes a guest-requested cold reboot")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_reboot = runtime.snapshot();
    ctx.equal(after_reboot.boot_generation, before_reboot.boot_generation + 1,
              "reboot command advances the cold boot generation exactly once");
    ctx.equal(after_reboot.guest_reboot_count,
              before_reboot.guest_reboot_count + 1,
              "reboot command increments the guest reboot counter exactly once");
    ctx.equal(after_reboot.image_version, before_reboot.image_version,
              "guest reboot reloads the same release boot image");
    ctx.equal(after_reboot.disk_path, before_reboot.disk_path,
              "guest reboot preserves the mutable release disk attachment");
    ctx.equal(after_reboot.cycles, 0LL,
              "guest reboot clears the prior VM cycle state");

    error.clear();
    ctx.check(runtime.exportDiagnostics(command_diagnostics.string(), &error),
              "terminal command flow exports post-reboot cold-state diagnostics");
    if (!error.empty()) ctx.fail("post-reboot diagnostic detail: " + error);
    const std::string cold_vm_state =
        tests_next::readText(command_diagnostics / "vm_state.txt");
    ctx.contains(cold_vm_state,
                 "boot_generation=" + std::to_string(after_reboot.boot_generation),
                 "diagnostics expose the new boot generation");
    ctx.contains(cold_vm_state,
                 "guest_reboot_count=" +
                     std::to_string(after_reboot.guest_reboot_count),
                 "diagnostics expose the completed guest reboot");
    ctx.equal(tests_next::readText(command_diagnostics / "guest.log"), std::string(),
              "guest reboot clears the previous boot's console log");
    ctx.contains(tests_next::readText(command_diagnostics / "process_table.json"),
                 "\"current_pid\": 0",
                 "guest reboot clears the previous kernel scheduler state");

    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;
    error.clear();
    ctx.check(runtime.exportDiagnostics(command_diagnostics.string(), &error),
              "terminal command flow exports second-boot desktop diagnostics");
    if (!error.empty()) ctx.fail("second-boot diagnostic detail: " + error);

    const std::string second_registry =
        tests_next::readText(command_diagnostics / "app_registry.json");
    ctx.equal(appLaunchCountForInode(second_registry, terminal_inode), baseline_terminal,
              "cold reboot resets Terminal launch state before the second login");
    ctx.equal(appLaunchCountForInode(second_registry, shell_inode), baseline_shell,
              "cold reboot resets Shell launch state before the second login");
    ctx.equal(appLaunchCountForInode(second_registry, reboot_inode), baseline_reboot,
              "cold reboot resets reboot-app launch state before the second login");

    const std::string process_table =
        tests_next::readText(command_diagnostics / "process_table.json");
    ctx.check(process_table.find("\"pid\": 106") == std::string::npos,
              "second boot does not retain the pre-reboot Terminal/Shell pid");
    ctx.contains(framebufferText(runtime.readFramebuffer()), "OS 3 - DESKTOP",
                 "guest reaches a fresh desktop after the reboot command");
}

void releaseTerminalShellCrashDiagnostics(TestContext& ctx) {
    if (!ensureFixture(ctx)) return;
    ReleaseFixture& fixture = fixtureRef();

    const std::vector<long long> disk_image =
        expandDiskRecords(fixture.disk_records, kReleaseDiskBlocks);
    const NativeVfsView root = decodeNativeVfs(disk_image);
    ctx.check(root.valid,
              "release sparse disk expands into a decodable native VFS image");
    const int terminal_inode = nativeLookup(root, "/bin/terminal");
    const int shell_inode = nativeLookup(root, "/bin/shell");
    const int crash_inode = nativeLookup(root, "/bin/crash");
    ctx.check(terminal_inode > 0, "release root resolves /bin/terminal inode");
    ctx.check(shell_inode > 0, "release root resolves /bin/shell inode");
    ctx.check(crash_inode > 0, "release root resolves /bin/crash inode");
    if (terminal_inode <= 0 || shell_inode <= 0 || crash_inode <= 0) return;

    sandbox::host::TosRuntimeConfig config;
    config.boot_image_path = fixture.boot_path.string();
    config.disk_path = fixture.disk_path.string();
    config.profile_name = "minimum";
    sandbox::host::TosRuntime runtime(config);

    std::string error;
    ctx.check(runtime.loadImage(&error),
              "runtime loads release image for terminal crash flow");
    if (!error.empty()) ctx.fail("terminal crash runtime load detail: " + error);
    if (!driveReleaseLoginToDesktop(ctx, runtime)) return;

    const std::filesystem::path crash_diagnostics =
        fixture.diagnostics_path / "terminal_shell_crash_diagnostics";
    ctx.check(runtime.exportDiagnostics(crash_diagnostics.string(), &error),
              "terminal crash flow exports baseline diagnostics");
    if (!error.empty()) ctx.fail("terminal crash baseline diagnostic detail: " + error);

    const std::string baseline_registry =
        tests_next::readText(crash_diagnostics / "app_registry.json");
    const long long baseline_terminal =
        appLaunchCountForInode(baseline_registry, terminal_inode);
    const long long baseline_shell =
        appLaunchCountForInode(baseline_registry, shell_inode);
    const long long baseline_crash =
        appLaunchCountForInode(baseline_registry, crash_inode);
    ctx.check(baseline_terminal >= 0,
              "app registry includes terminal descriptor row");
    ctx.check(baseline_shell >= 0,
              "app registry includes shell descriptor row");
    ctx.check(baseline_crash >= 0,
              "app registry includes crash descriptor row");
    if (baseline_terminal < 0 || baseline_shell < 0 || baseline_crash < 0) return;

    runtime.pushKeyboardInput('6');
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, crash_diagnostics, terminal_inode,
            baseline_terminal + 1, 6500000, 500000,
            "release desktop hotkey launches Terminal before crash")) {
        return;
    }
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, crash_diagnostics, shell_inode,
            baseline_shell + 1, 6500000, 500000,
            "release Terminal execs Shell before crash")) {
        return;
    }
    if (!runUntilDiagnosticFileContains(ctx, runtime, crash_diagnostics,
                                        "guest.log", "TRIT SHELL",
                                        50000000, 1000000,
                                        "release shell reaches prompt before crash")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot before_crash = runtime.snapshot();
    ctx.check(before_crash.boot_generation == 1 &&
                  before_crash.guest_reboot_count == 0,
              "terminal crash flow starts in the original guest boot generation");

    runtime.pushTextInput("crash");
    runtime.pushKeyboardInput(13);
    if (!runUntilAppLaunchCountAtLeast(
            ctx, runtime, crash_diagnostics, crash_inode,
            baseline_crash + 1, 10000000, 500000,
            "release shell command launches the intentional crash app")) {
        return;
    }
    if (!runUntilProcessFieldEquals(ctx, runtime, crash_diagnostics, 106,
                                    "state", 9, 10000000, 500000,
                                    "kernel records the faulting command as crashed")) {
        return;
    }

    const sandbox::host::TosRuntimeSnapshot after_crash = runtime.snapshot();
    ctx.equal(after_crash.boot_generation, before_crash.boot_generation,
              "intentional guest crash does not cold-reboot the release image");
    ctx.equal(after_crash.guest_reboot_count, before_crash.guest_reboot_count,
              "intentional guest crash does not increment reboot diagnostics");
    ctx.check(after_crash.status == sandbox::vm::VMStatus::RUNNING,
              "VM remains running after the kernel handles a user-process crash");

    error.clear();
    ctx.check(runtime.exportDiagnostics(crash_diagnostics.string(), &error),
              "terminal crash flow exports post-crash diagnostics");
    if (!error.empty()) ctx.fail("post-crash diagnostic detail: " + error);
    const std::string process_table =
        tests_next::readText(crash_diagnostics / "process_table.json");
    const std::string crashed_process = processObjectForPid(process_table, 106);
    ctx.check(!crashed_process.empty(),
              "process diagnostics retain the crashed command pid for parent inspection");
    ctx.contains(crashed_process, "\"state_name\": \"crashed\"",
                 "process diagnostics name pid 106 as crashed");
    ctx.contains(crashed_process, "\"parent_pid\": 1",
                 "crashed command remains parented by the desktop");
    ctx.equal(jsonIntValue(crashed_process, "exit_status"),
              static_cast<long long>(sandbox::isa::OS_CAUSE_DIV_ZERO),
              "crashed command exit status records the routed divide-by-zero cause");

    const std::string crash_report =
        tests_next::readText(crash_diagnostics / "crash_report.txt");
    ctx.contains(crash_report, "status=RUNNING",
                 "crash report shows the VM recovered to running state");
    ctx.contains(crash_report,
                 "boot_generation=" + std::to_string(after_crash.boot_generation),
                 "crash report includes boot generation context");
    ctx.contains(crash_report,
                 "guest_reboot_count=" + std::to_string(after_crash.guest_reboot_count),
                 "crash report includes reboot counter context");
    ctx.contains(crash_report, "crashed_pid=106",
                 "crash report summarizes the crashed process pid");
    ctx.contains(crash_report, "crashed_parent_pid=1",
                 "crash report summarizes the crashed process parent");
    ctx.contains(crash_report,
                 "crashed_exit_status=" +
                     std::to_string(sandbox::isa::OS_CAUSE_DIV_ZERO),
                 "crash report summarizes the durable routed fault cause");

    const std::string post_registry =
        tests_next::readText(crash_diagnostics / "app_registry.json");
    ctx.equal(appLaunchCountForInode(post_registry, crash_inode),
              baseline_crash + 1,
              "app registry records exactly one launch of /bin/crash");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"full_system.release.artifact_manifest", "full_system.release_contract",
         releaseArtifactManifest},
        {"full_system.release.disk_root_metadata", "full_system.release_contract",
         releaseDiskRootMetadata},
        {"full_system.release.rootfs_registry_decoding", "full_system.release_contract",
         releaseRootfsRegistryDecoding},
        {"full_system.release.cli_command_descriptors", "full_system.release_contract",
         releaseCliCommandDescriptors},
        {"full_system.release.runtime_diagnostics", "full_system.release_contract",
         releaseRuntimeBootDiagnostics},
        {"full_system.release.interactive_framebuffer", "full_system.release_contract",
         releaseRuntimeInteractiveFramebuffer},
        {"full_system.release.login_desktop_app_launch", "full_system.release_contract",
         releaseLoginDesktopAppLaunch},
        {"full_system.release.task_manager_launch_screen", "full_system.release_contract",
         releaseTaskManagerLaunchScreen},
        {"full_system.release.paint_launch_graphics_canvas", "full_system.release_contract",
         releasePaintLaunchGraphicsCanvas},
        {"full_system.release.paint_input_clear_cycle", "full_system.release_contract",
         releasePaintInputClearCycle},
        {"full_system.release.paint_exit_desktop_recovery", "full_system.release_contract",
         releasePaintExitDesktopRecovery},
        {"full_system.release.file_manager_launch_screen", "full_system.release_contract",
         releaseFileManagerLaunchScreen},
        {"full_system.release.file_manager_exit_desktop_recovery", "full_system.release_contract",
         releaseFileManagerExitDesktopRecovery},
        {"full_system.release.settings_launch_screen", "full_system.release_contract",
         releaseSettingsLaunchScreen},
        {"full_system.release.settings_exit_desktop_recovery", "full_system.release_contract",
         releaseSettingsExitDesktopRecovery},
        {"full_system.release.settings_persistence_readback", "full_system.release_contract",
         releaseSettingsPersistenceReadback},
        {"full_system.release.terminal_launch_reset_diagnostics", "full_system.release_contract",
         releaseTerminalLaunchResetDiagnostics},
        {"full_system.release.terminal_shell_reboot_command", "full_system.release_contract",
         releaseTerminalShellRebootCommand},
        {"full_system.release.terminal_shell_crash_diagnostics",
         "full_system.release_contract",
         releaseTerminalShellCrashDiagnostics},
    };
    return tests_next::runCases("next_full_system_release", cases);
}
