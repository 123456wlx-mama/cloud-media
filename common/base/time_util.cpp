#include "cm/base/time_util.h"

#include <cstdio>
#include <ctime>

namespace cm {
namespace {

std::tm local_tm(std::time_t t) {
    std::tm tm_value;
#ifdef _WIN32
    localtime_s(&tm_value, &t);
#else
    localtime_r(&t, &tm_value);
#endif
    return tm_value;
}

}  // namespace

std::string format_time_ms(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_value = local_tm(t);
    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm_value);

    std::chrono::milliseconds ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", base, static_cast<int>(ms.count()));
    return std::string(out);
}

std::string format_date(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_value = local_tm(t);
    char out[16];
    std::strftime(out, sizeof(out), "%Y%m%d", &tm_value);
    return std::string(out);
}

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_value = local_tm(t);
    char out[24];
    std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &tm_value);
    return std::string(out);
}

}  // namespace cm
