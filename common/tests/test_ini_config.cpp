#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include "cm/base/error_code.h"
#include "cm/config/ini_config.h"

namespace {

std::string MakeTempFile(const char* tag) {
    static std::atomic<unsigned> counter(0);
    std::ostringstream os;
    os << "cm_ini_" << tag << "_" << std::time(NULL) << "_" << counter++ << ".ini";
    return os.str();
}

bool WriteFile(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

void RemoveFile(const std::string& path) {
    std::remove(path.c_str());
}

}  // namespace

TEST(IniConfigTest, ParsesSectionsCommentsQuotesBom) {
    const std::string path = MakeTempFile("parse");
    const std::string content =
        "\xEF\xBB\xBF# comment\n"
        "; another comment\n"
        "[server]\n"
        "listen_ip = \"0.0.0.0\"\n"
        "port=8004\n"
        "[flags]\n"
        "enabled=true\n";
    ASSERT_TRUE(WriteFile(path, content));

    cm::IniConfig ini;
    std::string err;
    ASSERT_TRUE(ini.load(path, &err) == cm::ErrorCode::Ok);

    EXPECT_EQ(std::string("0.0.0.0"),
              ini.getString("server", "listen_ip", ""));
    int port = 0;
    EXPECT_TRUE(ini.getInt("server", "port", &port) == cm::ErrorCode::Ok);
    EXPECT_EQ(8004, port);
    bool enabled = false;
    EXPECT_TRUE(ini.getBool("flags", "enabled", &enabled) == cm::ErrorCode::Ok);
    EXPECT_TRUE(enabled);
    EXPECT_EQ(std::string("default"),
              ini.getString("server", "missing", "default"));

    RemoveFile(path);
}

TEST(IniConfigTest, DuplicateKeyLastWins) {
    const std::string path = MakeTempFile("duplicate");
    ASSERT_TRUE(WriteFile(path, "[s]\nk=first\nk=second\n"));

    cm::IniConfig ini;
    std::string err;
    ASSERT_TRUE(ini.load(path, &err) == cm::ErrorCode::Ok);
    EXPECT_EQ(std::string("second"), ini.getString("s", "k", ""));

    RemoveFile(path);
}

TEST(IniConfigTest, InvalidIntegerReportsError) {
    const std::string path = MakeTempFile("badint");
    ASSERT_TRUE(WriteFile(path, "[s]\nport=abc\n"));

    cm::IniConfig ini;
    std::string err;
    ASSERT_TRUE(ini.load(path, &err) == cm::ErrorCode::Ok);

    int value = 7;
    EXPECT_TRUE(ini.getInt("s", "port", &value) ==
                cm::ErrorCode::InvalidArgument);
    EXPECT_EQ(7, value);

    RemoveFile(path);
}

TEST(IniConfigTest, MalformedLineReportsLineNumber) {
    const std::string path = MakeTempFile("badline");
    ASSERT_TRUE(WriteFile(path, "[server]\nport=8004\nbroken line\n"));

    cm::IniConfig ini;
    std::string err;
    EXPECT_TRUE(ini.load(path, &err) == cm::ErrorCode::InvalidArgument);
    EXPECT_NE(std::string::npos, err.find("line 3"));

    RemoveFile(path);
}
