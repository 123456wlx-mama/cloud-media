#include "cm/log/logger.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <utility>

#include "cm/base/time_util.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace cm {
namespace {

thread_local char t_trace[128] = {0};
thread_local std::uint32_t t_tid = 0;
std::atomic<std::uint32_t> g_next_tid(1);

bool fileExists(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return in.good();
}

}  // namespace

Logger::Logger()
    : pendingBytes_(0),
      maxSizeBytes_(100 * 1024 * 1024),
      maxPendingBytes_(64 * 1024 * 1024),
      maxLineBytes_(8192),
      fileBytes_(0),
      rollDays_(7),
      flushIntervalMs_(1000),
      console_(false),
      fsyncOnFatal_(true),
      async_(true),
      running_(false),
      stop_(false),
      flushSeq_(0),
      flushDoneSeq_(0),
      level_(static_cast<int>(LogLevel::Info)),
      dropped_(0) {
    clock_ = []() { return std::chrono::system_clock::now(); };
}

Logger::~Logger() {
    shutdown();
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

ErrorCode Logger::init(const LogConfig& cfg, std::string* err) {
    if (cfg.path.empty() || cfg.max_line_bytes == 0 ||
        cfg.flush_interval_ms <= 0 || cfg.max_pending_bytes == 0) {
        if (err != nullptr) {
            *err = "invalid log config";
        }
        return ErrorCode::InvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        if (err != nullptr) {
            *err = "logger already initialized";
        }
        return ErrorCode::AlreadyExists;
    }

    path_ = cfg.path;
    level_.store(static_cast<int>(cfg.level), std::memory_order_relaxed);
    maxSizeBytes_ = cfg.max_size_bytes;
    maxPendingBytes_ = cfg.max_pending_bytes;
    maxLineBytes_ = cfg.max_line_bytes;
    rollDays_ = cfg.roll_days;
    flushIntervalMs_ = cfg.flush_interval_ms;
    console_ = cfg.console;
    fsyncOnFatal_ = cfg.fsync_on_fatal;
    async_ = cfg.async;
    pendingBytes_ = 0;
    fileBytes_ = 0;
    flushSeq_ = 0;
    flushDoneSeq_ = 0;
    stop_ = false;
    front_.clear();
    back_.clear();
    dropped_.store(0, std::memory_order_relaxed);

    if (!ensureParentDir(path_)) {
        if (err != nullptr) {
            *err = "cannot create log directory: " + path_;
        }
        return ErrorCode::PermissionDenied;
    }
    if (!openFile()) {
        if (err != nullptr) {
            *err = "cannot open log file: " + path_;
        }
        return ErrorCode::PermissionDenied;
    }

    lastDate_ = format_date(clock_());
    running_ = true;
    if (async_) {
        worker_ = std::thread(&Logger::workerLoop, this);
    }

    if (err != nullptr) {
        err->clear();
    }
    return ErrorCode::Ok;
}

void Logger::shutdown() {
    if (!async_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        running_ = false;
        return;
    }

    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stop_ = true;
        cv_.notify_all();
        worker.swap(worker_);
    }
    if (worker.joinable()) {
        worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
}

void Logger::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    if (!async_) {
        if (file_.is_open()) {
            file_.flush();
        }
        return;
    }
    std::uint64_t target = ++flushSeq_;
    cv_.notify_all();
    flushCv_.wait(lock, [this, target]() {
        return flushDoneSeq_ >= target || !running_;
    });
}

void Logger::setLevel(LogLevel level) {
    level_.store(static_cast<int>(level), std::memory_order_relaxed);
}

bool Logger::enabled(LogLevel level) const {
    int configured = level_.load(std::memory_order_relaxed);
    if (configured == static_cast<int>(LogLevel::Off)) {
        return false;
    }
    return static_cast<int>(level) >= configured;
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& msg) {
    if (!enabled(level)) {
        return;
    }

    std::string formatted = formatLine(level, file, line, msg);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }

    if (!async_) {
        // 同步模式在 flush/shutdown 落盘，避免每行 fflush 扭曲基准。
        writeLine(formatted);
        return;
    }

    std::size_t addBytes = formatted.size() + 1;
    if (maxPendingBytes_ > 0 && pendingBytes_ + addBytes > maxPendingBytes_) {
        if (level == LogLevel::Trace || level == LogLevel::Debug) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (pendingBytes_ + addBytes > maxPendingBytes_ * 4 && level != LogLevel::Fatal) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    front_.push_back(std::move(formatted));
    pendingBytes_ += addBytes;
    if (front_.size() >= 256 || pendingBytes_ >= 64 * 1024) {
        cv_.notify_one();
    }
}

void Logger::setClockForTest(ClockFn clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = clock;
}

void Logger::setTraceId(const std::string& trace) {
    std::memset(t_trace, 0, sizeof(t_trace));
    std::size_t count = trace.size() < sizeof(t_trace) - 1 ? trace.size() : sizeof(t_trace) - 1;
    std::memcpy(t_trace, trace.data(), count);
}

std::string Logger::traceId() {
    return std::string(t_trace);
}

void Logger::workerLoop() {
    for (;;) {
        std::vector<std::string> batch;
        bool stop = false;
        bool needFlush = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(flushIntervalMs_), [this]() {
                return stop_ || !front_.empty() || flushSeq_ > flushDoneSeq_;
            });
            stop = stop_;
            batch.swap(front_);
            needFlush = (flushSeq_ > flushDoneSeq_) || stop;
            if (batch.empty() && !stop && !needFlush) {
                continue;
            }
        }

        writeBatch(batch);

        if (stop) {
            std::vector<std::string> rest;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rest.swap(front_);
            }
            writeBatch(rest);
            if (file_.is_open()) {
                file_.flush();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                flushDoneSeq_ = flushSeq_;
                flushCv_.notify_all();
            }
            break;
        }

        bool pendingFlush = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingFlush = flushSeq_ > flushDoneSeq_;
        }
        if (needFlush || pendingFlush) {
            if (file_.is_open()) {
                file_.flush();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            flushDoneSeq_ = flushSeq_;
            flushCv_.notify_all();
        }
    }

    closeFile();
}

void Logger::writeBatch(std::vector<std::string>& batch) {
    if (batch.empty()) {
        return;
    }

    std::size_t bytes = 0;
    for (std::size_t i = 0; i < batch.size(); ++i) {
        writeLine(batch[i]);
        bytes += batch[i].size() + 1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingBytes_ > bytes) {
        pendingBytes_ -= bytes;
    } else {
        pendingBytes_ = 0;
    }
    batch.clear();
}

void Logger::writeLine(const std::string& line) {
    if (!file_.is_open()) {
        return;
    }

    std::string today = format_date(clock_());
    if (lastDate_ != today) {
        if (!rotate("date")) {
            return;
        }
    }

    std::size_t lineBytes = line.size() + 1;
    if (maxSizeBytes_ > 0 && fileBytes_ + lineBytes > maxSizeBytes_) {
        if (!rotate("size")) {
            return;
        }
    }

    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
    file_.put('\n');
    fileBytes_ += lineBytes;

    if (console_) {
        std::cout << line << '\n';
        std::cout.flush();
    }
}

bool Logger::rotate(const char* reason) {
    (void)reason;
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }

    std::string stamp = format_timestamp(clock_());
    std::string target;
    for (int n = 1; n < 100000; ++n) {
        std::ostringstream os;
        os << path_ << "." << stamp << "." << n;
        target = os.str();
        if (!fileExists(target)) {
            break;
        }
    }
    std::rename(path_.c_str(), target.c_str());

    if (!openFile()) {
        return false;
    }
    lastDate_ = format_date(clock_());
    return true;
}

bool Logger::openFile() {
    file_.open(path_.c_str(), std::ios::out | std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        return false;
    }
    std::streampos pos = file_.tellp();
    fileBytes_ = (pos == std::streampos(-1)) ? 0 : static_cast<std::size_t>(pos);
    return true;
}

void Logger::closeFile() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

std::string Logger::formatLine(LogLevel level, const char* file, int line,
                               const std::string& msg) const {
    std::string levelText = level_name(level);
    if (levelText.size() < 5) {
        levelText.append(5 - levelText.size(), ' ');
    }

    std::string raw;
    raw.reserve(256);
    raw += format_time_ms(clock_());
    raw += " | ";
    raw += levelText;
    raw += " | tid=";
    raw += std::to_string(currentThreadId());
    raw += " | ";
    raw += baseName(file);
    raw += ":";
    raw += std::to_string(line);
    raw += " | trace=";
    const std::string currentTrace = traceId();
    raw += currentTrace.empty() ? "-" : currentTrace;
    raw += " | ";
    raw += msg;

    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n' || raw[i] == '\r') {
            out += "\\n";
        } else {
            out.push_back(raw[i]);
        }
    }
    if (maxLineBytes_ > 0 && out.size() > maxLineBytes_) {
        out.resize(maxLineBytes_);
        out += "...(truncated)";
    }
    return out;
}

bool Logger::ensureParentDir(const std::string& path) const {
    std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos || pos == 0) {
        return true;
    }
    std::string dir = path.substr(0, pos);
#ifdef _WIN32
    int rc = _mkdir(dir.c_str());
#else
    int rc = mkdir(dir.c_str(), 0700);
#endif
    return rc == 0 || errno == EEXIST;
}

std::string Logger::baseName(const char* path) {
    if (path == nullptr) {
        return std::string();
    }
    const char* slash = std::strrchr(path, '/');
    const char* backslash = std::strrchr(path, '\\');
    const char* last = slash;
    if (backslash != nullptr && (last == nullptr || backslash > last)) {
        last = backslash;
    }
    return last == nullptr ? std::string(path) : std::string(last + 1);
}

std::uint32_t Logger::currentThreadId() {
    if (t_tid == 0) {
        t_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    }
    return t_tid;
}

}  // namespace cm
