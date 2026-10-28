#pragma once

#include <cstddef>
#include <string>

namespace cm {

// 把 A-F 转成小写，其余字符原样保留（调用方应先用十六进制规则校验过）。
//
// 为什么需要它：协议里的 md5 / taskId 是客户端给的，而服务端算出来的一律小写。
// 只要有一处按原样落库或拼路径，大写录入就会让"逐片 MD5 比对"永远不相等，
// 表现为该分片被反复判为坏片、客户端重传也修不好。
inline std::string toLowerHex(const std::string& value) {
    std::string out(value);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c >= 'A' && c <= 'F') {
            out[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

}  // namespace cm
