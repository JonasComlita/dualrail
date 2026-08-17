#pragma once

// Read-only directory-backed guest VFS for converted BitNet tensors.
//
// The external benchmark does not hand the inference engine an arbitrary host
// directory.  The converter publishes a small, versioned package manifest and
// one hash-locked file per tensor.  This adapter validates that package before
// exposing any stream.  The loader can then seek and stream individual files
// without ever concatenating an untrusted path or loading the whole model into
// one host buffer.

#ifndef BITNET_GUEST_VFS_H
#define BITNET_GUEST_VFS_H

#include "external_asset_support.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sandbox::bitnet {

namespace guest_vfs_fs = std::filesystem;

class ReadOnlyGuestVfs {
public:
    explicit ReadOnlyGuestVfs(guest_vfs_fs::path root) : root_(std::move(root)) {
        init();
    }

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    const std::string& profile() const { return profile_; }
    std::size_t entryCount() const { return entries_.size(); }
    const guest_vfs_fs::path& rootPath() const { return root_; }

    bool contains(const std::string& name) const {
        return entries_.find(name) != entries_.end();
    }

    bool open(const std::string& name, std::ifstream& output) const {
        const auto it = entries_.find(name);
        if (it == entries_.end()) return false;
        const guest_vfs_fs::path path = root_ / guest_vfs_fs::path(name);
        if (!isSafeRegularFile(path, it->second.size)) return false;
        output.open(path, std::ios::binary);
        return output.is_open();
    }

    bool fileSize(const std::string& name, std::uintmax_t& size) const {
        const auto it = entries_.find(name);
        if (it == entries_.end()) return false;
        size = it->second.size;
        return true;
    }

private:
    struct Entry {
        std::uintmax_t size = 0;
        std::string sha256;
    };

    guest_vfs_fs::path root_;
    std::map<std::string, Entry> entries_;
    std::string profile_;
    std::string error_;
    bool valid_ = false;

    static bool isSafeRelative(const std::string& name) {
        if (name.empty()) return false;
        const guest_vfs_fs::path path(name);
        if (path.empty() || path.is_absolute()) return false;
        for (const auto& component : path) {
            if (component == guest_vfs_fs::path("..") ||
                component == guest_vfs_fs::path("."))
                return false;
        }
        return true;
    }

    static bool isLowerHexDigest(const std::string& value) {
        if (value.size() != 64) return false;
        return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    }

    static void skipSpace(const std::string& text, std::size_t& cursor) {
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;
    }

    static bool parseJsonString(const std::string& text, std::size_t& cursor,
                                std::string& value) {
        skipSpace(text, cursor);
        if (cursor >= text.size() || text[cursor] != '"') return false;
        ++cursor;
        value.clear();
        while (cursor < text.size()) {
            const char ch = text[cursor++];
            if (ch == '"') return true;
            if (ch == '\\' || static_cast<unsigned char>(ch) < 0x20)
                return false;
            value.push_back(ch);
        }
        return false;
    }

    static bool parseUnsigned(const std::string& text, std::size_t& cursor,
                              std::uintmax_t& value) {
        skipSpace(text, cursor);
        if (cursor >= text.size() || text[cursor] < '0' || text[cursor] > '9')
            return false;
        value = 0;
        while (cursor < text.size() && text[cursor] >= '0' &&
               text[cursor] <= '9') {
            const std::uintmax_t digit =
                static_cast<std::uintmax_t>(text[cursor] - '0');
            if (value > (std::numeric_limits<std::uintmax_t>::max() - digit) / 10)
                return false;
            value = value * 10 + digit;
            ++cursor;
        }
        return true;
    }

    static bool field(const std::string& object, const char* name,
                      std::string& value) {
        const std::string key = std::string("\"") + name + "\"";
        const std::size_t key_pos = object.find(key);
        if (key_pos == std::string::npos) return false;
        std::size_t cursor = key_pos + key.size();
        skipSpace(object, cursor);
        if (cursor >= object.size() || object[cursor] != ':') return false;
        ++cursor;
        return parseJsonString(object, cursor, value);
    }

    static bool field(const std::string& object, const char* name,
                      std::uintmax_t& value) {
        const std::string key = std::string("\"") + name + "\"";
        const std::size_t key_pos = object.find(key);
        if (key_pos == std::string::npos) return false;
        std::size_t cursor = key_pos + key.size();
        skipSpace(object, cursor);
        if (cursor >= object.size() || object[cursor] != ':') return false;
        ++cursor;
        return parseUnsigned(object, cursor, value);
    }

    static bool requiredString(const std::string& text, const char* name,
                               const std::string& expected,
                               std::string* observed = nullptr) {
        std::string value;
        if (!field(text, name, value)) return false;
        if (observed) *observed = value;
        return expected.empty() || value == expected;
    }

    static bool isSafeRegularFile(const guest_vfs_fs::path& path,
                                  std::uintmax_t size) {
        std::error_code error;
        const guest_vfs_fs::file_status status =
            guest_vfs_fs::symlink_status(path, error);
        if (error || guest_vfs_fs::is_symlink(status) ||
            !guest_vfs_fs::is_regular_file(status))
            return false;
        const auto observed = guest_vfs_fs::file_size(path, error);
        return !error && observed == size;
    }

    static bool hashFile(const guest_vfs_fs::path& path,
                         std::uintmax_t expected_size,
                         const std::string& expected_hash) {
        if (!isSafeRegularFile(path, expected_size)) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) return false;
        sandbox::bitnet::external_assets::Sha256 digest;
        std::vector<std::uint8_t> buffer(1024 * 1024);
        std::uintmax_t bytes = 0;
        while (input.good()) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                digest.update(buffer.data(), static_cast<std::size_t>(count));
                bytes += static_cast<std::uintmax_t>(count);
            }
        }
        if (bytes != expected_size || digest.finalHex() != expected_hash)
            return false;
        return isSafeRegularFile(path, expected_size);
    }

    void fail(const std::string& message) {
        valid_ = false;
        error_ = message;
    }

    void init() {
        std::error_code root_error;
        if (!guest_vfs_fs::is_directory(root_, root_error) || root_error) {
            fail("guest VFS root is not a directory");
            return;
        }
        const guest_vfs_fs::path package_path =
            root_ / "guest_package.v1.json";
        std::error_code size_error;
        const std::uintmax_t package_size =
            guest_vfs_fs::file_size(package_path, size_error);
        if (size_error || package_size == 0 || package_size > 4 * 1024 * 1024) {
            fail("guest package manifest is missing or too large");
            return;
        }
        std::ifstream package(package_path, std::ios::binary);
        if (!package.is_open()) {
            fail("guest package manifest cannot be opened");
            return;
        }
        std::ostringstream content;
        content << package.rdbuf();
        const std::string text = content.str();
        if (text.size() != package_size) {
            fail("guest package manifest changed while reading");
            return;
        }
        std::string version;
        if (!requiredString(text, "schema", "trit.bitnet_guest_package.v1") ||
            !requiredString(text, "status", "complete") ||
            !field(text, "profile", profile_) || profile_.empty()) {
            fail("guest package schema or completion state is invalid");
            return;
        }
        std::uintmax_t numeric_version = 0;
        if (!field(text, "version", numeric_version) || numeric_version != 1) {
            fail("guest package version is unsupported");
            return;
        }

        const std::size_t entries_key = text.find("\"entries\"");
        if (entries_key == std::string::npos) {
            fail("guest package has no entries");
            return;
        }
        const std::size_t array_begin = text.find('[', entries_key);
        const std::size_t array_end = text.find(']', array_begin);
        if (array_begin == std::string::npos || array_end == std::string::npos ||
            array_end <= array_begin) {
            fail("guest package entries are malformed");
            return;
        }

        std::size_t cursor = array_begin + 1;
        while (cursor < array_end) {
            skipSpace(text, cursor);
            if (cursor >= array_end) break;
            if (text[cursor] == ',') {
                ++cursor;
                continue;
            }
            if (text[cursor] != '{') {
                fail("guest package entry is not an object");
                return;
            }
            const std::size_t object_end = text.find('}', cursor);
            if (object_end == std::string::npos || object_end > array_end) {
                fail("guest package entry is unterminated");
                return;
            }
            const std::string object = text.substr(cursor, object_end - cursor + 1);
            std::string name;
            std::string hash;
            std::uintmax_t size = 0;
            if (!field(object, "name", name) || !isSafeRelative(name) ||
                !field(object, "size", size) || !field(object, "sha256", hash) ||
                !isLowerHexDigest(hash) || entries_.count(name) != 0) {
                fail("guest package entry has invalid name, size, or hash");
                return;
            }
            entries_.emplace(name, Entry{size, hash});
            cursor = object_end + 1;
        }

        if (!contains("manifest.csv") || !contains("conversion_metadata.v1.json") ||
            entries_.empty()) {
            fail("guest package does not contain its control files");
            return;
        }
        for (const auto& [name, entry] : entries_) {
            if (!hashFile(root_ / guest_vfs_fs::path(name), entry.size,
                          entry.sha256)) {
                fail("guest package hash or file geometry mismatch: " + name);
                return;
            }
        }
        valid_ = true;
    }
};

} // namespace sandbox::bitnet

#endif // BITNET_GUEST_VFS_H
