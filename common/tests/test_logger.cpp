#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cm/base/error_code.h"
#include "cm/log/logger.h"

namespace {

std::string Join(const std::string& dir, const std::string& name) {
    return dir + "/" + name;
}

bool EnsureDir(const std::string& path) {
#ifdef _WIN32
    int rc = _mkdir(path.c_str());
#else
    int rc = mkdir(path.c_str(), 0700);
#endif
    return rc == 0 || errno == EEXIST;
}

std::vector<std::string> ListFiles(const std::string& dir) {
    std::vector<std::string> files;
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA((dir + "/*").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        return files;
    }
    do {
        std::string name = data.cFileName;
        if (name != "." && name != "..") {
            files.push_back(name);
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return files;
    }
    while (dirent* entry = readdir(handle)) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            files.push_back(name);
        }
    }
    closedir(handle);
#endif
    return files;
}

void RemoveTree(const std::string& dir) {
    std::vector<std::string> files = ListFiles(dir);
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::remove(Join(dir, files[i]).c_str());
    }
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}

std::string ReadAll(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

std::size_t CountLines(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++count;
        }
    }
    return count;
}

std::string MakeTempDir(const std::string& tag) {
    static std::atomic<unsigned> counter(0);
    std::ostringstream os;
    os << "cm_logger_" << tag << "_" << std::time(nullptr) << "_" << counter++;
    std::string dir = os.str();
    EnsureDir(dir);
    return dir;
}

cm::LogConfig MakeConfig(const std::string& dir, const std::string& name) {
    cm::LogConfig cfg;
    cfg.path = Join(dir, name);
    cfg.level = cm::LogLevel::Info;
    cfg.max_size_bytes = 100 * 1024 * 1024;
    cfg.roll_days = 7;
    cfg.async = true;
    cfg.console = false;
    cfg.flush_interval_ms = 5;
    cfg.max_pending_bytes = 64 * 1024 * 1024;
    cfg.fsync_on_fatal = false;
    cfg.max_line_bytes = 8192;
    return cfg;
}

}  // namespace

TEST(LoggerTest, LevelFiltering) {
    std::string dir = MakeTempDir("level");
    cm::LogConfig cfg = MakeConfig(dir, "level.log");
    cfg.level = cm::LogLevel::Warn;

    cm::Logger logger;
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);

    logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "info-line");
    logger.log(cm::LogLevel::Error, __FILE__, __LINE__, "error-line");
    logger.shutdown();

    std::string text = ReadAll(cfg.path);
    EXPECT_EQ(std::string::npos, text.find("info-line"));
    EXPECT_NE(std::string::npos, text.find("error-line"));
    RemoveTree(dir);
}

TEST(LoggerTest, FormatContainsFields) {
    std::string dir = MakeTempDir("format");
    cm::LogConfig cfg = MakeConfig(dir, "format.log");

    cm::Logger::setTraceId("abc123");
    cm::Logger logger;
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);
    logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "hello-format");
    logger.shutdown();

    std::string text = ReadAll(cfg.path);
    EXPECT_NE(std::string::npos, text.find("INFO"));
    EXPECT_NE(std::string::npos, text.find("tid="));
    EXPECT_NE(std::string::npos, text.find("test_logger.cpp:"));
    EXPECT_NE(std::string::npos, text.find("trace=abc123"));
    EXPECT_NE(std::string::npos, text.find("hello-format"));
    cm::Logger::setTraceId("");
    RemoveTree(dir);
}

TEST(LoggerTest, Concurrent100ThreadsNoLoss) {
    std::string dir = MakeTempDir("concurrent");
    cm::LogConfig cfg = MakeConfig(dir, "concurrent.log");

    cm::Logger logger;
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);

    const int threadCount = 100;
    const int perThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.push_back(std::thread([&logger, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                logger.log(cm::LogLevel::Info, __FILE__, __LINE__,
                           "t=" + std::to_string(t) + " i=" + std::to_string(i));
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }
    logger.shutdown();

    std::string text = ReadAll(cfg.path);
    EXPECT_EQ(static_cast<std::size_t>(threadCount * perThread), CountLines(text));
    RemoveTree(dir);
}

TEST(LoggerTest, SizeRollingKeepsAllLines) {
    std::string dir = MakeTempDir("size");
    cm::LogConfig cfg = MakeConfig(dir, "size.log");
    cfg.max_size_bytes = 2048;

    cm::Logger logger;
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);
    for (int i = 0; i < 200; ++i) {
        logger.log(cm::LogLevel::Info, __FILE__, __LINE__, std::string(80, 'x'));
    }
    logger.shutdown();

    std::vector<std::string> files = ListFiles(dir);
    int logFiles = 0;
    std::size_t lines = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].find("size.log") == 0) {
            ++logFiles;
            lines += CountLines(ReadAll(Join(dir, files[i])));
        }
    }
    EXPECT_GT(logFiles, 1);
    EXPECT_EQ(static_cast<std::size_t>(200), lines);
    RemoveTree(dir);
}

TEST(LoggerTest, DateRollingCreatesNewFile) {
    std::string dir = MakeTempDir("date");
    cm::LogConfig cfg = MakeConfig(dir, "date.log");
    std::time_t fakeNow = 1704067200;

    cm::Logger logger;
    logger.setClockForTest([&fakeNow]() {
        return std::chrono::system_clock::from_time_t(fakeNow);
    });
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);

    for (int i = 0; i < 5; ++i) {
        logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "day-one");
    }
    logger.flush();

    fakeNow += 86400;
    for (int i = 0; i < 5; ++i) {
        logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "day-two");
    }
    logger.flush();
    logger.shutdown();

    std::vector<std::string> files = ListFiles(dir);
    int logFiles = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].find("date.log") == 0) {
            ++logFiles;
        }
    }
    EXPECT_GT(logFiles, 1);
    RemoveTree(dir);
}

TEST(LoggerTest, FlushMakesLineVisible) {
    std::string dir = MakeTempDir("flush");
    cm::LogConfig cfg = MakeConfig(dir, "flush.log");

    cm::Logger logger;
    std::string err;
    ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);
    logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "flush-line");
    logger.flush();

    std::string text = ReadAll(cfg.path);
    EXPECT_NE(std::string::npos, text.find("flush-line"));
    logger.shutdown();
    RemoveTree(dir);
}

TEST(LoggerTest, DestructorFlushes) {
    std::string dir = MakeTempDir("dtor");
    cm::LogConfig cfg = MakeConfig(dir, "dtor.log");
    {
        cm::Logger logger;
        std::string err;
        ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);
        logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "dtor-line");
    }

    std::string text = ReadAll(cfg.path);
    EXPECT_NE(std::string::npos, text.find("dtor-line"));
    RemoveTree(dir);
}

TEST(LoggerTest, SyncAndAsyncWriteSameCount) {
    for (int mode = 0; mode < 2; ++mode) {
        std::string dir = MakeTempDir(mode == 0 ? "sync" : "async");
        cm::LogConfig cfg = MakeConfig(dir, mode == 0 ? "sync.log" : "async.log");
        cfg.async = (mode != 0);

        cm::Logger logger;
        std::string err;
        ASSERT_TRUE(logger.init(cfg, &err) == cm::ErrorCode::Ok);
        for (int i = 0; i < 100; ++i) {
            logger.log(cm::LogLevel::Info, __FILE__, __LINE__, "same-count");
        }
        logger.shutdown();

        EXPECT_EQ(static_cast<std::size_t>(100), CountLines(ReadAll(cfg.path)));
        RemoveTree(dir);
    }
}
