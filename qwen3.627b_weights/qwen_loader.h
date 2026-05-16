// =============================================================================
// qwen_loader.h - Utilities for loading Qwen weights into Trit VM
// =============================================================================
//
// Fixes vs. previous version:
//   - prefetchAsync / unpackedL50 deadlock: prefetchAsync held loaderMutex
//     while launching an async task that called unpackedL50, which also tries
//     to acquire loaderMutex → guaranteed deadlock on every prefetch.
//     Fix: release the lock before spawning the async task.
//   - unpackedL50 double-checks the cache inside the lock before inserting,
//     so concurrent prefetches of the same layer don't duplicate work.
//   - waitPrefetch moves the future out under the lock, then waits outside,
//     so it does not hold the lock for the duration of the wait.
//   - weightCacheUnpacked also checked in prefetchAsync before launching,
//     so a completed prefetch doesn't spawn a second redundant task.

#pragma once
#ifndef QWEN_LOADER_H
#define QWEN_LOADER_H

#include "ternary_transformer_runtime.h"
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <new>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {
namespace qwen {

namespace fs = std::filesystem;

struct WeightLayer {
    std::string name;
    std::string filename;
    double scale = 1.0;
    std::string mode = "T40";  // "L1", "T40", "L50", or "T50"
    int count = 0;
};

class QwenLoader {
public:
    std::string baseDir;
    std::map<std::string, WeightLayer>                   layers;
    std::map<std::string, std::vector<int8_t>>           weightCacheL1;
    std::map<std::string, std::vector<uint64_t>>         weightCacheT40;
    std::map<std::string, std::vector<uint8_t>>          weightCacheT2;
    std::map<std::string, std::vector<uint16_t>>         weightCacheBF16;
    std::map<std::string, std::vector<UInt128>>          weightCacheL50;
    std::map<std::string, std::vector<vm::TernaryValue>> weightCacheT50;
    std::map<std::string, std::vector<int8_t>>           weightCacheUnpacked;

    // All cache accesses must hold loaderMutex.
    std::mutex loaderMutex;

    // prefetchFutures holds in-flight async loads (key = layer name).
    // Access requires loaderMutex.
    std::map<std::string, std::future<void>> prefetchFutures;

    QwenLoader() = default;

    static bool debugEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("QWEN_LOADER_DEBUG");
            return value && std::string(value) != "0";
        }();
        return enabled;
    }

    explicit QwenLoader(const std::string& dir) : baseDir(dir) {
        normaliseDir(baseDir);
        init(baseDir);
    }

    bool init(const std::string& baseDir_) {
        baseDir = baseDir_;
        normaliseDir(baseDir);

        std::ifstream file(baseDir + "manifest.csv");
        if (!file.is_open()) return false;

        auto trim = [](std::string& s) {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) { s.clear(); return; }
            s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
        };

        layers.clear();
        std::string line;
        bool firstLine = true;
        while (std::getline(file, line)) {
            trim(line);
            if (line.empty()) continue;
            if (firstLine) {
                firstLine = false;
                if (line.find("parameter_name") != std::string::npos) continue;
            }
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, ',')) { trim(part); parts.push_back(part); }
            if (parts.size() < 3) continue;

            WeightLayer layer;
            layer.name     = parts[0];
            layer.filename = parts[1];
            try { layer.scale = std::stod(parts[2]); } catch (...) { layer.scale = 1.0; }

            if (parts.size() >= 4) {
                layer.mode = parts[3];
            } else {
                if      (layer.filename.find(".t40") != std::string::npos) layer.mode = "T40";
                else if (layer.filename.find(".t2") != std::string::npos) layer.mode = "T2";
                else if (layer.filename.find(".t50") != std::string::npos) layer.mode = "T50";
                else if (layer.filename.find(".l50") != std::string::npos) layer.mode = "L50";
                else                                                         layer.mode = "L1";
            }

            try {
                const std::string fullPath = baseDir + layer.filename;
                if (fs::exists(fullPath)) {
                    const uintmax_t size = fs::file_size(fullPath);
                    if      (layer.mode == "T2")   layer.count = static_cast<int>(size * 4);
                    else if (layer.mode == "T40")  layer.count = static_cast<int>(size / 8) * 40;
                    else if (layer.mode == "BF16") layer.count = static_cast<int>(size / 2);
                    else if (layer.mode == "L50")  layer.count = static_cast<int>(size / 16) * 50;
                    else                           layer.count = static_cast<int>(size);
                }
            } catch (...) { layer.count = 0; }

            layers[layer.name] = layer;
        }
        return !layers.empty();
    }

    WeightLayer getLayer(const std::string& name) const {
        auto it = layers.find(name);
        return it != layers.end() ? it->second : WeightLayer{};
    }

    double getScale(const std::string& name) const {
        auto it = layers.find(name);
        return it != layers.end() ? it->second.scale : 1.0;
    }

    size_t getTotalWeightCount() const {
        size_t total = 0;
        for (const auto& [_, layer] : layers) total += layer.count;
        return total;
    }

    // -------------------------------------------------------------------------
    // packedL50 — load and cache raw L50 128-bit lanes.
    // Thread-safe (acquires loaderMutex for cache write).
    // -------------------------------------------------------------------------
    const std::vector<UInt128>* packedL50(const std::string& layerName) {
        const auto layer = getLayer(layerName);
        if (layer.name.empty() || layer.mode != "L50") return nullptr;

        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheL50.find(layerName);
            if (it != weightCacheL50.end()) return &it->second;
        }

        // I/O outside the lock so other threads can continue
        const int laneCount = (layer.count + TritLane50::trits - 1) / TritLane50::trits;
        std::vector<UInt128> lanes(static_cast<std::size_t>(laneCount));
        {
            std::ifstream f(baseDir + layer.filename, std::ios::binary);
            if (!f.is_open()) return nullptr;
            f.read(reinterpret_cast<char*>(lanes.data()),
                   static_cast<std::streamsize>(static_cast<std::size_t>(laneCount) * 16));
            if (!f) return nullptr;
        }

        std::lock_guard<std::mutex> lk(loaderMutex);
        // Another thread may have loaded while we did I/O — check again
        auto it = weightCacheL50.find(layerName);
        if (it != weightCacheL50.end()) return &it->second;
        auto [ins, _] = weightCacheL50.emplace(layerName, std::move(lanes));
        return &ins->second;
    }

    // -------------------------------------------------------------------------
    // bf16 — load and cache raw BF16.
    // -------------------------------------------------------------------------
    const std::vector<uint16_t>* bf16(const std::string& name) {
        std::lock_guard<std::mutex> lock(loaderMutex);
        auto it = weightCacheBF16.find(name);
        if (it != weightCacheBF16.end()) return &it->second;

        auto lit = layers.find(name);
        if (lit == layers.end() || lit->second.mode != "BF16") return nullptr;

        std::string path = baseDir + lit->second.filename;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return nullptr;

        std::vector<uint16_t> vec(static_cast<std::size_t>(lit->second.count));
        char* dst = reinterpret_cast<char*>(vec.data());
        std::size_t remaining = vec.size() * sizeof(uint16_t);
        constexpr std::size_t chunkBytes = 64ull * 1024ull * 1024ull;
        while (remaining > 0) {
            const std::size_t n = remaining < chunkBytes ? remaining : chunkBytes;
            f.read(dst, static_cast<std::streamsize>(n));
            if (f.gcount() != static_cast<std::streamsize>(n)) {
                std::cerr << "bf16: short read " << name
                          << " expected chunk=" << n
                          << " got=" << f.gcount() << "\n";
                return nullptr;
            }
            dst += n;
            remaining -= n;
        }
        
        auto res = weightCacheBF16.emplace(name, std::move(vec));
        return &res.first->second;
    }

    // -------------------------------------------------------------------------
    // packedT40 — load and cache raw T40 (Triple) 64-bit base-3 words.
    // -------------------------------------------------------------------------
    const std::vector<uint64_t>* packedT40(const std::string& layerName) {
        const auto layer = getLayer(layerName);
        if (layer.name.empty() || layer.mode != "T40") return nullptr;

        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheT40.find(layerName);
            if (it != weightCacheT40.end()) return &it->second;
        }

        const int wordCount = layer.count / 40;
        std::vector<uint64_t> words(static_cast<std::size_t>(wordCount));
        {
            std::ifstream f(baseDir + layer.filename, std::ios::binary);
            if (!f.is_open()) return nullptr;
            f.read(reinterpret_cast<char*>(words.data()),
                   static_cast<std::streamsize>(static_cast<std::size_t>(wordCount) * 8));
            if (!f) return nullptr;
        }

        std::lock_guard<std::mutex> lk(loaderMutex);
        auto it = weightCacheT40.find(layerName);
        if (it != weightCacheT40.end()) return &it->second;
        auto [ins, _] = weightCacheT40.emplace(layerName, std::move(words));
        return &ins->second;
    }

    // -------------------------------------------------------------------------
    // packedT2 — load and cache raw T2 2-bit bytes.
    // -------------------------------------------------------------------------
    const std::vector<uint8_t>* packedT2(const std::string& layerName) {
        const auto layer = getLayer(layerName);
        if (layer.name.empty() || layer.mode != "T2") return nullptr;

        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheT2.find(layerName);
            if (it != weightCacheT2.end()) return &it->second;
        }

        const int byteCount = (layer.count + 3) / 4;
        if (debugEnabled()) {
            std::cerr << "packedT2: begin " << layerName
                      << " file=" << layer.filename
                      << " count=" << layer.count
                      << " bytes=" << byteCount << "\n";
        }
        std::vector<uint8_t> bytes(static_cast<std::size_t>(byteCount));
        {
            std::ifstream f(baseDir + layer.filename, std::ios::binary);
            if (!f.is_open()) { std::cerr << "packedT2: cannot open " << baseDir + layer.filename << "\n"; return nullptr; }
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(byteCount));
            if (!f) { std::cerr << "packedT2: failed to read " << byteCount << " bytes from " << layer.filename << "\n"; return nullptr; }
        }
        if (debugEnabled()) std::cerr << "packedT2: read complete " << layerName << "\n";

        std::lock_guard<std::mutex> lk(loaderMutex);
        auto it = weightCacheT2.find(layerName);
        if (it != weightCacheT2.end()) return &it->second;
        if (debugEnabled()) std::cerr << "packedT2: cache emplace begin " << layerName << "\n";
        auto [ins, _] = weightCacheT2.emplace(layerName, std::move(bytes));
        if (debugEnabled()) {
            std::cerr << "packedT2: cache emplace done " << layerName
                      << " cached_bytes=" << ins->second.size() << "\n";
        }
        return &ins->second;
    }

    void dropPackedT2(const std::string& layerName) {
        std::lock_guard<std::mutex> lk(loaderMutex);
        weightCacheT2.erase(layerName);
    }

    void dropBF16(const std::string& layerName) {
        std::lock_guard<std::mutex> lk(loaderMutex);
        weightCacheBF16.erase(layerName);
    }

    // -------------------------------------------------------------------------
    // unpackedAny — decode T40, L50 or T50 to int8 {-1, 0, 1} for AVX2 matmul.
    // -------------------------------------------------------------------------
    const std::vector<int8_t>* unpackedAny(const std::string& layerName) {
        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheUnpacked.find(layerName);
            if (it != weightCacheUnpacked.end()) return &it->second;
        }

        const auto layer = getLayer(layerName);
        if (layer.name.empty()) {
            if (debugEnabled()) std::cerr << "unpackedAny: layer not found " << layerName << "\n";
            return nullptr;
        }

        std::vector<int8_t> unpacked;
        if (layer.mode == "T40") {
            const auto* packed = packedT40(layerName);
            if (!packed) return nullptr;
            unpacked.resize(static_cast<std::size_t>(layer.count));
            int out = 0;
            for (uint64_t raw : *packed) {
                auto trits = TernaryScalar<40>{raw}.unpack();
                for (int i = 0; i < 40 && out < layer.count; ++i)
                    unpacked[static_cast<std::size_t>(out++)] = trits[static_cast<std::size_t>(i)];
            }
        } else if (layer.mode == "L50") {
            const auto* packed = packedL50(layerName);
            if (!packed) return nullptr;
            unpacked.resize(static_cast<std::size_t>(layer.count));
            int out = 0;
            for (const UInt128 raw : *packed) {
                TritLane50 lane = TritLane50::fromRawForKernel(raw);
                for (int i = 0; i < TritLane50::trits && out < layer.count; ++i)
                    unpacked[static_cast<std::size_t>(out++)] = lane.tritAt(i);
            }
        } else if (layer.mode == "T2") {
            if (debugEnabled()) {
                std::cerr << "unpackedAny[T2]: begin " << layerName
                          << " count=" << layer.count
                          << " expected_bytes=" << ((layer.count + 3) / 4)
                          << " scale=" << layer.scale << "\n";
            }
            const auto* packed = packedT2(layerName);
            if (!packed) {
                std::cerr << "unpackedAny[T2]: packedT2 returned null " << layerName << "\n";
                return nullptr;
            }
            if (debugEnabled()) {
                std::cerr << "unpackedAny[T2]: packed ready " << layerName
                          << " packed_bytes=" << packed->size() << "\n";
            }
            try {
                if (debugEnabled()) std::cerr << "unpackedAny[T2]: resize begin " << layerName << "\n";
                unpacked.resize(static_cast<std::size_t>(layer.count));
                if (debugEnabled()) {
                    std::cerr << "unpackedAny[T2]: resize done " << layerName
                              << " unpacked_size=" << unpacked.size() << "\n";
                }
                int out = 0;
                for (uint8_t raw : *packed) {
                    for (int i = 0; i < 4 && out < layer.count; ++i) {
                        uint8_t bits = (raw >> (i * 2)) & 0b11;
                        if (bits == 0b00) unpacked[static_cast<std::size_t>(out++)] = 0;
                        else if (bits == 0b01) unpacked[static_cast<std::size_t>(out++)] = 1;
                        else unpacked[static_cast<std::size_t>(out++)] = -1;
                    }
                }
                if (debugEnabled()) {
                    std::cerr << "unpackedAny[T2]: decode done " << layerName
                              << " decoded=" << out << "\n";
                }
            } catch (const std::bad_alloc& e) {
                std::cerr << "unpackedAny[T2]: bad_alloc for " << layerName
                          << " count=" << layer.count
                          << " what=" << e.what() << "\n";
                return nullptr;
            } catch (const std::exception& e) {
                std::cerr << "unpackedAny[T2]: exception for " << layerName
                          << " what=" << e.what() << "\n";
                return nullptr;
            }
        } else {
            std::cerr << "unpackedAny: unknown mode " << layer.mode << " for " << layerName << "\n";
            return nullptr;
        }

        std::lock_guard<std::mutex> lk(loaderMutex);
        auto it = weightCacheUnpacked.find(layerName);
        if (it != weightCacheUnpacked.end()) return &it->second;
        if (debugEnabled()) {
            std::cerr << "unpackedAny: cache emplace begin " << layerName
                      << " size=" << unpacked.size() << "\n";
        }
        auto [ins, _] = weightCacheUnpacked.emplace(layerName, std::move(unpacked));
        if (debugEnabled()) {
            std::cerr << "unpackedAny: cache emplace done " << layerName
                      << " cached_size=" << ins->second.size() << "\n";
        }
        return &ins->second;
    }

    // For backward compatibility with qwen_inference.h
    const std::vector<int8_t>* unpackedL50(const std::string& layerName) {
        return unpackedAny(layerName);
    }

    // -------------------------------------------------------------------------
    // prefetchAsync — schedule background decode of a layer.
    //
    // DEADLOCK FIX: the previous version held loaderMutex while launching the
    // async task, which called unpackedL50, which also acquires loaderMutex.
    // Fix: determine whether to launch while holding the lock, then release
    // before actually calling std::async.
    // -------------------------------------------------------------------------
    void prefetchAsync(const std::string& layerName) {
        bool shouldLaunch = false;
        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            // Already cached or already in flight — do nothing
            if (weightCacheUnpacked.count(layerName)) return;
            if (weightCacheL50.count(layerName)     ) {
                // Packed is ready but unpacked isn't; launch decode
                shouldLaunch = !prefetchFutures.count(layerName);
            } else {
                shouldLaunch = !prefetchFutures.count(layerName);
            }
        }

        if (!shouldLaunch) return;

        // Launch the async task OUTSIDE the lock
        std::future<void> fut = std::async(std::launch::async,
                                            [this, layerName]() {
            this->unpackedAny(layerName); // internally lock-safe
        });

        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            // Another thread may have raced and launched first
            if (!prefetchFutures.count(layerName))
                prefetchFutures.emplace(layerName, std::move(fut));
            // Otherwise fut destructor will join immediately — harmless
        }
    }

    // -------------------------------------------------------------------------
    // waitPrefetch — block until a prefetch completes (if one is in flight).
    // Moves the future out under the lock so we don't hold the lock while
    // blocking on future.get().
    // -------------------------------------------------------------------------
    void waitPrefetch(const std::string& layerName) {
        std::future<void> fut;
        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = prefetchFutures.find(layerName);
            if (it == prefetchFutures.end()) return;
            fut = std::move(it->second);
            prefetchFutures.erase(it);
        }
        // Block outside the lock — other threads can proceed
        if (fut.valid()) fut.get();
    }

    // -------------------------------------------------------------------------
    // decodedL1 — for the VM path (L1 or L50 → int8).
    // Not needed by the host path (which uses unpackedL50 directly).
    // -------------------------------------------------------------------------
    const std::vector<int8_t>* decodedL1(const std::string& layerName) {
        const auto layer = getLayer(layerName);
        if (layer.name.empty()) return nullptr;

        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheL1.find(layerName);
            if (it != weightCacheL1.end()) return &it->second;
        }

        std::vector<int8_t> decoded(static_cast<std::size_t>(layer.count));
        if (layer.mode == "L1") {
            std::ifstream f(baseDir + layer.filename, std::ios::binary);
            if (!f.is_open()) return nullptr;
            f.read(reinterpret_cast<char*>(decoded.data()),
                   static_cast<std::streamsize>(decoded.size()));
            if (!f) return nullptr;
        } else if (layer.mode == "L50") {
            const auto* packed = packedL50(layerName);
            if (!packed) return nullptr;
            int out = 0;
            for (const UInt128 raw : *packed) {
                TritLane50 lane = TritLane50::fromRawForKernel(raw);
                for (int i = 0; i < TritLane50::trits && out < layer.count; ++i)
                    decoded[static_cast<std::size_t>(out++)] = lane.tritAt(i);
            }
        } else {
            return nullptr;
        }

        std::lock_guard<std::mutex> lk(loaderMutex);
        auto it = weightCacheL1.find(layerName);
        if (it != weightCacheL1.end()) return &it->second;
        auto [ins, _] = weightCacheL1.emplace(layerName, std::move(decoded));
        return &ins->second;
    }

    // -------------------------------------------------------------------------
    // loadLayerToVM — for the VM path only.
    // Host path uses unpackedL50() directly and never calls this for weights.
    // -------------------------------------------------------------------------
    int loadLayerToVM(vm::VMState& state, const std::string& layerName,
                      int baseAddr, int maxCount = -1, int offsetInWeights = 0) {
        const auto layer = getLayer(layerName);
        if (layer.name.empty()) return 0;
        if (baseAddr < 0 || baseAddr >= state.dmem.size()) return 0;

        const int available = std::max(0, layer.count - offsetInWeights);
        const int requested = (maxCount > 0) ? std::min(maxCount, available) : available;
        if (requested <= 0) return 0;
        if (baseAddr + requested > state.dmem.size()) return 0;

        if (layer.mode == "L1" || layer.mode == "L50") {
            const auto* decoded = decodedL1(layerName);
            if (!decoded) return 0;
            for (int i = 0; i < requested; ++i) {
                TritLane1 lane;
                lane.setTrit(0, (*decoded)[static_cast<std::size_t>(offsetInWeights + i)]);
                state.dmem.words[baseAddr + i] = vm::TernaryValue::fromL1(lane);
            }
            return requested;
        }

        if (offsetInWeights == 0) {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheT50.find(layerName);
            if (it != weightCacheT50.end()) {
                const int toCopy = std::min(requested, static_cast<int>(it->second.size()));
                std::copy(it->second.begin(), it->second.begin() + toCopy,
                          state.dmem.words + baseAddr);
                return toCopy;
            }
        }

        std::ifstream f(baseDir + layer.filename, std::ios::binary);
        if (!f.is_open()) return 0;
        if (offsetInWeights > 0)
            f.seekg(static_cast<std::streamoff>(offsetInWeights) * 16, std::ios::beg);

        int count = 0;
        std::vector<vm::TernaryValue> loaded_t50;
        if (offsetInWeights == 0 && maxCount == -1 && requested <= 1000000)
            loaded_t50.reserve(static_cast<std::size_t>(requested));

        const TernaryMode targetMode = (layer.mode == "T50") ? TernaryMode::T50 : TernaryMode::L50;
        for (; count < requested; ++count) {
            uint64_t lo = 0, hi = 0;
            f.read(reinterpret_cast<char*>(&lo), sizeof(lo));
            f.read(reinterpret_cast<char*>(&hi), sizeof(hi));
            if (!f) break;
            vm::TernaryValue val;
            val.mode = targetMode;
            val.bits = UInt128(hi, lo);
            state.dmem.words[baseAddr + count] = val;
            if (!loaded_t50.empty() || loaded_t50.capacity() > 0)
                loaded_t50.push_back(val);
        }
        if (count == requested && offsetInWeights == 0 && maxCount == -1 && !loaded_t50.empty()) {
            std::lock_guard<std::mutex> lk(loaderMutex);
            weightCacheT50.emplace(layerName, std::move(loaded_t50));
        }
        return count;
    }

    // -------------------------------------------------------------------------
    // loadModelSubset — utility for VM-path bulk loading
    // -------------------------------------------------------------------------
    int loadModelSubset(vm::VMState& state, const std::string& pattern, int baseAddr) {
        int totalLoaded = 0, currentAddr = baseAddr;
        for (const auto& [name, layer] : layers) {
            if (pattern != "*" && name.find(pattern) == std::string::npos) continue;
            if (currentAddr + layer.count > state.dmem.size()) {
                std::cerr << "Warning: Skipping " << name << " — exceeds DMEM\n";
                continue;
            }
            int loaded = loadLayerToVM(state, name, currentAddr);
            if (loaded > 0) { totalLoaded += loaded; currentAddr += loaded; }
        }
        return totalLoaded;
    }

private:
    static void normaliseDir(std::string& dir) {
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir += '/';
    }
};

} // namespace qwen
} // namespace sandbox

#endif // QWEN_LOADER_H
