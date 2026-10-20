#include "cli_common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cm/base/md5.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/auth.h"
#include "cm/protocol/serialize.h"

namespace cm {
namespace cli {
namespace {

const int kIoSocketTimeoutSec = 30;
const std::size_t kReadChunk = 64 * 1024;
const std::size_t kMd5Buffer = 1024 * 1024;

bool writeAll(int fd, const char* data, std::size_t len, std::string* error) {
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = std::string("write failed: ") + std::strerror(errno);
            }
            return false;
        }
        if (n == 0) {
            if (error != nullptr) {
                *error = "write returned 0";
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

int tcpConnect(const std::string& host, int port, std::string* error) {
    char service[16];
    std::snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), service, &hints, &result);
    if (gai != 0) {
        if (error != nullptr) {
            *error = std::string("resolve failed: ") + ::gai_strerror(gai);
        }
        return -1;
    }

    int fd = -1;
    std::string lastError = "connect failed";
    for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        lastError = std::string("connect failed: ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    if (fd < 0) {
        if (error != nullptr) {
            *error = lastError;
        }
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = kIoSocketTimeoutSec;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

}  // namespace

int connectAndLogin(const std::string& host,
                    int port,
                    const std::string& tel,
                    const std::string& password,
                    std::string* error,
                    int* exitCode) {
    if (exitCode != nullptr) {
        *exitCode = kExitNetwork;
    }
    const int fd = tcpConnect(host, port, error);
    if (fd < 0) {
        return -1;
    }

    protocol::LoginRq rq;
    rq.tel = tel;
    rq.password = password;

    std::string payload;
    if (!rq.encode(&payload)) {
        if (error != nullptr) {
            *error = "invalid login arguments";
        }
        if (exitCode != nullptr) {
            *exitCode = kExitUsage;
        }
        ::close(fd);
        return -1;
    }
    if (!sendFrame(fd, static_cast<std::uint16_t>(protocol::MessageType::LoginRq),
                   1, payload, error)) {
        ::close(fd);
        return -1;
    }

    cm::Buffer buffer;
    protocol::Frame frame;
    if (!recvFrame(fd, &buffer, &frame, error)) {
        ::close(fd);
        return -1;
    }

    if (frame.type ==
        static_cast<std::uint16_t>(protocol::MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        decodeErrorRs(frame, &code, &message);
        if (error != nullptr) {
            *error = "login rejected: code=" + std::to_string(code) +
                     (message.empty() ? "" : " " + message);
        }
        if (exitCode != nullptr) {
            *exitCode = kExitUsage;
        }
        ::close(fd);
        return -1;
    }

    protocol::LoginRs rs;
    if (frame.type !=
            static_cast<std::uint16_t>(protocol::MessageType::LoginRs) ||
        !rs.decode(frame.payload)) {
        if (error != nullptr) {
            *error = "unexpected login response";
        }
        ::close(fd);
        return -1;
    }
    if (rs.code != 0) {
        if (error != nullptr) {
            *error = "login rejected: code=" + std::to_string(rs.code);
        }
        if (exitCode != nullptr) {
            *exitCode = kExitUsage;
        }
        ::close(fd);
        return -1;
    }
    return fd;
}

bool sendFrame(int fd,
               std::uint16_t type,
               std::uint32_t seq,
               const std::string& payload,
               std::string* error) {
    std::string frame;
    if (protocol::Codec::encode(type, seq, payload, &frame) != ErrorCode::Ok) {
        if (error != nullptr) {
            *error = "encode frame failed";
        }
        return false;
    }
    return writeAll(fd, frame.data(), frame.size(), error);
}

bool recvFrame(int fd,
               cm::Buffer* buffer,
               cm::protocol::Frame* frame,
               std::string* error) {
    if (buffer == nullptr || frame == nullptr) {
        if (error != nullptr) {
            *error = "null buffer";
        }
        return false;
    }

    for (;;) {
        protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
        const ErrorCode rc = protocol::Codec::decode(*buffer, frame, &status);
        if (rc == ErrorCode::Ok) {
            return true;
        }
        if (rc != ErrorCode::NeedMore) {
            if (error != nullptr) {
                *error = "bad frame (status=" +
                         std::to_string(static_cast<int>(status)) + ")";
            }
            return false;
        }

        char chunk[kReadChunk];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            if (!buffer->append(chunk, static_cast<std::size_t>(n))) {
                if (error != nullptr) {
                    *error = "receive buffer overflow";
                }
                return false;
            }
            continue;
        }
        if (n == 0) {
            if (error != nullptr) {
                *error = "connection closed by peer";
            }
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (error != nullptr) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                *error = "receive timeout";
            } else {
                *error = std::string("read failed: ") + std::strerror(errno);
            }
        }
        return false;
    }
}

bool decodeErrorRs(const cm::protocol::Frame& frame,
                   std::uint32_t* code,
                   std::string* message) {
    protocol::Reader reader(frame.payload);
    std::string text;
    std::uint32_t value = 0;
    if (!reader.readU32(&value) || !reader.readString(&text)) {
        return false;
    }
    if (code != nullptr) {
        *code = value;
    }
    if (message != nullptr) {
        *message = text;
    }
    return true;
}

std::string passwordFromEnv() {
    const char* value = std::getenv("CM_CLI_PASSWORD");
    return value != nullptr ? std::string(value) : std::string();
}

bool parseU64(const std::string& text, std::uint64_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *value = static_cast<std::uint64_t>(v);
    return true;
}

bool parseU32(const std::string& text, std::uint32_t* value) {
    std::uint64_t v = 0;
    if (!parseU64(text, &v) || v > 0xFFFFFFFFULL) {
        return false;
    }
    *value = static_cast<std::uint32_t>(v);
    return true;
}

bool parsePositiveInt(const std::string& text, int* value) {
    std::uint64_t v = 0;
    if (!parseU64(text, &v) || v == 0 || v > 2147483647ULL) {
        return false;
    }
    *value = static_cast<int>(v);
    return true;
}

std::string baseName(const std::string& path) {
    const std::string::size_type pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string bitmapToHex(const std::string& bitmap) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bitmap.size() * 2);
    for (std::size_t i = 0; i < bitmap.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bitmap[i]);
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

bool bitmapFromHex(const std::string& hex, std::string* bitmap) {
    if (bitmap == nullptr || (hex.size() % 2) != 0) {
        return false;
    }
    bitmap->clear();
    bitmap->reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int high = 0;
        int low = 0;
        const char a = hex[i];
        const char b = hex[i + 1];
        if (a >= '0' && a <= '9') {
            high = a - '0';
        } else if (a >= 'a' && a <= 'f') {
            high = a - 'a' + 10;
        } else {
            return false;
        }
        if (b >= '0' && b <= '9') {
            low = b - '0';
        } else if (b >= 'a' && b <= 'f') {
            low = b - 'a' + 10;
        } else {
            return false;
        }
        bitmap->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

void bitmapSet(std::string* bitmap, std::uint32_t index) {
    if (bitmap == nullptr) {
        return;
    }
    const std::size_t byte = index / 8;
    if (byte >= bitmap->size()) {
        return;
    }
    (*bitmap)[byte] = static_cast<char>(
        static_cast<unsigned char>((*bitmap)[byte]) |
        static_cast<unsigned char>(1u << (index % 8)));
}

bool bitmapTest(const std::string& bitmap, std::uint32_t index) {
    const std::size_t byte = index / 8;
    if (byte >= bitmap.size()) {
        return false;
    }
    return (static_cast<unsigned char>(bitmap[byte]) &
            static_cast<unsigned char>(1u << (index % 8))) != 0;
}

bool writeFileAtomic(const std::string& path,
                     const std::string& data,
                     std::string* error) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "open " + tmp + " failed: " + std::strerror(errno);
        }
        return false;
    }
    bool ok = writeAll(fd, data.data(), data.size(), error);
    if (ok && ::fsync(fd) != 0) {
        ok = false;
        if (error != nullptr) {
            *error = std::string("fsync failed: ") + std::strerror(errno);
        }
    }
    ::close(fd);
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error != nullptr) {
            *error = "rename failed: " + std::string(std::strerror(errno));
        }
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool readFileToString(const std::string& path, std::string* data) {
    if (data == nullptr) {
        return false;
    }
    data->clear();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            data->append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

bool md5File(const std::string& path, std::string* hex, std::string* error) {
    if (hex == nullptr) {
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "open " + path + " failed: " + std::strerror(errno);
        }
        return false;
    }
    std::vector<char> buffer(kMd5Buffer);
    cm::Md5 md5;
    for (;;) {
        const ssize_t n = ::read(fd, &buffer[0], buffer.size());
        if (n > 0) {
            md5.update(&buffer[0], static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (error != nullptr) {
            *error = std::string("read failed: ") + std::strerror(errno);
        }
        ::close(fd);
        return false;
    }
    ::close(fd);
    *hex = md5.hex();
    return true;
}

bool pwriteAll(int fd, const char* data, std::size_t len, off_t offset,
               std::string* error) {
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::pwrite(fd, data + written, len - written,
                                   offset + static_cast<off_t>(written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = std::string("pwrite failed: ") + std::strerror(errno);
            }
            return false;
        }
        if (n == 0) {
            if (error != nullptr) {
                *error = "pwrite returned 0";
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

bool preadExact(int fd, char* data, std::size_t len, off_t offset,
                std::string* error) {
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::pread(fd, data + got, len - got,
                                  offset + static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (error != nullptr) {
                *error = std::string("pread failed: ") + std::strerror(errno);
            }
            return false;
        }
        if (n == 0) {
            if (error != nullptr) {
                *error = "unexpected end of file";
            }
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace cli
}  // namespace cm
