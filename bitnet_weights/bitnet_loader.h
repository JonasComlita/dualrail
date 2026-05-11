// =============================================================================
// bitnet_loader.h - Utilities for loading BitNet b1.58 weights into Trit VM
// =============================================================================

#pragma once
#ifndef BITNET_LOADER_H
#define BITNET_LOADER_H

#include "../ternary_transformer_runtime.h"
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <future>
#include <condition_variable>
#include <queue>
#include <functional>

namespace sandbox {
namespace bitnet {

namespace fs = std::filesystem;

struct WeightLayer {
    std::string name;
    std::string filename;
    double scale = 1.0;
    std::string mode = "L1"; // "L1" or "T50"
    int count = 0;
};

class BitNetLoader {
public:
    std::string baseDir;
    std::map<std::string, WeightLayer> layers;
    std::map<std::string, std::vector<int8_t>> weightCacheL1;
    std::map<std::string, std::vector<UInt128>> weightCacheL50;
    std::map<std::string, std::vector<vm::TernaryValue>> weightCacheT50;
    std::map<std::string, std::vector<int8_t>> weightCacheUnpacked;

    std::mutex loaderMutex;
    std::map<std::string, std::future<void>> prefetchFutures;

    BitNetLoader() = default;
    BitNetLoader(const std::string& dir) : baseDir(dir) {
        if (!baseDir.empty() && baseDir.back() != '/' && baseDir.back() != '\\') {
            baseDir += "/";
        }
        init(baseDir);
    }

    bool init(const std::string& baseDir_) {
        baseDir = baseDir_;
        if (!baseDir.empty() && baseDir.back() != '/' && baseDir.back() != '\\') {
            baseDir += "/";
        }
        
        std::ifstream file(baseDir + "manifest.csv");
        if (!file.is_open()) return false;

        auto trim = [](std::string& s) {
            size_t first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) { s = ""; return; }
            size_t last = s.find_last_not_of(" \t\r\n");
            s = s.substr(first, (last - first + 1));
        };

        layers.clear();
        std::string line;
        bool firstLine = true;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            trim(line);
            if (line.empty()) continue;

            if (firstLine) {
                firstLine = false;
                if (line.find("parameter_name") != std::string::npos) continue;
            }
            
            std::vector<std::string> parts;
            std::stringstream ss(line);
            std::string part;
            while (std::getline(ss, part, ',')) {
                trim(part);
                parts.push_back(part);
            }

            if (parts.size() < 3) continue;

            WeightLayer layer;
            layer.name = parts[0];
            layer.filename = parts[1];
            try {
                layer.scale = std::stod(parts[2]);
            } catch (...) {
                layer.scale = 1.0;
            }

            if (parts.size() >= 4) {
                layer.mode = parts[3];
            } else {
                if (layer.filename.find(".t50") != std::string::npos) layer.mode = "T50";
                else if (layer.filename.find(".l50") != std::string::npos) layer.mode = "L50";
                else layer.mode = "L1";
            }

            // Get file size to update count
            try {
                std::string fullPath = baseDir + layer.filename;
                if (fs::exists(fullPath)) {
                    uintmax_t size = fs::file_size(fullPath);
                    if (layer.mode == "T50") {
                        layer.count = static_cast<int>(size / 16);
                    } else if (layer.mode == "L50") {
                        layer.count = static_cast<int>(size / 16) * 50;
                    } else {
                        layer.count = static_cast<int>(size);
                    }
                }
            } catch (...) {
                layer.count = 0;
            }

            layers[layer.name] = layer;
        }

        return !layers.empty();
    }

    WeightLayer getLayer(const std::string& name) const {
        auto it = layers.find(name);
        if (it != layers.end()) return it->second;
        return {};
    }

    double getScale(const std::string& name) const {
        auto it = layers.find(name);
        return it == layers.end() ? 1.0 : it->second.scale;
    }

    size_t getTotalWeightCount() const {
        size_t total = 0;
        for (const auto& [name, layer] : layers) total += layer.count;
        return total;
    }

    // Loads a range of layers to stay within RAM limits
    int loadModelSubset(vm::VMState& state, const std::string& pattern, int baseAddr) {
        int totalLoaded = 0;
        int currentAddr = baseAddr;
        
        for (const auto& [name, layer] : layers) {
            if (pattern != "*" && name.find(pattern) == std::string::npos) continue;
            
            // Check if this layer would exceed DMEM capacity
            if (currentAddr + layer.count > state.dmem.size()) {
                std::cerr << "Warning: Skipping layer " << name << " - exceeds DMEM capacity\n";
                continue;
            }

            std::cout << "Loading " << name << " (" << layer.count << " weights) at 0x" << std::hex << currentAddr << std::dec << "...\n";
            int loaded = loadLayerToVM(state, name, currentAddr);
            if (loaded > 0) {
                totalLoaded += loaded;
                currentAddr += loaded;
            }
        }
        return totalLoaded;
    }

    const std::vector<UInt128>* packedL50(const std::string& layerName) {
        auto layer = getLayer(layerName);
        if (layer.name.empty() || layer.mode != "L50") return nullptr;

        {
            std::lock_guard<std::mutex> lock(loaderMutex);
            auto cached = weightCacheL50.find(layerName);
            if (cached != weightCacheL50.end()) return &cached->second;
        }

        const int laneCount = (layer.count + TritLane50::trits - 1) / TritLane50::trits;
        std::vector<UInt128> lanes(static_cast<std::size_t>(laneCount));

        std::string fullPath = baseDir + layer.filename;
        std::ifstream file(fullPath, std::ios::binary);
        if (!file.is_open()) return nullptr;

        file.read(reinterpret_cast<char*>(lanes.data()), static_cast<std::streamsize>(static_cast<size_t>(laneCount) * 16));
        if (!file) return nullptr;
        file.close();

        {
            std::lock_guard<std::mutex> lock(loaderMutex);
            auto [it, inserted] = weightCacheL50.emplace(layerName, std::move(lanes));
            (void)inserted;
            return &it->second;
        }
    }

    const std::vector<int8_t>* unpackedL50(const std::string& layerName) {
        {
            std::lock_guard<std::mutex> lock(loaderMutex);
            auto it = weightCacheUnpacked.find(layerName);
            if (it != weightCacheUnpacked.end()) return &it->second;
        }

        const auto* packed = packedL50(layerName);
        if (!packed) return nullptr;

        auto layer = getLayer(layerName);
        std::vector<int8_t> unpacked;
        unpacked.resize(static_cast<std::size_t>(layer.count));

        int out = 0;
        for (UInt128 raw : *packed) {
            TritLane50 lane = TritLane50::fromRawForKernel(raw);
            for (int i = 0; i < TritLane50::trits && out < layer.count; ++i) {
                unpacked[static_cast<std::size_t>(out++)] = lane.tritAt(i);
            }
        }

        {
            std::lock_guard<std::mutex> lock(loaderMutex);
            auto [it, inserted] = weightCacheUnpacked.emplace(layerName, std::move(unpacked));
            (void)inserted;
            return &it->second;
        }
    }

    void prefetchAsync(const std::string& layerName) {
        std::lock_guard<std::mutex> lock(loaderMutex);
        if (weightCacheL50.count(layerName) || prefetchFutures.count(layerName)) return;

        prefetchFutures[layerName] = std::async(std::launch::async, [this, layerName]() {
            this->unpackedL50(layerName);
        });
    }

    void waitPrefetch(const std::string& layerName) {
        std::future<void> fut;
        {
            std::lock_guard<std::mutex> lock(loaderMutex);
            auto it = prefetchFutures.find(layerName);
            if (it == prefetchFutures.end()) return;
            fut = std::move(it->second);
            prefetchFutures.erase(it);
        }
        if (fut.valid()) fut.get();
    }

    const std::vector<int8_t>* decodedL1(const std::string& layerName) {
        auto layer = getLayer(layerName);
        if (layer.name.empty()) return nullptr;

        auto cached = weightCacheL1.find(layerName);
        if (cached != weightCacheL1.end()) return &cached->second;

        std::vector<int8_t> decoded;
        decoded.resize(static_cast<std::size_t>(layer.count));

        if (layer.mode == "L1") {
            std::string fullPath = baseDir + layer.filename;
            std::ifstream file(fullPath, std::ios::binary);
            if (!file.is_open()) return nullptr;
            file.read(reinterpret_cast<char*>(decoded.data()), decoded.size());
            if (!file) return nullptr;
        } else if (layer.mode == "L50") {
            const auto* lanes = packedL50(layerName);
            if (!lanes) return nullptr;

            int out = 0;
            for (UInt128 raw : *lanes) {
                TritLane50 lane = TritLane50::fromRawForKernel(raw);
                for (int i = 0; i < TritLane50::trits && out < layer.count; ++i) {
                    decoded[static_cast<std::size_t>(out++)] = lane.tritAt(i);
                }
            }
        } else {
            return nullptr;
        }

        auto [it, inserted] = weightCacheL1.emplace(layerName, std::move(decoded));
        (void)inserted;
        return &it->second;
    }

    /**
     * Loads a layer into VM DMEM with high-speed buffered reading.
     * @param offsetInWeights Number of logical weights to skip in the file.
     */
    int loadLayerToVM(vm::VMState& state, const std::string& layerName, int baseAddr, int maxCount = -1, int offsetInWeights = 0) {
        auto layer = getLayer(layerName);
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

        if (offsetInWeights == 0 && weightCacheT50.count(layerName)) {
            const auto& cached = weightCacheT50[layerName];
            const int toCopy = std::min(requested, static_cast<int>(cached.size()));
            for (int i = 0; i < toCopy; ++i) {
                state.dmem.words[baseAddr + i] = cached[static_cast<std::size_t>(i)];
            }
            return toCopy;
        }

        std::string fullPath = baseDir + layer.filename;
        std::ifstream file(fullPath, std::ios::binary);
        if (!file.is_open()) return 0;

        if (offsetInWeights > 0) {
            file.seekg(static_cast<std::streamoff>(offsetInWeights) * 16, std::ios::beg);
        }

        int count = 0;
        std::vector<vm::TernaryValue> loaded_t50;
        if (offsetInWeights == 0 && maxCount == -1 && requested <= 1000000) {
            loaded_t50.reserve(static_cast<std::size_t>(requested));
        }

        TernaryMode targetMode = (layer.mode == "T50") ? TernaryMode::T50 : TernaryMode::L50;
        for (; count < requested; ++count) {
            uint64_t lo = 0;
            uint64_t hi = 0;
            file.read(reinterpret_cast<char*>(&lo), sizeof(lo));
            file.read(reinterpret_cast<char*>(&hi), sizeof(hi));
            if (!file) break;

            vm::TernaryValue val;
            val.mode = targetMode;
            val.bits = UInt128(hi, lo);
            state.dmem.words[baseAddr + count] = val;
            if (!loaded_t50.empty() || loaded_t50.capacity() > 0) {
                loaded_t50.push_back(val);
            }
        }

        if (count == requested && offsetInWeights == 0 && maxCount == -1 && !loaded_t50.empty()) {
            weightCacheT50[layerName] = std::move(loaded_t50);
        }

        return count;
    }
};

} // namespace bitnet
} // namespace sandbox

#endif // BITNET_LOADER_H
