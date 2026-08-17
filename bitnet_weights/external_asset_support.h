#pragma once

// Small, dependency-free helpers shared by the opt-in native external
// benchmark profiles.  The helper validates the expected size and SHA-256
// before a payload is used; absence is represented as missing/skip and is
// never converted into a synthetic pass.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox::bitnet::external_assets {

namespace fs = std::filesystem;

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        bitLength_ = 0;
        dataLength_ = 0;
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    }

    void update(const std::uint8_t* data, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            data_[dataLength_++] = data[index];
            if (dataLength_ == 64) {
                transform();
                bitLength_ += 512;
                dataLength_ = 0;
            }
        }
    }

    std::string finalHex() {
        std::size_t index = dataLength_;
        if (dataLength_ < 56) {
            data_[index++] = 0x80;
            while (index < 56) data_[index++] = 0;
        } else {
            data_[index++] = 0x80;
            while (index < 64) data_[index++] = 0;
            transform();
            data_.fill(0);
        }
        bitLength_ += static_cast<std::uint64_t>(dataLength_) * 8;
        for (int shift = 7; shift >= 0; --shift)
            data_[56 + (7 - shift)] = static_cast<std::uint8_t>(bitLength_ >> (shift * 8));
        transform();

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (std::uint32_t word : state_) out << std::setw(8) << word;
        return out.str();
    }

private:
    std::array<std::uint8_t, 64> data_{};
    std::array<std::uint32_t, 8> state_{};
    std::uint64_t bitLength_ = 0;
    std::size_t dataLength_ = 0;

    static constexpr std::uint32_t rotr(std::uint32_t value, int count) {
        return (value >> count) | (value << (32 - count));
    }

    void transform() {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::uint32_t words[64]{};
        for (int index = 0; index < 16; ++index) {
            const int offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(data_[offset]) << 24) |
                           (static_cast<std::uint32_t>(data_[offset + 1]) << 16) |
                           (static_cast<std::uint32_t>(data_[offset + 2]) << 8) |
                           static_cast<std::uint32_t>(data_[offset + 3]);
        }
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotr(words[index - 15], 7) ^
                rotr(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const std::uint32_t s1 = rotr(words[index - 2], 17) ^
                rotr(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choice + k[index] + words[index];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
};

struct FileCheck {
    bool ok = false;
    bool missing = false;
    std::string reason;
    fs::path path;
    std::uintmax_t size = 0;
    std::string sha256;
};

struct WadLump {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

inline std::string environmentValue(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : fallback;
}

inline fs::path cacheRoot() {
    const std::string explicitRoot = environmentValue("TRIT_EXTERNAL_ASSET_ROOT");
    if (!explicitRoot.empty()) return fs::path(explicitRoot);
    return fs::path(environmentValue("TRIT_BUILD_DIR", "build")) / "external-assets";
}

inline fs::path findPayload(const std::string& assetId,
                            const std::string& relativeName,
                            const char* overrideVariable = nullptr) {
    if (overrideVariable) {
        const std::string overridePath = environmentValue(overrideVariable);
        if (!overridePath.empty()) return fs::path(overridePath);
    }
    const fs::path root = cacheRoot();
    const std::vector<fs::path> candidates = {
        root / assetId / "payload" / fs::path(relativeName),
        root / assetId / fs::path(relativeName),
        root / fs::path(relativeName),
    };
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate)) return candidate;
    }
    const fs::path assetRoot = root / assetId;
    if (fs::is_directory(assetRoot)) {
        std::vector<fs::path> matches;
        std::error_code error;
        for (fs::recursive_directory_iterator it(assetRoot, error), end; it != end && !error; it.increment(error)) {
            if (it->is_regular_file(error) && it->path().filename() == fs::path(relativeName).filename())
                matches.push_back(it->path());
        }
        if (matches.size() == 1) return matches.front();
    }
    return {};
}

inline FileCheck checkFile(const fs::path& path,
                           std::uintmax_t expectedSize,
                           const std::string& expectedSha256) {
    FileCheck result;
    result.path = path;
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        result.missing = true;
        result.reason = "external payload is missing";
        return result;
    }
    result.size = fs::file_size(path, error);
    if (error || result.size != expectedSize) {
        result.reason = "external payload size does not match provenance lock";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        result.reason = "external payload cannot be opened";
        return result;
    }
    Sha256 digest;
    std::array<std::uint8_t, 1024 * 1024> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read > 0) digest.update(buffer.data(), static_cast<std::size_t>(read));
    }
    result.sha256 = digest.finalHex();
    if (result.sha256 != expectedSha256) {
        result.reason = "external payload SHA-256 does not match provenance lock";
        return result;
    }
    result.ok = true;
    return result;
}

inline bool readAt(const fs::path& path, std::uint64_t offset,
                   std::vector<std::uint8_t>& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input.good()) return false;
    input.read(reinterpret_cast<char*>(output.data()),
               static_cast<std::streamsize>(output.size()));
    return input.gcount() == static_cast<std::streamsize>(output.size());
}

template <typename Callback>
inline bool streamRange(const fs::path& path, std::uint64_t offset,
                        std::uint64_t bytes, Callback&& callback,
                        std::size_t chunkSize = 1024 * 1024) {
    if (chunkSize == 0) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input.good()) return false;
    std::vector<std::uint8_t> buffer(chunkSize);
    std::uint64_t remaining = bytes;
    while (remaining > 0) {
        const std::size_t request = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(request));
        const std::streamsize received = input.gcount();
        if (received <= 0) return false;
        if (!callback(buffer.data(), static_cast<std::size_t>(received))) return false;
        remaining -= static_cast<std::uint64_t>(received);
    }
    return true;
}

inline std::uint32_t littleU32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::int16_t littleI16(const std::uint8_t* bytes) {
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8));
}

inline bool readWadDirectory(const fs::path& path,
                             std::vector<WadLump>& lumps,
                             std::string& reason) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size < 12) {
        reason = "WAD is shorter than its 12-byte header";
        return false;
    }
    std::vector<std::uint8_t> header(12);
    if (!readAt(path, 0, header)) {
        reason = "WAD header cannot be read";
        return false;
    }
    if ((header[0] != 'I' && header[0] != 'P') ||
        header[1] != 'W' || header[2] != 'A' || header[3] != 'D') {
        reason = "WAD magic must be IWAD or PWAD";
        return false;
    }
    const std::uint32_t lumpCount = littleU32(header.data() + 4);
    const std::uint32_t directoryOffset = littleU32(header.data() + 8);
    if (lumpCount > 1'000'000U || directoryOffset > size ||
        static_cast<std::uint64_t>(lumpCount) * 16ULL > size - directoryOffset) {
        reason = "WAD directory extends beyond file";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        reason = "WAD directory cannot be opened";
        return false;
    }
    input.seekg(static_cast<std::streamoff>(directoryOffset), std::ios::beg);
    lumps.clear();
    lumps.reserve(lumpCount);
    for (std::uint32_t index = 0; index < lumpCount; ++index) {
        std::array<std::uint8_t, 16> entry{};
        input.read(reinterpret_cast<char*>(entry.data()),
                   static_cast<std::streamsize>(entry.size()));
        if (input.gcount() != static_cast<std::streamsize>(entry.size())) {
            reason = "WAD directory entry is truncated";
            return false;
        }
        const std::uint32_t offset = littleU32(entry.data());
        const std::uint32_t bytes = littleU32(entry.data() + 4);
        if (offset > size || bytes > size - offset) {
            reason = "WAD lump data extends beyond file";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(entry.data() + 8), 8);
        while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
            name.pop_back();
        if (name.empty()) {
            reason = "WAD lump has no name";
            return false;
        }
        for (unsigned char character : name) {
            if (character < 32 || character > 126) {
                reason = "WAD lump name is not printable ASCII";
                return false;
            }
        }
        lumps.push_back({name, offset, bytes});
    }
    return true;
}

inline bool isMapMarker(const std::string& name) {
    if (name.size() == 4 && name[0] == 'E' && name[2] == 'M' &&
        std::isdigit(static_cast<unsigned char>(name[1])) &&
        std::isdigit(static_cast<unsigned char>(name[3]))) return true;
    return name.size() == 5 && name.compare(0, 3, "MAP") == 0 &&
        std::isdigit(static_cast<unsigned char>(name[3])) &&
        std::isdigit(static_cast<unsigned char>(name[4]));
}

inline const WadLump* findWadLump(const std::vector<WadLump>& lumps,
                                  std::size_t begin, std::size_t end,
                                  const std::string& name) {
    end = std::min(end, lumps.size());
    for (std::size_t index = begin; index < end; ++index)
        if (lumps[index].name == name) return &lumps[index];
    return nullptr;
}

inline bool readWadLump(const fs::path& path, const WadLump& lump,
                        std::vector<std::uint8_t>& bytes,
                        std::string& reason) {
    bytes.clear();
    bytes.reserve(lump.size);
    const bool ok = streamRange(
        path, lump.offset, lump.size,
        [&bytes](const std::uint8_t* chunk, std::size_t count) {
            bytes.insert(bytes.end(), chunk, chunk + count);
            return true;
        });
    if (!ok || bytes.size() != lump.size) {
        reason = "WAD lump payload is truncated: " + lump.name;
        bytes.clear();
        return false;
    }
    return true;
}

// Render the E1M1 dependency closure into a fixed 27x27 ternary map preview.
// Every map lump in the closure is streamed once for the raw closure digest;
// only the bounded VERTEXES/LINEDEFS/THINGS geometry is retained for rendering.
inline bool readE1M1Render(const fs::path& path,
                           std::vector<long long>& pixels,
                           std::uint32_t& totalLumps,
                           std::uint32_t& closureLumps,
                           std::uint64_t& closureBytes,
                           std::uint64_t& closureHash,
                           std::string& reason) {
    std::vector<WadLump> lumps;
    if (!readWadDirectory(path, lumps, reason)) return false;
    totalLumps = static_cast<std::uint32_t>(lumps.size());
    const auto marker = std::find_if(
        lumps.begin(), lumps.end(), [](const WadLump& lump) {
            return lump.name == "E1M1";
        });
    if (marker == lumps.end()) {
        reason = "Freedoom WAD does not contain an E1M1 marker";
        return false;
    }
    const std::size_t begin = static_cast<std::size_t>(marker - lumps.begin());
    std::size_t end = begin + 1;
    while (end < lumps.size() && !isMapMarker(lumps[end].name)) ++end;
    closureLumps = static_cast<std::uint32_t>(end - begin);
    closureBytes = 0;
    closureHash = 1469598103934665603ULL;
    for (std::size_t index = begin; index < end; ++index) {
        closureBytes += lumps[index].size;
        const bool streamed = streamRange(
            path, lumps[index].offset, lumps[index].size,
            [&closureHash](const std::uint8_t* chunk, std::size_t count) {
                for (std::size_t byte = 0; byte < count; ++byte) {
                    closureHash ^= static_cast<std::uint64_t>(chunk[byte]);
                    closureHash *= 1099511628211ULL;
                }
                return true;
            });
        if (!streamed) {
            reason = "Freedoom E1M1 lump closure could not be streamed";
            return false;
        }
    }

    const WadLump* verticesLump = findWadLump(lumps, begin, end, "VERTEXES");
    const WadLump* linesLump = findWadLump(lumps, begin, end, "LINEDEFS");
    const WadLump* thingsLump = findWadLump(lumps, begin, end, "THINGS");
    if (!verticesLump || !linesLump || !thingsLump) {
        reason = "E1M1 is missing VERTEXES, LINEDEFS, or THINGS";
        return false;
    }
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint8_t> lines;
    std::vector<std::uint8_t> things;
    if (!readWadLump(path, *verticesLump, vertices, reason) ||
        !readWadLump(path, *linesLump, lines, reason) ||
        !readWadLump(path, *thingsLump, things, reason)) return false;
    if (vertices.size() < 4 || vertices.size() % 4 != 0 ||
        lines.size() < 14 || lines.size() % 14 != 0 ||
        things.size() < 10 || things.size() % 10 != 0) {
        reason = "E1M1 geometry lump has an invalid record size";
        return false;
    }

    struct Point { std::int32_t x = 0; std::int32_t y = 0; };
    std::vector<Point> points;
    points.reserve(vertices.size() / 4);
    std::int32_t minX = std::numeric_limits<std::int32_t>::max();
    std::int32_t minY = std::numeric_limits<std::int32_t>::max();
    std::int32_t maxX = std::numeric_limits<std::int32_t>::min();
    std::int32_t maxY = std::numeric_limits<std::int32_t>::min();
    for (std::size_t offset = 0; offset < vertices.size(); offset += 4) {
        const Point point{littleI16(vertices.data() + offset),
                          littleI16(vertices.data() + offset + 2)};
        points.push_back(point);
        minX = std::min(minX, point.x); minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x); maxY = std::max(maxY, point.y);
    }
    const std::int64_t rangeX = std::max<std::int64_t>(1, maxX - minX);
    const std::int64_t rangeY = std::max<std::int64_t>(1, maxY - minY);
    constexpr int renderSize = 27;
    pixels.assign(renderSize * renderSize, 0);
    const auto projectX = [=](std::int32_t value) {
        return static_cast<int>((static_cast<std::int64_t>(value - minX) * (renderSize - 1)) / rangeX);
    };
    const auto projectY = [=](std::int32_t value) {
        return renderSize - 1 - static_cast<int>((static_cast<std::int64_t>(value - minY) * (renderSize - 1)) / rangeY);
    };
    const auto paint = [&pixels](int x, int y, long long value) {
        if (x >= 0 && x < renderSize && y >= 0 && y < renderSize)
            pixels[static_cast<std::size_t>(y * renderSize + x)] = value;
    };
    const auto drawLine = [&paint](int x0, int y0, int x1, int y1) {
        const int dx = std::abs(x1 - x0);
        const int sx = x0 < x1 ? 1 : -1;
        const int dy = -std::abs(y1 - y0);
        const int sy = y0 < y1 ? 1 : -1;
        int error = dx + dy;
        while (true) {
            paint(x0, y0, 1);
            if (x0 == x1 && y0 == y1) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x0 += sx; }
            if (twice <= dx) { error += dx; y0 += sy; }
        }
    };
    for (std::size_t offset = 0; offset < lines.size(); offset += 14) {
        const std::uint16_t first = static_cast<std::uint16_t>(
            littleI16(lines.data() + offset));
        const std::uint16_t second = static_cast<std::uint16_t>(
            littleI16(lines.data() + offset + 2));
        if (first >= points.size() || second >= points.size()) {
            reason = "E1M1 LINEDEFS references a missing VERTEXES record";
            return false;
        }
        drawLine(projectX(points[first].x), projectY(points[first].y),
                 projectX(points[second].x), projectY(points[second].y));
    }
    for (std::size_t offset = 0; offset < things.size(); offset += 10) {
        const std::int16_t type = littleI16(things.data() + offset + 6);
        if (type == 1) {
            paint(projectX(littleI16(things.data() + offset)),
                  projectY(littleI16(things.data() + offset + 2)), -1);
            break;
        }
    }
    return true;
}

inline bool readModelSample(const fs::path& path, std::uint64_t payloadOffset,
                            std::size_t bytes, std::vector<long long>& words) {
    std::vector<std::uint8_t> data(bytes);
    if (!readAt(path, payloadOffset, data)) return false;
    words.reserve(words.size() + data.size());
    for (std::uint8_t byte : data)
        words.push_back(static_cast<long long>(byte % 3U) - 1LL);
    return true;
}

inline bool readLittleU64(const fs::path& path, std::uint64_t offset,
                          std::uint64_t& value) {
    std::vector<std::uint8_t> bytes(8);
    if (!readAt(path, offset, bytes)) return false;
    value = 0;
    for (int index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(index)]) << (index * 8);
    return true;
}

inline bool validateWadFile(const fs::path& path, std::uint32_t& lumpCount,
                            std::string& reason) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size < 12) {
        reason = "WAD is shorter than its 12-byte header";
        return false;
    }
    std::vector<std::uint8_t> header(12);
    if (!readAt(path, 0, header)) {
        reason = "WAD header cannot be read";
        return false;
    }
    if ((header[0] != 'I' && header[0] != 'P') ||
        header[1] != 'W' || header[2] != 'A' || header[3] != 'D') {
        reason = "WAD magic must be IWAD or PWAD";
        return false;
    }
    lumpCount = static_cast<std::uint32_t>(header[4]) |
        (static_cast<std::uint32_t>(header[5]) << 8) |
        (static_cast<std::uint32_t>(header[6]) << 16) |
        (static_cast<std::uint32_t>(header[7]) << 24);
    const std::uint32_t directoryOffset = static_cast<std::uint32_t>(header[8]) |
        (static_cast<std::uint32_t>(header[9]) << 8) |
        (static_cast<std::uint32_t>(header[10]) << 16) |
        (static_cast<std::uint32_t>(header[11]) << 24);
    if (lumpCount > 1'000'000U || directoryOffset > size ||
        static_cast<std::uint64_t>(lumpCount) * 16ULL > size - directoryOffset) {
        reason = "WAD directory extends beyond file";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(directoryOffset), std::ios::beg);
    for (std::uint32_t index = 0; index < lumpCount; ++index) {
        std::array<std::uint8_t, 16> entry{};
        input.read(reinterpret_cast<char*>(entry.data()),
                   static_cast<std::streamsize>(entry.size()));
        if (input.gcount() != static_cast<std::streamsize>(entry.size())) {
            reason = "WAD directory entry is truncated";
            return false;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(entry[0]) |
            (static_cast<std::uint32_t>(entry[1]) << 8) |
            (static_cast<std::uint32_t>(entry[2]) << 16) |
            (static_cast<std::uint32_t>(entry[3]) << 24);
        const std::uint32_t bytes = static_cast<std::uint32_t>(entry[4]) |
            (static_cast<std::uint32_t>(entry[5]) << 8) |
            (static_cast<std::uint32_t>(entry[6]) << 16) |
            (static_cast<std::uint32_t>(entry[7]) << 24);
        bool named = false;
        for (int nameIndex = 8; nameIndex < 16 && entry[nameIndex] != 0; ++nameIndex) {
            if (entry[nameIndex] < 32 || entry[nameIndex] > 126) {
                reason = "WAD lump name is not printable ASCII";
                return false;
            }
            named = true;
        }
        if (!named || offset > size || bytes > size - offset) {
            reason = "WAD lump data extends beyond file or has no name";
            return false;
        }
    }
    return true;
}

inline bool safetensorsPayloadOffset(const fs::path& path, std::uint64_t& offset) {
    std::uint64_t headerLength = 0;
    if (!readLittleU64(path, 0, headerLength) || headerLength > 128ULL * 1024ULL * 1024ULL)
        return false;
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || headerLength > size - std::min<std::uintmax_t>(size, 8)) return false;
    offset = 8 + headerLength;
    return offset <= size;
}

} // namespace sandbox::bitnet::external_assets
