#include <gtest/gtest.h>

#include <string>

#include "cache/cache_store.h"

TEST(CacheKeyTest, ShareSixChars) {
    EXPECT_EQ(std::string("cm:v1:share:234567"),
              cm::cache::CacheKey::share("234567"));
    EXPECT_EQ(std::string("cm:v1:share:miss:234567"),
              cm::cache::CacheKey::shareMiss("234567"));
}

TEST(CacheKeyTest, ShareRejectsOldNineDigits) {
    EXPECT_TRUE(cm::cache::CacheKey::share("123456789").empty());
    EXPECT_TRUE(cm::cache::CacheKey::shareMiss("123456789").empty());
}

TEST(CacheKeyTest, ShareRejectsConfusableChars) {
    EXPECT_TRUE(cm::cache::CacheKey::share("23456O").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("23456I").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("23456L").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("023456").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("123456").empty());
}

TEST(CacheKeyTest, ShareRejectsLowercaseAndBadLength) {
    EXPECT_TRUE(cm::cache::CacheKey::share("23456a").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("23456").empty());
    EXPECT_TRUE(cm::cache::CacheKey::share("2345678").empty());
}

TEST(CacheKeyTest, UploadReqKey) {
    EXPECT_EQ(std::string("cm:v1:up:req:42:7"),
              cm::cache::CacheKey::uploadReq(42, 7));
}

TEST(CacheKeyTest, ShareReqKey) {
    EXPECT_EQ(std::string("cm:v1:share:req:42:7"),
              cm::cache::CacheKey::shareReq(42, 7));
}

TEST(CacheKeyTest, OtherKeysUnchanged) {
    EXPECT_EQ(std::string("cm:v1:sess:token"),
              cm::cache::CacheKey::session("token"));
    EXPECT_EQ(std::string("cm:v1:file:md5:0123456789abcdef0123456789abcdef"),
              cm::cache::CacheKey::fileByMd5(
                  "0123456789abcdef0123456789abcdef"));
    EXPECT_EQ(std::string("cm:v1:dl:ticket:t"),
              cm::cache::CacheKey::downloadTicket("t"));
}
