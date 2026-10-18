#pragma once

#include <cctype>
#include <string>

namespace cm {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
    Off = 6
};

inline const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off: return "OFF";
    }
    return "UNKNOWN";
}

inline bool parse_log_level(const std::string& text, LogLevel* out) {
    std::string upper;
    for (std::size_t i = 0; i < text.size(); ++i) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(text[i]))));
    }
    if (upper == "TRACE") { *out = LogLevel::Trace; return true; }
    if (upper == "DEBUG") { *out = LogLevel::Debug; return true; }
    if (upper == "INFO") { *out = LogLevel::Info; return true; }
    if (upper == "WARN") { *out = LogLevel::Warn; return true; }
    if (upper == "ERROR") { *out = LogLevel::Error; return true; }
    if (upper == "FATAL") { *out = LogLevel::Fatal; return true; }
    if (upper == "OFF") { *out = LogLevel::Off; return true; }
    return false;
}

}  // namespace cm
