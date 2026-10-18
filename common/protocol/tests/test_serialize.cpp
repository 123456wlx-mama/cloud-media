#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/protocol/serialize.h"

TEST(SerializeTest, PrimitiveRoundTrip) {
    cm::protocol::Writer writer;
    ASSERT_TRUE(writer.writeU8(0x12));
    ASSERT_TRUE(writer.writeU16(0x3456));
    ASSERT_TRUE(writer.writeU32(0x789ABCDEu));
    ASSERT_TRUE(writer.writeU64(0x0123456789ABCDEFull));

    cm::protocol::Reader reader(writer.data());
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;

    ASSERT_TRUE(reader.readU8(&u8));
    ASSERT_TRUE(reader.readU16(&u16));
    ASSERT_TRUE(reader.readU32(&u32));
    ASSERT_TRUE(reader.readU64(&u64));

    EXPECT_EQ(0x12u, static_cast<unsigned>(u8));
    EXPECT_EQ(0x3456u, static_cast<unsigned>(u16));
    EXPECT_EQ(0x789ABCDEu, u32);
    EXPECT_EQ(0x0123456789ABCDEFull, u64);
    EXPECT_EQ(static_cast<std::size_t>(0), reader.remaining());
    EXPECT_TRUE(reader.ok());
}

TEST(SerializeTest, StringRoundTrip) {
    const std::string value = "云盘测试";
    cm::protocol::Writer writer;
    ASSERT_TRUE(writer.writeString(value));

    cm::protocol::Reader reader(writer.data());
    std::string decoded;
    ASSERT_TRUE(reader.readString(&decoded));

    EXPECT_EQ(value, decoded);
    EXPECT_EQ(static_cast<std::size_t>(0), reader.remaining());
    EXPECT_TRUE(reader.ok());
}

TEST(SerializeTest, TruncatedStringFails) {
    cm::protocol::Writer writer;
    ASSERT_TRUE(writer.writeString("hello"));

    std::string truncated = writer.data();
    ASSERT_FALSE(truncated.empty());
    truncated.resize(truncated.size() - 1);

    cm::protocol::Reader reader(truncated);
    std::string decoded;
    EXPECT_FALSE(reader.readString(&decoded));
    EXPECT_FALSE(reader.ok());
}
