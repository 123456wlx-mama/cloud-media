#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <random>
#include <string>

#include "cm/base/crc32.h"

namespace {

std::string makeData(std::size_t len, std::uint32_t seed) {
    std::string out(len, '\0');
    std::uint32_t value = seed;
    for (std::size_t i = 0; i < len; ++i) {
        value = value * 1664525u + 1013904223u;
        out[i] = static_cast<char>((value >> 24) & 0xffu);
    }
    return out;
}

std::uint32_t crcOf(const std::string& data) {
    return cm::crc32(data.data(), data.size());
}

}  // namespace

TEST(Crc32CombineTest, Empty) {
    EXPECT_EQ(cm::crc32(""), cm::crc32Combine(cm::crc32(""), cm::crc32(""), 0));
}

TEST(Crc32CombineTest, KnownVector) {
    const std::string a = "1234";
    const std::string b = "56789";
    EXPECT_EQ(cm::crc32("123456789"),
              cm::crc32Combine(crcOf(a), crcOf(b), b.size()));
}

TEST(Crc32CombineTest, Randomized128) {
    const std::size_t edges[] = {
        0, 1, 3, 4 * 1024 * 1024 - 1, 4 * 1024 * 1024, 1024, 65535, 2
    };
    const std::size_t edgeCount = sizeof(edges) / sizeof(edges[0]);

    std::mt19937 rng(0xC0FFEEu);
    for (int caseIndex = 0; caseIndex < 128; ++caseIndex) {
        std::size_t len = 0;
        if (caseIndex < static_cast<int>(edgeCount)) {
            len = edges[caseIndex];
        } else {
            len = static_cast<std::size_t>(rng() % (256u * 1024u + 1u));
        }

        const std::string data = makeData(len, 0x9E3779B9u + caseIndex);
        const std::size_t split =
            len == 0 ? 0 : static_cast<std::size_t>(rng() % (len + 1));
        const std::string a = data.substr(0, split);
        const std::string b = data.substr(split);

        const std::uint32_t expected = cm::crc32(data.data(), data.size());
        const std::uint32_t actual =
            cm::crc32Combine(crcOf(a), crcOf(b), b.size());

        EXPECT_EQ(expected, actual)
            << "case=" << caseIndex << " lenA=" << a.size()
            << " lenB=" << b.size() << " expected=0x" << std::hex << expected
            << " actual=0x" << actual;
    }
}

TEST(Crc32CombineTest, FourMegabyteMinusOne) {
    const std::size_t len = 4 * 1024 * 1024 - 1;
    const std::string data = makeData(len, 0x13579BDFu);
    const std::size_t split = len / 2;
    EXPECT_EQ(cm::crc32(data.data(), data.size()),
              cm::crc32Combine(crcOf(data.substr(0, split)),
                               crcOf(data.substr(split)),
                               data.size() - split));
}

TEST(Crc32CombineTest, Header11AndFourMegabyteBlock) {
    const std::string header = makeData(11, 0x2468ACE0u);
    const std::string payload = makeData(4 * 1024 * 1024, 0x0F1E2D3Cu);

    cm::Crc32 direct;
    direct.update(header.data(), header.size());
    direct.update(payload.data(), payload.size());

    EXPECT_EQ(direct.value(),
              cm::crc32Combine(crcOf(header), crcOf(payload), payload.size()));
}

TEST(Crc32CombineTest, ChunkedEqualsOneShot) {
    const std::string data = makeData(1024 * 1024 + 37, 0xDEADBEEFu);
    const std::size_t split = 400000;
    const std::string a = data.substr(0, split);
    const std::string b = data.substr(split);

    cm::Crc32 ca;
    for (std::size_t offset = 0; offset < a.size(); offset += 64 * 1024) {
        const std::size_t take =
            a.size() - offset < 64 * 1024 ? a.size() - offset : 64 * 1024;
        ca.update(a.data() + offset, take);
    }

    cm::Crc32 cb;
    for (std::size_t offset = 0; offset < b.size(); offset += 64 * 1024) {
        const std::size_t take =
            b.size() - offset < 64 * 1024 ? b.size() - offset : 64 * 1024;
        cb.update(b.data() + offset, take);
    }

    EXPECT_EQ(cm::crc32(data.data(), data.size()),
              cm::crc32Combine(ca.value(), cb.value(), b.size()));
}

TEST(Crc32CombineTest, LenZeroReturnsCrc1) {
    EXPECT_EQ(0x12345678u, cm::crc32Combine(0x12345678u, 0xABCDEF01u, 0));
}
