#include <gtest/gtest.h>

#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include "cm/base/buffer.h"

TEST(BufferTest, AppendAndRead) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append("hello"));
    EXPECT_EQ(static_cast<std::size_t>(5), buffer.readableBytes());
    EXPECT_TRUE(buffer.peek() != nullptr);
    EXPECT_EQ(std::string("hello"), std::string(buffer.peek(), buffer.readableBytes()));

    ASSERT_TRUE(buffer.retrieve(5));
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(BufferTest, PartialRetrieve) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append("hello world"));
    ASSERT_TRUE(buffer.retrieve(6));

    EXPECT_EQ(static_cast<std::size_t>(5), buffer.readableBytes());
    EXPECT_EQ(std::string("world"), std::string(buffer.peek(), buffer.readableBytes()));
    EXPECT_EQ(std::string("world"), buffer.retrieveAsString(5));
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(BufferTest, GrowsForLargePayload) {
    const std::size_t size = 1024 * 1024;
    std::string payload(size, 'x');
    payload[0] = 'A';
    payload[size - 1] = 'Z';

    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(payload));
    EXPECT_EQ(size, buffer.readableBytes());
    EXPECT_EQ(std::string(payload), buffer.retrieveAsString(size));
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(BufferTest, CompactReusesSpace) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append(std::string(1000, 'a')));
    ASSERT_TRUE(buffer.retrieve(800));

    ASSERT_TRUE(buffer.append(std::string(5000, 'b')));
    EXPECT_EQ(static_cast<std::size_t>(200 + 5000), buffer.readableBytes());

    std::string first(buffer.peek(), 200);
    EXPECT_EQ(std::string(200, 'a'), first);
    ASSERT_TRUE(buffer.retrieve(200));

    std::string second(buffer.peek(), 5000);
    EXPECT_EQ(std::string(5000, 'b'), second);
    ASSERT_TRUE(buffer.retrieve(5000));
    EXPECT_EQ(static_cast<std::size_t>(0), buffer.readableBytes());
}

TEST(BufferTest, EnsureWritableExpandsAndCompacts) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.ensureWritable(2000));
    EXPECT_GE(buffer.writableBytes(), static_cast<std::size_t>(2000));

    ASSERT_TRUE(buffer.append(std::string(1000, 'a')));
    ASSERT_TRUE(buffer.retrieve(900));
    // 可写区只剩 24 字节，ensureWritable 必须先 compact 再扩容。
    ASSERT_TRUE(buffer.ensureWritable(900));
    EXPECT_EQ(std::size_t(100), buffer.readableBytes());
    EXPECT_EQ(std::string(100, 'a'),
              std::string(buffer.peek(), buffer.readableBytes()));
    ASSERT_TRUE(buffer.append(std::string(900, 'b')));
    EXPECT_EQ(std::size_t(1000), buffer.readableBytes());
}

TEST(BufferTest, RejectsOverMaxBytes) {
    cm::Buffer buffer;
    const std::string tooBig(cm::Buffer::kMaxBytes + 1, 'x');
    EXPECT_FALSE(buffer.append(tooBig));
    EXPECT_EQ(std::size_t(0), buffer.readableBytes());
    EXPECT_FALSE(buffer.ensureWritable(cm::Buffer::kMaxBytes + 1));
}

#if !defined(_WIN32)
namespace {

void OnAlarm(int) {
}

}  // namespace

TEST(BufferTest, ReadFdReadsFromPipe) {
    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));
    ASSERT_EQ(3, static_cast<int>(::write(fds[1], "abc", 3)));

    cm::Buffer buffer;
    int savedErrno = 0;
    const ssize_t n = buffer.readFd(fds[0], &savedErrno);
    EXPECT_EQ(3, static_cast<int>(n));
    EXPECT_EQ(std::string("abc"),
              std::string(buffer.peek(), buffer.readableBytes()));
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, ReadFdReturnsEagainOnEmptyNonBlockingPipe) {
    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    ASSERT_GE(flags, 0);
    ASSERT_EQ(0, ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK));

    cm::Buffer buffer;
    int savedErrno = 0;
    const ssize_t n = buffer.readFd(fds[0], &savedErrno);
    EXPECT_EQ(-1, static_cast<int>(n));
    EXPECT_TRUE(savedErrno == EAGAIN || savedErrno == EWOULDBLOCK);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(BufferTest, ReadFdReturnsEintrOnSignal) {
    int fds[2];
    ASSERT_EQ(0, ::pipe(fds));

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = OnAlarm;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // 关键：不要 SA_RESTART，readv 才会被中断
    struct sigaction old;
    ASSERT_EQ(0, ::sigaction(SIGALRM, &action, &old));

    ::alarm(1);
    cm::Buffer buffer;
    int savedErrno = 0;
    const ssize_t n = buffer.readFd(fds[0], &savedErrno);
    ::alarm(0);
    ::sigaction(SIGALRM, &old, nullptr);

    EXPECT_EQ(-1, static_cast<int>(n));
    EXPECT_EQ(EINTR, savedErrno);
    ::close(fds[0]);
    ::close(fds[1]);
}
#endif
