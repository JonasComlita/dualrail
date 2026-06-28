#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tests_next {

struct TestCase {
    std::string id;
    std::string claim;
    std::function<void(class TestContext&)> run;
};

class TestContext {
public:
    TestContext(std::string id, std::string claim)
        : id_(std::move(id)), claim_(std::move(claim)) {}

    [[nodiscard]] const std::string& id() const { return id_; }
    [[nodiscard]] const std::string& claim() const { return claim_; }
    [[nodiscard]] int failures() const { return failures_; }

    void check(bool condition, const std::string& message) {
        if (condition) return;
        fail(message);
    }

    template <typename Got, typename Want>
    void equal(const Got& got, const Want& want, const std::string& message) {
        if (got == want) return;
        std::ostringstream out;
        out << message << " got=" << got << " want=" << want;
        fail(out.str());
    }

    void contains(const std::string& haystack,
                  const std::string& needle,
                  const std::string& message) {
        if (haystack.find(needle) != std::string::npos) return;
        fail(message + " missing='" + needle + "'");
    }

    void fail(const std::string& message) {
        ++failures_;
        messages_.push_back(message);
    }

    void printFailures() const {
        for (const auto& message : messages_) {
            std::cout << "FAIL " << id_ << ": " << message << "\n";
        }
    }

private:
    std::string id_;
    std::string claim_;
    int failures_ = 0;
    std::vector<std::string> messages_;
};

inline int runCases(const std::string& suite, const std::vector<TestCase>& cases) {
    int failures = 0;
    std::cout << "[suite] " << suite << "\n";
    for (const TestCase& test : cases) {
        TestContext ctx(test.id, test.claim);
        std::cout << "[run] " << test.id << " -> " << test.claim << "\n";
        try {
            test.run(ctx);
        } catch (const std::exception& ex) {
            ctx.fail(std::string("uncaught exception: ") + ex.what());
        } catch (...) {
            ctx.fail("uncaught non-standard exception");
        }

        if (ctx.failures() == 0) {
            std::cout << "[ok] " << test.id << "\n";
        } else {
            ctx.printFailures();
            failures += ctx.failures();
        }
    }

    if (failures == 0) {
        std::cout << "[suite-ok] " << suite << "\n";
        return 0;
    }
    std::cout << "[suite-fail] " << suite << " failures=" << failures << "\n";
    return 1;
}

inline std::uint64_t deterministicSeed(const std::string& id,
                                       std::uint64_t salt = 0x545249544E455854ULL) {
    std::uint64_t hash = 1469598103934665603ULL ^ salt;
    for (unsigned char ch : id) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::mt19937_64 deterministicRng(const std::string& id) {
    return std::mt19937_64(deterministicSeed(id));
}

class TempWorkspace {
public:
    explicit TempWorkspace(const std::string& prefix = "trit_next") {
        static int sequence = 0;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                (prefix + "_" + std::to_string(now) + "_" + std::to_string(++sequence));
        std::filesystem::create_directories(root_);
    }

    TempWorkspace(const TempWorkspace&) = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    ~TempWorkspace() {
        std::error_code ec;
        if (!root_.empty()) std::filesystem::remove_all(root_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return root_; }

private:
    std::filesystem::path root_;
};

inline std::filesystem::path goldenPath(const std::filesystem::path& repo_root,
                                        const std::string& relative) {
    return repo_root / "tests_next" / "goldens" / relative;
}

inline std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

} // namespace tests_next
