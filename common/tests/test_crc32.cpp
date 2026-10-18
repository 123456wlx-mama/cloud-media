#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "cm/base/crc32.h"

TEST(Crc32Test, KnownVector) {
    const char* input = "123456789";
    EXPECT_EQ(0xCBF43926u, cm::crc32(input, 9));
}

TEST(Crc32Test, EmptyInput) {
    EXPECT_EQ(0u, cm::crc32(NULL, 0));
    EXPECT_EQ(0u, cm::crc32(std::string()));
}

TEST(Crc32Test, StreamingEqualsOneShot) {
    const std::string data = "The quick brown fox jumps over the lazy dog";
    const std::size_t first = 10;
    const std::size_t second = 20;

    cm::Crc32 streaming;
    streaming.update(data.data(), first);
    streaming.update(data.data() + first, second);
    streaming.update(data.data() + first + second,
                     data.size() - first - second);

    EXPECT_EQ(cm::crc32(data), streaming.value());
}
