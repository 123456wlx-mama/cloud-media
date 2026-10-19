#include "logic/block_crc.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"

namespace cm {
namespace logic {
namespace {

const char kMagic[5] = {'C', 'M', 'B', 'C', '1'};
const std::uint8_t kVersion = 1;
const std::size_t kIoBufferSize = 1024 * 1024;

// 懒填充只发生在 sidecar 缺失/损坏的慢路径；单进程串行化即可避免
// “建壳 + ftruncate”与另一线程 pwrite 的竞态。多进程不在本期范围。
std::mutex g_lazyFillMutex;

std::uint32_t blockCountFor(std::uint64_t size) {
    return static_cast<std::uint32_t>(
        (size + kBlockCrcBlockSize - 1) / kBlockCrcBlockSize);
}

std::size_t sidecarSizeFor(std::uint64_t size) {
    return kBlockCrcHeaderSize +
           static_cast<std::size_t>(blockCountFor(size)) * 4;
}

std::uint32_t blockLength(std::uint64_t size, std::uint32_t index) {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(index) * kBlockCrcBlockSize;
    if (offset >= size) {
        return 0;
    }
    const std::uint64_t remain = size - offset;
    return static_cast<std::uint32_t>(
        remain < kBlockCrcBlockSize ? remain : kBlockCrcBlockSize);
}

std::string sidecarPath(const std::string& blobPath) {
    return blobPath + ".crc";
}

bool writeAll(int fd, const void* data, std::size_t len) {
    const char* bytes = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, bytes + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

bool pwriteAll(int fd, const void* data, std::size_t len, off_t offset) {
    const char* bytes = static_cast<const char*>(data);
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n =
            ::pwrite(fd, bytes + written, len - written,
                     offset + static_cast<off_t>(written));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

bool readAll(int fd, void* data, std::size_t len) {
    char* bytes = static_cast<char*>(data);
    std::size_t readBytes = 0;
    while (readBytes < len) {
        const ssize_t n = ::read(fd, bytes + readBytes, len - readBytes);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        readBytes += static_cast<std::size_t>(n);
    }
    return true;
}

void encodeHeader(std::uint64_t size, std::uint8_t header[kBlockCrcHeaderSize]) {
    std::memcpy(header, kMagic, sizeof(kMagic));
    header[5] = kVersion;
    cm::writeU32BE(header + 6, kBlockCrcBlockSize);
    cm::writeU32BE(header + 10, blockCountFor(size));
}

// 读该块并算 CRC；失败返回非 Ok。
cm::ErrorCode computeBlockCrc(const std::string& blobPath,
                              std::uint64_t size,
                              std::uint32_t index,
                              std::uint32_t* crc) {
    const std::uint32_t len = blockLength(size, index);
    if (len == 0) {
        return cm::ErrorCode::InvalidArgument;
    }

    const int fd = ::open(blobPath.c_str(), O_RDONLY);
    if (fd < 0) {
        return cm::ErrorCode::FileGone;
    }

    const off_t offset =
        static_cast<off_t>(static_cast<std::uint64_t>(index) *
                           kBlockCrcBlockSize);
    if (::lseek(fd, offset, SEEK_SET) < 0) {
        ::close(fd);
        return cm::ErrorCode::FileGone;
    }

    cm::Crc32 value;
    std::vector<char> buffer(std::min<std::size_t>(kIoBufferSize, len));
    std::uint32_t remain = len;
    while (remain > 0) {
        const std::size_t want =
            std::min<std::size_t>(buffer.size(), static_cast<std::size_t>(remain));
        const ssize_t n = ::read(fd, &buffer[0], want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return cm::ErrorCode::FileGone;
        }
        if (n == 0) {
            ::close(fd);
            return cm::ErrorCode::FileGone;
        }
        value.update(&buffer[0], static_cast<std::size_t>(n));
        remain -= static_cast<std::uint32_t>(n);
    }
    ::close(fd);

    *crc = value.value();
    return cm::ErrorCode::Ok;
}

// 只在 sidecar 缺失/损坏时调用；保证文件长度正好是 14+4n。
// 内容先写 header，其余由 ftruncate 补 0（0 表示未知，可懒填充）。
bool ensureSidecarShell(const std::string& path, std::uint64_t size,
                        bool forceRewrite) {
    const std::size_t expected = sidecarSizeFor(size);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    // 长度对但 magic/count 坏时也必须重写，不能只看长度。
    if (!forceRewrite && static_cast<std::size_t>(st.st_size) == expected) {
        ::close(fd);
        return true;
    }

    std::uint8_t header[kBlockCrcHeaderSize];
    encodeHeader(size, header);
    if (!pwriteAll(fd, header, sizeof(header), 0) ||
        ::ftruncate(fd, static_cast<off_t>(expected)) != 0) {
        ::close(fd);
        return false;
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

}  // namespace

ErrorCode writeBlockCrc(const std::string& blobPath, std::uint64_t size) {
    if (blobPath.empty() || size == 0) {
        return ErrorCode::InvalidArgument;
    }

    const std::uint32_t count = blockCountFor(size);
    const std::string tmpPath = sidecarPath(blobPath) + ".tmp";

    const int blobFd = ::open(blobPath.c_str(), O_RDONLY);
    if (blobFd < 0) {
        return ErrorCode::FileGone;
    }

    const int outFd = ::open(tmpPath.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) {
        ::close(blobFd);
        return ErrorCode::NoSpace;
    }

    bool ok = true;
    std::uint8_t header[kBlockCrcHeaderSize];
    encodeHeader(size, header);
    if (!writeAll(outFd, header, sizeof(header))) {
        ok = false;
    }

    std::vector<char> buffer(kIoBufferSize);
    for (std::uint32_t index = 0; ok && index < count; ++index) {
        const std::uint32_t len = blockLength(size, index);
        cm::Crc32 value;
        std::uint32_t remain = len;
        while (remain > 0) {
            const std::size_t want = std::min<std::size_t>(
                buffer.size(), static_cast<std::size_t>(remain));
            const ssize_t n = ::read(blobFd, &buffer[0], want);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }
            if (n == 0) {
                ok = false;  // blob 比声明短
                break;
            }
            value.update(&buffer[0], static_cast<std::size_t>(n));
            remain -= static_cast<std::uint32_t>(n);
        }
        if (!ok) {
            break;
        }
        std::uint8_t encoded[4];
        cm::writeU32BE(encoded, value.value());
        if (!writeAll(outFd, encoded, sizeof(encoded))) {
            ok = false;
        }
    }

    ::close(blobFd);
    if (ok && ::fsync(outFd) != 0) {
        ok = false;
    }
    if (::close(outFd) != 0) {
        ok = false;
    }
    if (!ok) {
        ::unlink(tmpPath.c_str());
        return ErrorCode::NoSpace;
    }
    if (::rename(tmpPath.c_str(), sidecarPath(blobPath).c_str()) != 0) {
        ::unlink(tmpPath.c_str());
        return ErrorCode::NoSpace;
    }
    return ErrorCode::Ok;
}

ErrorCode readBlockCrc(const std::string& blobPath, std::uint64_t size,
                       std::vector<std::uint32_t>* crcs, bool* missing) {
    if (blobPath.empty() || size == 0 || crcs == NULL) {
        return ErrorCode::InvalidArgument;
    }
    crcs->clear();
    if (missing != NULL) {
        *missing = false;
    }

    const int fd = ::open(sidecarPath(blobPath).c_str(), O_RDONLY);
    if (fd < 0) {
        if (missing != NULL) {
            *missing = true;
        }
        return ErrorCode::Ok;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        if (missing != NULL) {
            *missing = true;
        }
        return ErrorCode::Ok;
    }

    std::uint8_t header[kBlockCrcHeaderSize];
    if (st.st_size < static_cast<off_t>(kBlockCrcHeaderSize) ||
        !readAll(fd, header, sizeof(header)) ||
        std::memcmp(header, kMagic, sizeof(kMagic)) != 0 ||
        header[5] != kVersion ||
        cm::readU32BE(header + 6) != kBlockCrcBlockSize) {
        ::close(fd);
        if (missing != NULL) {
            *missing = true;
        }
        return ErrorCode::Ok;
    }

    const std::uint32_t expectedCount = blockCountFor(size);
    const std::uint32_t count = cm::readU32BE(header + 10);
    const std::size_t expectedSize =
        kBlockCrcHeaderSize + static_cast<std::size_t>(expectedCount) * 4;
    // 长度不足或多出、block_count 与 size 推导不一致，都视为损坏。
    if (count != expectedCount ||
        static_cast<std::size_t>(st.st_size) != expectedSize) {
        ::close(fd);
        if (missing != NULL) {
            *missing = true;
        }
        return ErrorCode::Ok;
    }

    crcs->resize(expectedCount);
    std::uint8_t encoded[4];
    for (std::uint32_t i = 0; i < expectedCount; ++i) {
        if (!readAll(fd, encoded, sizeof(encoded))) {
            crcs->clear();
            if (missing != NULL) {
                *missing = true;
            }
            ::close(fd);
            return ErrorCode::Ok;
        }
        (*crcs)[i] = cm::readU32BE(encoded);
    }
    ::close(fd);
    return ErrorCode::Ok;
}

ErrorCode ensureBlockCrc(const std::string& blobPath, std::uint64_t size,
                         std::uint32_t blockIndex, std::uint32_t* crc) {
    if (blobPath.empty() || size == 0 || crc == NULL) {
        return ErrorCode::InvalidArgument;
    }
    if (blockIndex >= blockCountFor(size)) {
        return ErrorCode::InvalidArgument;
    }

    std::vector<std::uint32_t> crcs;
    bool missing = false;
    ErrorCode rc = readBlockCrc(blobPath, size, &crcs, &missing);
    if (rc != ErrorCode::Ok) {
        return rc;
    }
    if (!missing && blockIndex < crcs.size() && crcs[blockIndex] != 0) {
        *crc = crcs[blockIndex];
        return ErrorCode::Ok;
    }

    // 慢路径：单进程串行，避免建壳与 pwrite 的竞态。
    std::lock_guard<std::mutex> lock(g_lazyFillMutex);

    // 双检：等锁期间别的线程可能已经填好。
    crcs.clear();
    missing = false;
    rc = readBlockCrc(blobPath, size, &crcs, &missing);
    if (rc != ErrorCode::Ok) {
        return rc;
    }
    if (!missing && blockIndex < crcs.size() && crcs[blockIndex] != 0) {
        *crc = crcs[blockIndex];
        return ErrorCode::Ok;
    }

    std::uint32_t computed = 0;
    rc = computeBlockCrc(blobPath, size, blockIndex, &computed);
    if (rc != ErrorCode::Ok) {
        return rc;
    }
    *crc = computed;

    const std::string path = sidecarPath(blobPath);
    if (missing || crcs.empty()) {
        // 失败不阻断下载：值已经算出来了。
        if (!ensureSidecarShell(path, size, true)) {
            // TODO(log): 记 WARN（sidecar 空壳创建失败，下次仍走懒填充）。
            return ErrorCode::Ok;
        }
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        // TODO(log): 记 WARN。
        return ErrorCode::Ok;
    }
    std::uint8_t encoded[4];
    cm::writeU32BE(encoded, computed);
    const off_t slot =
        static_cast<off_t>(kBlockCrcHeaderSize) +
        static_cast<off_t>(blockIndex) * 4;
    if (!pwriteAll(fd, encoded, sizeof(encoded), slot)) {
        // TODO(log): 记 WARN（懒填充写盘失败）。
    }
    ::fsync(fd);
    ::close(fd);
    return ErrorCode::Ok;
}

}  // namespace logic
}  // namespace cm
