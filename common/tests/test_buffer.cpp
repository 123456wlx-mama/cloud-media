#include <gtest/gtest.h>

#include <string>

#include "cm/base/buffer.h"

TEST(BufferTest, AppendAndRead) {
    cm::Buffer buffer;
    ASSERT_TRUE(buffer.append("hello"));
    EXPECT_EQ(static_cast<std::size_t>(5), buffer.readableBytes());
    EXPECT_TRUE(buffer.peek() != NULL);
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
