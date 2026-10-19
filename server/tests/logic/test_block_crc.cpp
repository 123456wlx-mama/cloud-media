#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"
#include "logic/block_crc.h"
#include "logic/upload_fs.h"

namespace {

const std::uint32_t kBlockSize = 4194304;

std::vector<char> makeData(std::size_t size) {
    std::vector<char> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 31U + 7U) & 0xFF);
    }
    return data;
}

bool writeWholeFile(const std::string& path, const std::vector<char>& data) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::uint32_t directBlockCrc(const std::vector<char>& data,
                             std::uint32_t index) {
    const std::size_t offset = static_cast<std::size_t>(index) * kBlockSize;
    const std::size_t len =
        std::min<std::size_t>(kBlockSize, data.size() - offset);
    cm::Crc32 crc;
    crc.update(data.data() + offset, len);
    return crc.value();
}

bool pwriteAt(const std::string& path, off_t offset,
              const void* data, std::size_t len) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = ::pwrite(fd, data, len, offset);
    ::close(fd);
    return n == static_cast<ssize_t>(len);
}

class BlockCrcTest : public ::testing::Test {
protected:
    void SetUp() override {
        char templ[] = "/tmp/cm_blockcrc_XXXXXX";
        char* dir = ::mkdtemp(templ);
        ASSERT_TRUE(dir != NULL) << "mkdtemp failed";
        m_root = std::string(dir);
        m_blob = m_root + "/blob.bin";
    }

    void TearDown() override {
        if (!m_root.empty()) {
            cm::logic::removeTree(m_root);
            m_root.clear();
        }
    }

    void makeBlob(const std::vector<char>& data) {
        ASSERT_TRUE(writeWholeFile(m_blob, data));
    }

    std::string sidecar() const { return m_blob + ".crc"; }

    std::string m_root;
    std::string m_blob;
};

TEST_F(BlockCrcTest, BlockCrcWriteRead) {
    const std::vector<char> data = makeData(kBlockSize + 1000);
    makeBlob(data);

    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));

    std::vector<std::uint32_t> crcs;
    bool missing = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, &missing));
    EXPECT_FALSE(missing);
    ASSERT_EQ(std::size_t(2), crcs.size());
    EXPECT_EQ(directBlockCrc(data, 0), crcs[0]);
    EXPECT_EQ(directBlockCrc(data, 1), crcs[1]);

    struct stat st;
    ASSERT_EQ(0, ::stat(sidecar().c_str(), &st));
    EXPECT_EQ(static_cast<off_t>(cm::logic::kBlockCrcHeaderSize + 2 * 4),
              st.st_size);
}

TEST_F(BlockCrcTest, BlockCrcLastBlockShort) {
    const std::vector<char> data = makeData(kBlockSize + 137);
    makeBlob(data);
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));

    std::vector<std::uint32_t> crcs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, NULL));
    ASSERT_EQ(std::size_t(2), crcs.size());
    EXPECT_EQ(directBlockCrc(data, 1), crcs[1]);
}

TEST_F(BlockCrcTest, BlockCrcZeroCrcLazyFill) {
    const std::vector<char> data = makeData(kBlockSize + 1000);
    makeBlob(data);
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));

    const std::uint32_t zero = 0;
    ASSERT_TRUE(pwriteAt(sidecar(),
                         static_cast<off_t>(cm::logic::kBlockCrcHeaderSize) + 4,
                         &zero, sizeof(zero)));

    std::vector<std::uint32_t> crcs;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, NULL));
    ASSERT_EQ(std::size_t(2), crcs.size());
    EXPECT_EQ(0u, crcs[1]);

    std::uint32_t crc = 0;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::ensureBlockCrc(m_blob, data.size(), 1, &crc));
    EXPECT_EQ(directBlockCrc(data, 1), crc);

    crcs.clear();
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, NULL));
    ASSERT_EQ(std::size_t(2), crcs.size());
    EXPECT_EQ(directBlockCrc(data, 1), crcs[1]);
}

TEST_F(BlockCrcTest, BlockCrcTruncatedOrBadMagic) {
    const std::vector<char> data = makeData(kBlockSize + 1000);
    makeBlob(data);
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));

    // 截断：长度不足。
    ASSERT_EQ(0, ::truncate(sidecar().c_str(), 20));
    std::vector<std::uint32_t> crcs;
    bool missing = false;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, &missing));
    EXPECT_TRUE(missing);
    EXPECT_TRUE(crcs.empty());

    // 错 magic。
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));
    const char bad[5] = {'X', 'X', 'X', 'X', 'X'};
    ASSERT_TRUE(pwriteAt(sidecar(), 0, bad, sizeof(bad)));
    missing = false;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, &missing));
    EXPECT_TRUE(missing);
    EXPECT_TRUE(crcs.empty());
}

TEST_F(BlockCrcTest, BlockCrcBlockCountMismatch) {
    const std::vector<char> data = makeData(kBlockSize + 1000);
    makeBlob(data);
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::writeBlockCrc(m_blob, data.size()));

    std::uint8_t encoded[4];
    cm::writeU32BE(encoded, 3);  // size 只推导出 2 块
    ASSERT_TRUE(pwriteAt(sidecar(), 10, encoded, sizeof(encoded)));

    std::vector<std::uint32_t> crcs;
    bool missing = false;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, &missing));
    EXPECT_TRUE(missing);
    EXPECT_TRUE(crcs.empty());
}

TEST_F(BlockCrcTest, BlockCrcConcurrentPwriteSameSlot) {
    const std::vector<char> data = makeData(kBlockSize + 1000);
    makeBlob(data);
    EXPECT_NE(0, ::access(sidecar().c_str(), F_OK));  // 起点没有 sidecar

    const std::size_t kThreads = 8;
    std::vector<std::uint32_t> values(kThreads, 0);
    std::vector<cm::ErrorCode> codes(kThreads, cm::ErrorCode::UnknownError);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.push_back(std::thread(
            [this, &data, &values, &codes, i]() {
                codes[i] = cm::logic::ensureBlockCrc(
                    m_blob, data.size(), 1, &values[i]);
            }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    const std::uint32_t expected = directBlockCrc(data, 1);
    for (std::size_t i = 0; i < kThreads; ++i) {
        EXPECT_EQ(cm::ErrorCode::Ok, codes[i]);
        EXPECT_EQ(expected, values[i]);
    }

    std::vector<std::uint32_t> crcs;
    bool missing = true;
    ASSERT_EQ(cm::ErrorCode::Ok,
              cm::logic::readBlockCrc(m_blob, data.size(), &crcs, &missing));
    EXPECT_FALSE(missing);
    ASSERT_EQ(std::size_t(2), crcs.size());
    EXPECT_EQ(expected, crcs[1]);
    EXPECT_EQ(0u, crcs[0]);  // 只懒填充了第 1 块
}

}  // namespace
