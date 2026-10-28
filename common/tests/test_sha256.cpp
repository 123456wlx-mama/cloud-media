#include <gtest/gtest.h>

#include <string>

#include "cm/base/sha256.h"

namespace {

TEST(Sha256Test, EmptyString) {
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              cm::sha256Hex(""));
}

TEST(Sha256Test, Abc) {
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              cm::sha256Hex("abc"));
}

// FIPS 180-4 附录 B.2：两个块边界上的经典用例。
TEST(Sha256Test, TwoBlockMessage) {
    const std::string msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              cm::sha256Hex(msg));
}

// 补齐长度落在「56~63 字节」这个必须多补一个块的区间。
TEST(Sha256Test, LengthCrossesPaddingBoundary) {
    const std::string msg(56, 'a');
    EXPECT_EQ("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
              cm::sha256Hex(msg));
}

// 分片喂入必须与一次性喂入等价。
TEST(Sha256Test, StreamingMatchesOneShot) {
    const std::string msg = "the quick brown fox jumps over the lazy dog";
    cm::Sha256 hasher;
    for (std::size_t i = 0; i < msg.size(); i += 7) {
        hasher.update(msg.data() + i,
                      msg.size() - i < 7 ? msg.size() - i : static_cast<std::size_t>(7));
    }
    EXPECT_EQ(cm::sha256Hex(msg), hasher.hex());
}

// hex() 不得消费状态：调用后仍能继续 update 并得到正确结果。
TEST(Sha256Test, HexDoesNotConsumeState) {
    cm::Sha256 hasher;
    hasher.update("abc", 3);
    const std::string first = hasher.hex();
    EXPECT_EQ(first, hasher.hex());
    hasher.update("def", 3);
    EXPECT_EQ(cm::sha256Hex("abcdef"), hasher.hex());
}

}  // namespace
