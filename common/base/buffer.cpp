#include "cm/base/buffer.h"

#include <algorithm>
#include <cstring>

#ifndef _WIN32
#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace cm {

const std::size_t Buffer::kMaxBytes = 16 * 1024 * 1024;

Buffer::Buffer()
    : storage_(1024),
      readerIndex_(0),
      writerIndex_(0) {}

bool Buffer::append(const char* data, std::size_t len) {
    if (len == 0) {
        return true;
    }
    if (data == nullptr || len > kMaxBytes) {
        return false;
    }
    if (!ensureWritable(len)) {
        return false;
    }
    std::memcpy(&storage_[writerIndex_], data, len);
    writerIndex_ += len;
    return true;
}

bool Buffer::append(const std::string& data) {
    return append(data.data(), data.size());
}

const char* Buffer::peek() const {
    if (storage_.empty()) {
        return nullptr;
    }
    return &storage_[readerIndex_];
}

const char* Buffer::data() const {
    return peek();
}

std::size_t Buffer::readableBytes() const {
    return writerIndex_ - readerIndex_;
}

std::size_t Buffer::writableBytes() const {
    return storage_.size() - writerIndex_;
}

std::size_t Buffer::capacity() const {
    return storage_.size();
}

void Buffer::shrinkTo(std::size_t minCapacity) {
    const std::size_t readable = readableBytes();
    std::size_t target = minCapacity;
    if (target < 1024) {
        target = 1024;
    }
    if (target < readable) {
        target = readable;
    }
    if (target > kMaxBytes) {
        target = kMaxBytes;
    }
    if (target >= storage_.size()) {
        return;
    }

    std::vector<char> next(target);
    if (readable > 0) {
        std::memcpy(&next[0], &storage_[readerIndex_], readable);
    }
    storage_.swap(next);
    readerIndex_ = 0;
    writerIndex_ = readable;
}

bool Buffer::retrieve(std::size_t len) {
    if (len > readableBytes()) {
        return false;
    }
    readerIndex_ += len;
    if (readerIndex_ == writerIndex_) {
        readerIndex_ = 0;
        writerIndex_ = 0;
    }
    return true;
}

std::string Buffer::retrieveAsString(std::size_t len) {
    if (len > readableBytes()) {
        return std::string();
    }
    std::string out(storage_.data() + readerIndex_, len);
    retrieve(len);
    return out;
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

void Buffer::retrieveAll() {
    retrieve(readableBytes());
}

bool Buffer::ensureWritable(std::size_t len) {
    if (len > kMaxBytes) {
        return false;
    }
    if (writableBytes() >= len) {
        return true;
    }
    if (readerIndex_ > 0) {
        compact();
    }
    if (writableBytes() >= len) {
        return true;
    }

    std::size_t required = writerIndex_ + len;
    if (required > kMaxBytes) {
        return false;
    }

    std::size_t newSize = storage_.size() * 2;
    if (newSize < required) {
        newSize = required;
    }
    if (newSize > kMaxBytes) {
        newSize = kMaxBytes;
    }
    if (newSize < required) {
        return false;
    }
    storage_.resize(newSize);
    return true;
}

void Buffer::compact() {
    if (readerIndex_ == 0) {
        return;
    }
    std::size_t readable = readableBytes();
    if (readable > 0) {
        std::memmove(storage_.data(), storage_.data() + readerIndex_, readable);
    }
    readerIndex_ = 0;
    writerIndex_ = readable;
}

void Buffer::reset() {
    readerIndex_ = 0;
    writerIndex_ = 0;
}

#ifndef _WIN32
ssize_t Buffer::readFd(int fd, int* savedErrno) {
    char extrabuf[65536];
    struct iovec vec[2];
    const std::size_t writable = writableBytes();
    vec[0].iov_base = storage_.data() + writerIndex_;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, iovcnt);
    if (n < 0) {
        *savedErrno = errno;
    } else if (static_cast<std::size_t>(n) <= writable) {
        writerIndex_ += static_cast<std::size_t>(n);
    } else {
        writerIndex_ = storage_.size();
        append(extrabuf, static_cast<std::size_t>(n) - writable);
    }
    return n;
}
#endif

}  // namespace cm
