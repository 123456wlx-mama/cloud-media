#pragma once

#include <chrono>
#include <string>

namespace cm {

std::string format_time_ms(std::chrono::system_clock::time_point tp);
std::string format_date(std::chrono::system_clock::time_point tp);
std::string format_timestamp(std::chrono::system_clock::time_point tp);

}  // namespace cm
