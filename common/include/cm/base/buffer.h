#pragma once

#include <cstddef>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>  // ssize_t
#endif

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
    std::string retrieveAllAsString();
    void retrieveAll();

    bool ensureWritable(std::size_t len);
    void compact();
    void reset();

#ifndef _WIN32
    // 从 fd 读取到缓冲（readv 双段：先写满内部可写区，再写 64KB 栈上 extra）。
    // 返回值：>0 读取字节数；0 对端关闭；-1 出错（errno 存入 *savedErrno）。
    // 仅供服务端 epoll 读路径使用，Windows 下不编译。
    ssize_t readFd(int fd, int* savedErrno);
#endif

private:
    std::vector<char> storage_;
    std::size_t readerIndex_;
    std::size_t writerIndex_;
};

}  // namespace cm
