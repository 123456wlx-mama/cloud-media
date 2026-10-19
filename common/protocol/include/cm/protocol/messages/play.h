#pragma once

#include <cstdint>
#include <string>

namespace cm {
namespace protocol {

// 10032 播放进度上报。
struct PlayReportRq {
    std::uint64_t fId;
    std::uint64_t positionMs;
    std::uint64_t durationMs;
    std::uint8_t finished;

    PlayReportRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10033。
struct PlayReportRs {
    std::uint32_t code;
    std::uint8_t saved;

    PlayReportRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
