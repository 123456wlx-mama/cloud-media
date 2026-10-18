#pragma once

#include <cstddef>
#include <string>

#include "cm/log/log_level.h"

namespace cm {

struct LogConfig {
    std::string path;
    LogLevel level;
    std::size_t max_size_bytes;
    int roll_days;
    bool async;
    bool console;
    int flush_interval_ms;
    std::size_t max_pending_bytes;
    bool fsync_on_fatal;
    std::size_t max_line_bytes;

    LogConfig()
        : path("logs/server.log"),
          level(LogLevel::Info),
          max_size_bytes(100 * 1024 * 1024),
          roll_days(7),
          async(true),
          console(false),
          flush_interval_ms(1000),
          max_pending_bytes(64 * 1024 * 1024),
          fsync_on_fatal(true),
          max_line_bytes(8192) {}
};

}  // namespace cm
