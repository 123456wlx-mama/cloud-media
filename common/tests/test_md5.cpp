#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/base/md5.h"

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

std::string md5Chunked(const std::string& data, std::size_t chunk) {
    cm::Md5 md5;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t take =
            data.size() - offset < chunk ? data.size() - offset : chunk;
        md5.update(data.data() + offset, take);
        offset += take;
    }
    return md5.hex();
}

}  // namespace

TEST(Md5Test, Empty) {
    EXPECT_EQ("d41d8cd98f00b204e9800998ecf8427e", cm::md5Hex(""));
}

TEST(Md5Test, KnownVectors) {
    EXPECT_EQ("900150983cd24fb0d6963f7d28e17f72", cm::md5Hex("abc"));
    EXPECT_EQ("25f9e794323b453885f5181f1b624d0b",
              cm::md5Hex("123456789"));
}

TEST(Md5Test, OneMegabyteChunkedEqualsOneShot) {
    const std::string data = makeData(1024 * 1024, 0x12345678u);
    const std::string expected = cm::md5Hex(data);
    EXPECT_EQ(expected, md5Chunked(data, 1));
    EXPECT_EQ(expected, md5Chunked(data, 7));
    EXPECT_EQ(expected, md5Chunked(data, 4096));
    EXPECT_EQ(expected, md5Chunked(data, 64 * 1024));
}

TEST(Md5Test, FourMegabyteOneShotAndChunked) {
    const std::string data = makeData(4 * 1024 * 1024, 0x0badf00du);
    const std::string expected = cm::md5Hex(data);
    EXPECT_EQ(expected, md5Chunked(data, 64 * 1024));
}

TEST(Md5Test, ResetReuse) {
    cm::Md5 md5;
    md5.update("abc", 3);
    EXPECT_EQ(cm::md5Hex("abc"), md5.hex());
    md5.reset();
    md5.update("123456789", 9);
    EXPECT_EQ(cm::md5Hex("123456789"), md5.hex());
}

TEST(Md5Test, UpdateNullZeroIsNoOp) {
    cm::Md5 md5;
    md5.update(nullptr, 0);
    EXPECT_EQ(cm::md5Hex(""), md5.hex());
}
