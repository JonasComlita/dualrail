// =============================================================================
// bitnet_loader.h - Utilities for loading BitNet b1.58 weights into Trit VM
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
#ifndef BITNET_LOADER_H
#define BITNET_LOADER_H

#include "ternary_transformer_runtime.h"
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

namespace sandbox {
namespace bitnet {

namespace fs = std::filesystem;

struct WeightLayer {
    std::string name;
    std::string filename;
    double scale = 1.0;
    std::string mode = "L50";  // "L1", "L50", or "T50"
    int count = 0;
};

class BitNetLoader {
public:
    std::string baseDir;
    std::map<std::string, WeightLayer>                   layers;
    std::map<std::string, std::vector<int8_t>>           weightCacheL1;
    std::map<std::string, std::vector<UInt128>>          weightCacheL50;
    std::map<std::string, std::vector<vm::TernaryValue>> weightCacheT50;
    std::map<std::string, std::vector<int8_t>>           weightCacheUnpacked;

    // All cache accesses must hold loaderMutex.
    std::mutex loaderMutex;

    // prefetchFutures holds in-flight async loads (key = layer name).
    // Access requires loaderMutex.
    std::map<std::string, std::future<void>> prefetchFutures;

    BitNetLoader() = default;

    explicit BitNetLoader(const std::string& dir) : baseDir(dir) {
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
                if      (layer.filename.find(".t50") != std::string::npos) layer.mode = "T50";
                else if (layer.filename.find(".l50") != std::string::npos) layer.mode = "L50";
                else                                                         layer.mode = "L1";
            }

            try {
                const std::string fullPath = baseDir + layer.filename;
                if (fs::exists(fullPath)) {
                    const uintmax_t size = fs::file_size(fullPath);
                    if      (layer.mode == "T50") layer.count = static_cast<int>(size / 16);
                    else if (layer.mode == "L50") layer.count = static_cast<int>(size / 16) * 50;
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
    // unpackedL50 — decode L50 lanes to int8 {-1, 0, 1} for AVX2 matmul.
    // Thread-safe; only decodes once even under concurrent calls.
    // -------------------------------------------------------------------------
    const std::vector<int8_t>* unpackedL50(const std::string& layerName) {
        {
            std::lock_guard<std::mutex> lk(loaderMutex);
            auto it = weightCacheUnpacked.find(layerName);
            if (it != weightCacheUnpacked.end()) return &it->second;
        }

        // Load packed form (I/O outside lock)
        const auto* packed = packedL50(layerName);
        if (!packed) return nullptr;

        const auto layer = getLayer(layerName);
        std::vector<int8_t> unpacked(static_cast<std::size_t>(layer.count));
        int out = 0;
        for (const UInt128 raw : *packed) {
            TritLane50 lane = TritLane50::fromRawForKernel(raw);
            for (int i = 0; i < TritLane50::trits && out < layer.count; ++i)
                unpacked[static_cast<std::size_t>(out++)] = lane.tritAt(i);
        }

        std::lock_guard<std::mutex> lk(loaderMutex);
        // Double-check: another thread may have decoded while we worked
        auto it = weightCacheUnpacked.find(layerName);
        if (it != weightCacheUnpacked.end()) return &it->second;
        auto [ins, _] = weightCacheUnpacked.emplace(layerName, std::move(unpacked));
        return &ins->second;
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
            this->unpackedL50(layerName); // internally lock-safe
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

} // namespace bitnet
} // namespace sandbox

#endif // BITNET_LOADER_H
