#pragma once

#include <cstdint>
#include <string>

namespace cm {
namespace protocol {

// 10000 注册。tel 与 name 为 UTF-8 文本；password 仅上行，不下发。
struct RegisterRq {
    std::string tel;
    std::string password;
    std::string name;

    RegisterRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10001：失败时 uid=0。
struct RegisterRs {
    std::uint32_t code;
    std::uint64_t uid;

    RegisterRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10002 登录。
struct LoginRq {
    std::string tel;
    std::string password;

    LoginRq();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

// 10003：只回昵称，绝不回显哈希/salt。
struct LoginRs {
    std::uint32_t code;
    std::uint64_t uid;
    std::string name;

    LoginRs();
    bool encode(std::string* out) const;
    bool decode(const std::string& payload);
};

}  // namespace protocol
}  // namespace cm
