// netapi 帧重组单测：粘包 / 半包 / 坏帧。不依赖服务端，也不起 Qt 事件循环。
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/auth.h"
#include "netapi/FrameAssembler.h"

namespace {

using cm::netapi::FrameAssembler;

std::string makeLoginRs(std::uint32_t seq, std::uint32_t code,
                        std::uint64_t uid) {
    cm::protocol::LoginRs rs;
    rs.code = code;
    rs.uid = uid;
    rs.name = "tester";
    std::string payload;
    EXPECT_TRUE(rs.encode(&payload));
    std::string frame;
    EXPECT_EQ(cm::ErrorCode::Ok,
              cm::protocol::Codec::encode(
                  static_cast<std::uint16_t>(cm::protocol::MessageType::LoginRs),
                  seq, payload, &frame));
    return frame;
}

}  // namespace

TEST(FrameAssembler, SingleFrame) {
    FrameAssembler assembler;
    const std::string frame = makeLoginRs(7, 0, 42);
    std::vector<cm::protocol::Frame> out;
    ASSERT_TRUE(assembler.feed(frame.data(), frame.size(), &out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(7u, out[0].seq);
    EXPECT_EQ(static_cast<std::uint16_t>(cm::protocol::MessageType::LoginRs),
              out[0].type);

    cm::protocol::LoginRs rs;
    ASSERT_TRUE(rs.decode(out[0].payload));
    EXPECT_EQ(0u, rs.code);
    EXPECT_EQ(42u, rs.uid);
    EXPECT_EQ("tester", rs.name);
}

// 两个帧粘在一起，且每次只喂 3 字节：必须切出 2 帧，且顺序/SEQ 正确。
TEST(FrameAssembler, StickyAndSplitAcrossReads) {
    FrameAssembler assembler;
    const std::string stream = makeLoginRs(11, 0, 1) + makeLoginRs(12, 0, 2);
    std::vector<cm::protocol::Frame> out;
    for (std::size_t offset = 0; offset < stream.size(); offset += 3) {
        const std::size_t len =
            (stream.size() - offset < 3) ? stream.size() - offset : 3;
        ASSERT_TRUE(assembler.feed(stream.data() + offset, len, &out));
    }
    ASSERT_EQ(2u, out.size());
    EXPECT_EQ(11u, out[0].seq);
    EXPECT_EQ(12u, out[1].seq);
}

// 坏 CRC：必须返回 false（调用方据此断链），不能把坏帧当正常帧抛出去。
TEST(FrameAssembler, BadCrcRejected) {
    FrameAssembler assembler;
    std::string frame = makeLoginRs(21, 0, 3);
    frame[frame.size() - 1] = static_cast<char>(frame[frame.size() - 1] ^ 0xFF);
    std::vector<cm::protocol::Frame> out;
    EXPECT_FALSE(assembler.feed(frame.data(), frame.size(), &out));
    EXPECT_TRUE(out.empty());
}

// 重新连接后 reset 必须丢掉残留半包，否则新一轮会从旧字节开始解析。
TEST(FrameAssembler, ResetDropsPartial) {
    FrameAssembler assembler;
    const std::string frame = makeLoginRs(31, 0, 4);
    std::vector<cm::protocol::Frame> out;
    ASSERT_TRUE(assembler.feed(frame.data(), 5, &out));  // 只喂前 5 字节
    EXPECT_TRUE(out.empty());
    assembler.reset();
    ASSERT_TRUE(assembler.feed(frame.data(), frame.size(), &out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(31u, out[0].seq);
}
