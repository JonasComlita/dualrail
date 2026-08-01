#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace trit::system_benchmark {

inline constexpr int kWarmups = 2;
inline constexpr int kIterations = 7;
inline constexpr double kMaximumAcceptedCv = 0.03;

struct TimingSummary {
    double median = 0.0;
    double mean = 0.0;
    double standard_deviation = 0.0;
    double coefficient_of_variation = 0.0;
    bool stable = false;
};

inline TimingSummary summarizeTiming(const std::vector<double>& samples) {
    TimingSummary result;
    if (samples.empty()) return result;
    std::vector<double> ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    result.median = ordered[ordered.size() / 2];
    if (ordered.size() % 2 == 0) {
        result.median = (ordered[ordered.size() / 2 - 1] +
                         ordered[ordered.size() / 2]) / 2.0;
    }
    result.mean = std::accumulate(ordered.begin(), ordered.end(), 0.0) /
                  static_cast<double>(ordered.size());
    double squared = 0.0;
    for (double sample : ordered) {
        const double delta = sample - result.mean;
        squared += delta * delta;
    }
    result.standard_deviation =
        std::sqrt(squared / static_cast<double>(ordered.size()));
    result.coefficient_of_variation = result.mean > 0.0
        ? result.standard_deviation / result.mean
        : 0.0;
    result.stable = samples.size() == static_cast<std::size_t>(kIterations) &&
                   result.coefficient_of_variation < kMaximumAcceptedCv;
    return result;
}

inline std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch) << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

inline std::string jsonString(const std::string& value) {
    return "\"" + jsonEscape(value) + "\"";
}

inline std::string environmentValue(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

inline bool environmentBool(const char* name, bool fallback = false) {
    const std::string value = environmentValue(name, fallback ? "true" : "false");
    return value == "1" || value == "true" || value == "TRUE" || value == "dirty";
}

inline std::string utcNow() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

inline std::string hostSystem() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "Darwin";
#else
    return "Linux";
#endif
}

inline std::string hostProcessor() {
#ifdef _WIN32
    return environmentValue("PROCESSOR_IDENTIFIER", "unknown");
#else
    return environmentValue("HOSTTYPE", "unknown");
#endif
}

inline std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

inline std::uint64_t fnv1a64(const std::vector<long long>& values) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (long long value : values) {
        std::uint64_t encoded = static_cast<std::uint64_t>(value);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (encoded >> (byte * 8)) & 0xffULL;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

inline bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) return false;
    output << text;
    return output.good();
}

inline void writeIntArray(std::ostream& out, const std::vector<int>& values) {
    out << "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out << ", ";
        out << values[index];
    }
    out << "]";
}
inline void writeSamples(std::ostream& out, const std::vector<double>& samples) {
    out << "[";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (index != 0) out << ", ";
        out << std::setprecision(12) << samples[index];
    }
    out << "]";
}

inline void writeTiming(std::ostream& out,
                        const std::string& scope,
                        double bootstrap_seconds,
                        const std::vector<double>& samples) {
    const TimingSummary summary = summarizeTiming(samples);
    out << "\"timing\": {\n"
        << "    \"unit\": \"seconds\",\n"
        << "    \"scope\": " << jsonString(scope) << ",\n"
        << "    \"bootstrap_seconds\": " << std::setprecision(12)
        << bootstrap_seconds << ",\n"
        << "    \"warmups\": " << kWarmups << ",\n"
        << "    \"iterations\": " << kIterations << ",\n"
        << "    \"samples\": ";
    writeSamples(out, samples);
    out << ",\n"
        << "    \"median\": " << summary.median << ",\n"
        << "    \"mean\": " << summary.mean << ",\n"
        << "    \"standard_deviation\": " << summary.standard_deviation << ",\n"
        << "    \"coefficient_of_variation\": " << summary.coefficient_of_variation << ",\n"
        << "    \"maximum_accepted_cv\": " << kMaximumAcceptedCv << ",\n"
        << "    \"stable\": " << (summary.stable ? "true" : "false") << "\n"
        << "  }";
}

} // namespace trit::system_benchmark