#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cm/base/error_code.h"
#include "cm/log/logger.h"

namespace {

std::uintmax_t FileSize(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary | std::ios::ate);
    if (!in) {
        return 0;
    }
    std::streamoff size = in.tellg();
    return size > 0 ? static_cast<std::uintmax_t>(size) : 0;
}

bool RunCase(const std::string& mode, int threads, std::uint64_t count,
             const std::string& basePath) {
    const std::string path = basePath + ".t" + std::to_string(threads) +
                             "." + mode + ".log";
    std::remove(path.c_str());

    cm::LogConfig cfg;
    cfg.path = path;
    cfg.level = cm::LogLevel::Info;
    cfg.max_size_bytes = 1024ull * 1024ull * 1024ull;
    cfg.roll_days = 7;
    cfg.async = (mode == "async");
    cfg.console = false;
    cfg.flush_interval_ms = 1000;
    cfg.max_pending_bytes = 512ull * 1024ull * 1024ull;
    cfg.fsync_on_fatal = false;
    cfg.max_line_bytes = 8192;

    cm::Logger logger;
    std::string err;
    cm::ErrorCode result = logger.init(cfg, &err);
    if (result != cm::ErrorCode::Ok) {
        std::cerr << "logger init failed: " << cm::to_string(result)
                  << " " << err << "\n";
        return false;
    }

    const std::string message =
        "benchmark payload 0123456789abcdefghijklmnopqrstuvwxyz"
        "0123456789abcdefghijklmnopqrstuvwxyz";
    const std::uint64_t base = count / static_cast<std::uint64_t>(threads);
    const std::uint64_t remainder = count % static_cast<std::uint64_t>(threads);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();

    for (int t = 0; t < threads; ++t) {
        const std::uint64_t lines =
            base + (static_cast<std::uint64_t>(t) < remainder ? 1 : 0);
        workers.push_back(std::thread([&logger, lines, &message]() {
            for (std::uint64_t i = 0; i < lines; ++i) {
                logger.log(cm::LogLevel::Info, __FILE__, __LINE__, message);
            }
        }));
    }

    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }

    logger.shutdown();
    const std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();

    const double elapsedMs =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                .count()) /
        1000.0;
    const double linesPerSec =
        elapsedMs > 0.0 ? static_cast<double>(count) * 1000.0 / elapsedMs : 0.0;

    std::cout << mode << "," << threads << "," << count << ","
              << std::fixed << std::setprecision(3) << elapsedMs << ","
              << linesPerSec << "," << FileSize(path) << "\n";
    return true;
}

void Usage() {
    std::cout
        << "usage: cm_log_bench [--matrix] [--count N] [--threads N]\n"
        << "                     [--sync|--async] [--file BASE]\n"
        << "  default: --matrix --count 1000000 --file build/log_bench\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t count = 1000000;
    int threads = 1;
    bool threadsSet = false;
    bool syncFlag = false;
    bool asyncFlag = false;
    bool matrix = false;
    std::string basePath = "build/log_bench";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            Usage();
            return 0;
        }
        if (arg == "--matrix") {
            matrix = true;
            continue;
        }
        if (arg == "--sync") {
            syncFlag = true;
            continue;
        }
        if (arg == "--async") {
            asyncFlag = true;
            continue;
        }
        if (arg == "--count" && i + 1 < argc) {
            char* end = NULL;
            const unsigned long long value =
                std::strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value == 0) {
                std::cerr << "invalid --count\n";
                return 1;
            }
            count = static_cast<std::uint64_t>(value);
            continue;
        }
        if (arg == "--threads" && i + 1 < argc) {
            char* end = NULL;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 256) {
                std::cerr << "invalid --threads (1..256)\n";
                return 1;
            }
            threads = static_cast<int>(value);
            threadsSet = true;
            continue;
        }
        if (arg == "--file" && i + 1 < argc) {
            basePath = argv[++i];
            continue;
        }
        std::cerr << "unknown argument: " << arg << "\n";
        Usage();
        return 1;
    }

    if (syncFlag && asyncFlag) {
        std::cerr << "--sync and --async cannot both be set\n";
        return 1;
    }

    if (!matrix && !syncFlag && !asyncFlag && !threadsSet) {
        matrix = true;
    }

    std::cout << "mode,threads,count,elapsed_ms,lines_per_sec,file_bytes\n";

    if (matrix) {
        if (!RunCase("sync", 1, count, basePath)) {
            return 1;
        }
        if (!RunCase("async", 1, count, basePath)) {
            return 1;
        }
        if (!RunCase("sync", 8, count, basePath)) {
            return 1;
        }
        if (!RunCase("async", 8, count, basePath)) {
            return 1;
        }
        return 0;
    }

    const std::string mode = syncFlag ? "sync" : "async";
    return RunCase(mode, threads, count, basePath) ? 0 : 1;
}
