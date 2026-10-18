#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cm {

// 可变长字节缓冲：只负责字节读写，不做协议解析。
// peek/data 返回的指针在下一次 append/retrieve/compact/reset 后失效。
class Buffer {
public:
    static const std::size_t kMaxBytes;

    Buffer();

    bool append(const char* data, std::size_t len);
    bool append(const std::string& data);

    const char* peek() const;
    const char* data() const;

    std::size_t readableBytes() const;
    std::size_t writableBytes() const;

    bool retrieve(std::size_t len);
    std::string retrieveAsString(std::size_t len);
    void retrieveAll();

    bool ensureWritable(std::size_t len);
    void compact();
    void reset();

private:
    std::vector<char> storage_;
    std::size_t readerIndex_;
    std::size_t writerIndex_;
};

}  // namespace cm
