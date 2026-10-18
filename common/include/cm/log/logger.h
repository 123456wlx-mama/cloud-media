#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cm/base/error_code.h"
#include "cm/log/log_config.h"
#include "cm/log/log_level.h"

namespace cm {

class Logger {
public:
    using ClockFn = std::function<std::chrono::system_clock::time_point()>;

    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& instance();

    ErrorCode init(const LogConfig& cfg, std::string* err);
    void shutdown();
    void flush();

    void setLevel(LogLevel level);
    bool enabled(LogLevel level) const;

    void log(LogLevel level, const char* file, int line, const std::string& msg);

    void setClockForTest(ClockFn clock);

    static void setTraceId(const std::string& trace);
    static std::string traceId();

private:
    void workerLoop();
    void writeBatch(std::vector<std::string>& batch);
    void writeLine(const std::string& line);
    bool rotate(const char* reason);
    bool openFile();
    void closeFile();
    std::string formatLine(LogLevel level, const char* file, int line,
                           const std::string& msg) const;
    bool ensureParentDir(const std::string& path) const;
    static std::string baseName(const char* path);
    static std::uint32_t currentThreadId();

    std::string path_;
    std::string lastDate_;
    ClockFn clock_;
    std::ofstream file_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable flushCv_;
    std::thread worker_;

    std::vector<std::string> front_;
    std::vector<std::string> back_;
    std::size_t pendingBytes_;
    std::size_t maxSizeBytes_;
    std::size_t maxPendingBytes_;
    std::size_t maxLineBytes_;
    std::size_t fileBytes_;

    int rollDays_;
    int flushIntervalMs_;
    bool console_;
    bool fsyncOnFatal_;
    bool async_;
    bool running_;
    bool stop_;

    std::uint64_t flushSeq_;
    std::uint64_t flushDoneSeq_;
    std::atomic<int> level_;
    std::atomic<std::uint64_t> dropped_;
};

}  // namespace cm
