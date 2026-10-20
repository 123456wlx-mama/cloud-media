#include <gtest/gtest.h>

#if defined(__linux__)

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "TcpConnection.h"
#include "EventLoop.h"
#include "EventLoopThread.h"

namespace {

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string makeData(std::size_t len, char seed) {
    std::string out(len, seed);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<char>(seed + static_cast<char>(i % 17));
    }
    return out;
}

int createFileWithData(const std::string& data) {
    char path[] = "/tmp/cm_sendfile_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    ::unlink(path);
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ::close(fd);
            return -1;
        }
    }
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int countOpenFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    const int selfFd = ::dirfd(dir);
    int count = 0;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const int fd = std::atoi(entry->d_name);
        if (fd != selfFd) {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
}

bool readExact(int fd, char* out, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        const ssize_t n = ::read(fd, out + got, len - got);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

struct DoneState {
    DoneState() : count(0), ok(false), finished(false) {}

    std::mutex mutex;
    std::condition_variable cv;
    int count;
    bool ok;
    bool finished;
};

std::function<void(bool)> makeDone(const std::shared_ptr<DoneState>& state) {
    return [state](bool ok) {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->count;
        state->ok = ok;
        state->finished = true;
        state->cv.notify_all();
    };
}

bool waitDone(const std::shared_ptr<DoneState>& state, int timeoutMs) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [state]() { return state->finished; });
}

class ConnectionHarness {
public:
    ConnectionHarness() : loop_(nullptr), clientFd_(-1), ready_(false) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            EXPECT_EQ(0, errno);
            return;
        }
        clientFd_ = fds[1];

        int sndbuf = 4096;
        ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        EXPECT_TRUE(setNonBlocking(fds[0]));

        // loop 必须在它将要运行的线程里构造，否则线程归属断言会失败。
        loop_ = loopThread_.startLoop();
        conn_ = std::make_shared<net::TcpConnection>(loop_, "sendfile-test",
                                                     fds[0], "local");
        loop_->runInLoop([this]() {
            conn_->connectEstablished();
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = true;
            cv_.notify_all();
        });
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(2), [this]() { return ready_; });
    }

    ~ConnectionHarness() {
        std::atomic<bool> destroyed(false);
        if (conn_ && loop_ != nullptr) {
            loop_->runInLoop([this, &destroyed]() {
                conn_->connectDestroyed();
                conn_.reset();
                destroyed.store(true);
            });
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
            while (!destroyed.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (loop_ != nullptr) {
            loop_->quit();
        }
        if (clientFd_ >= 0) {
            ::close(clientFd_);
        }
    }

    net::TcpConnection* conn() const { return conn_.get(); }
    int clientFd() const { return clientFd_; }
    net::EventLoop& loop() { return *loop_; }

private:
    net::EventLoopThread loopThread_;  // 析构时 quit + join
    net::EventLoop* loop_;
    std::shared_ptr<net::TcpConnection> conn_;
    int clientFd_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool ready_;
};

void sendAndRead(const std::string& data, bool withPrelude) {
    ConnectionHarness harness;
    ASSERT_NE(-1, harness.clientFd());

    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    if (withPrelude) {
        harness.conn()->send("HEAD");
    }
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));

    std::string received(data.size() + (withPrelude ? 4 : 0), '\0');
    ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
    if (withPrelude) {
        EXPECT_EQ(std::string("HEAD"), received.substr(0, 4));
        EXPECT_EQ(data, received.substr(4));
    } else {
        EXPECT_EQ(data, received);
    }

    ASSERT_TRUE(waitDone(done, 5000));
    EXPECT_EQ(1, done->count);
    EXPECT_TRUE(done->ok);
    EXPECT_EQ(-1, ::fcntl(fileFd, F_GETFD));
}

}  // namespace

TEST(TcpConnectionSendFileTest, WholeFile) {
    sendAndRead(makeData(4 * 1024 * 1024, 'a'), false);
}

TEST(TcpConnectionSendFileTest, PreludeFirst) {
    sendAndRead(makeData(256 * 1024, 'b'), true);
}

TEST(TcpConnectionSendFileTest, PartialEagain) {
    const std::string data = makeData(1024 * 1024, 'c');
    ConnectionHarness harness;
    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));

    // 先不读，让内核发送缓冲填满，制造 EAGAIN。
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::string received(data.size(), '\0');
    ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
    EXPECT_EQ(data, received);
    ASSERT_TRUE(waitDone(done, 5000));
    EXPECT_TRUE(done->ok);
}

TEST(TcpConnectionSendFileTest, FdClosedOnComplete) {
    const std::string data = makeData(64 * 1024, 'f');
    ConnectionHarness harness;
    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));
    std::string received(data.size(), '\0');
    ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
    ASSERT_TRUE(waitDone(done, 5000));
    EXPECT_EQ(-1, ::fcntl(fileFd, F_GETFD));
}

TEST(TcpConnectionSendFileTest, FdBaselineRestored) {
    const int before = countOpenFds();
    ASSERT_GE(before, 0);

    {
        const std::string data = makeData(32 * 1024, 'g');
        ConnectionHarness harness;
        const int fileFd = createFileWithData(data);
        ASSERT_GE(fileFd, 0);

        std::shared_ptr<DoneState> done(new DoneState());
        ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(),
                                             makeDone(done)));
        std::string received(data.size(), '\0');
        ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
        ASSERT_TRUE(waitDone(done, 5000));
    }

    EXPECT_EQ(before, countOpenFds());
}

TEST(TcpConnectionSendFileTest, IdleNoEpollOutAfterSendFile) {
    const std::string data = makeData(256 * 1024, 'h');
    ConnectionHarness harness;
    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));
    std::string received(data.size(), '\0');
    ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
    ASSERT_TRUE(waitDone(done, 5000));

    std::atomic<int> writing(-1);
    harness.loop().runInLoop([&]() {
        writing.store(harness.conn()->isWriting() ? 1 : 0);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (writing.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(0, writing.load());
}

TEST(TcpConnectionSendFileTest, CallbackOnce) {
    const std::string data = makeData(512 * 1024, 'd');
    ConnectionHarness harness;
    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));
    std::string received(data.size(), '\0');
    ASSERT_TRUE(readExact(harness.clientFd(), &received[0], received.size()));
    ASSERT_TRUE(waitDone(done, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(1, done->count);
}

TEST(TcpConnectionSendFileTest, CancelOnClose) {
    const std::string data = makeData(16 * 1024 * 1024, 'e');
    ConnectionHarness harness;
    const int fileFd = createFileWithData(data);
    ASSERT_GE(fileFd, 0);

    std::shared_ptr<DoneState> done(new DoneState());
    ASSERT_TRUE(harness.conn()->sendFile(fileFd, 0, data.size(), makeDone(done)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    harness.conn()->forceClose();

    ASSERT_TRUE(waitDone(done, 5000));
    EXPECT_EQ(1, done->count);
    EXPECT_FALSE(done->ok);
    EXPECT_EQ(-1, ::fcntl(fileFd, F_GETFD));
}

TEST(TcpConnectionPauseReadTest, StopsEpollInAfterRearm) {
    std::atomic<int> messages(0);
    std::mutex mutex;
    std::condition_variable cv;

    ConnectionHarness harness;
    harness.conn()->setMessageCallback(
        [&](const net::ConnectionPtr& conn, cm::Buffer*, net::Timestamp) {
            const int count = messages.fetch_add(1) + 1;
            if (count == 1) {
                conn->pauseRead();
            }
            cv.notify_all();
        });

    const char first[] = "one";
    ASSERT_EQ(3, ::write(harness.clientFd(), first, 3));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&]() { return messages.load() >= 1; }));
    }

    std::atomic<int> reading(-1);
    harness.loop().runInLoop([&]() {
        reading.store(harness.conn()->isReading() ? 1 : 0);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (reading.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(0, reading.load());

    const char second[] = "two";
    ASSERT_EQ(3, ::write(harness.clientFd(), second, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(1, messages.load());

    harness.conn()->resumeRead();
    std::atomic<int> resumed(-1);
    harness.loop().runInLoop([&]() {
        resumed.store(harness.conn()->isReading() ? 1 : 0);
    });
    const auto resumeDeadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
    while (resumed.load() < 0 &&
           std::chrono::steady_clock::now() < resumeDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(1, resumed.load());

    const char third[] = "three";
    ASSERT_EQ(5, ::write(harness.clientFd(), third, 5));
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&]() { return messages.load() >= 2; }));
    }
}

TEST(TcpConnectionPauseReadTest, SurvivesRearm) {
    std::atomic<int> messages(0);
    std::mutex mutex;
    std::condition_variable cv;

    ConnectionHarness harness;
    harness.conn()->setMessageCallback(
        [&](const net::ConnectionPtr& conn, cm::Buffer*, net::Timestamp) {
            messages.fetch_add(1);
            conn->pauseRead();
            cv.notify_all();
        });

    const char data[] = "x";
    ASSERT_EQ(1, ::write(harness.clientFd(), data, 1));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&]() { return messages.load() >= 1; }));
    }

    std::atomic<int> reading(-1);
    harness.loop().runInLoop([&]() {
        reading.store(harness.conn()->isReading() ? 1 : 0);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (reading.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(0, reading.load());
}

#else  // !__linux__

TEST(TcpConnectionSendFileTest, LinuxOnly) {
    GTEST_SKIP() << "sendfile is Linux only";
}

TEST(TcpConnectionPauseReadTest, LinuxOnly) {
    GTEST_SKIP() << "epoll is Linux only";
}

#endif
