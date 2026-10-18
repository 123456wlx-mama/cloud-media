#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include "cm/base/error_code.h"
#include "cm/config/server_config.h"

namespace {

std::string MakeTempFile(const char* tag) {
    static std::atomic<unsigned> counter(0);
    std::ostringstream os;
    os << "cm_server_" << tag << "_" << std::time(NULL) << "_" << counter++ << ".ini";
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

std::string BuildConfig(bool includeUser, int listenPort) {
    std::ostringstream os;
    os << "[server]\n"
       << "listen_ip=127.0.0.1\n"
       << "listen_port=" << listenPort << "\n"
       << "io_threads=4\n"
       << "worker_threads=8\n"
       << "data_dir=data\n"
       << "\n[log]\n"
       << "path=logs/server.log\n"
       << "level=info\n"
       << "max_size_mb=100\n"
       << "roll_days=7\n"
       << "\n[mysql]\n"
       << "host=127.0.0.1\n"
       << "port=3306\n";
    if (includeUser) {
        os << "user=cloud\n";
    }
    os << "password=secret\n"
       << "database=cloud_media\n"
       << "\n[redis]\n"
       << "host=127.0.0.1\n"
       << "port=6379\n"
       << "password=\n"
       << "db=0\n"
       << "\n[transfer]\n"
       << "download_dir=downloads\n"
       << "max_connections_per_file=4\n"
       << "max_concurrent_files=2\n";
    return os.str();
}

}  // namespace

TEST(ServerConfigTest, MissingRequiredFailsWithKey) {
    const std::string path = MakeTempFile("missing");
    ASSERT_TRUE(WriteFile(path, BuildConfig(false, 8004)));

    cm::ServerConfig cfg;
    std::string err;
    cm::ErrorCode result = cm::loadServerConfig(path, &cfg, &err);

    EXPECT_TRUE(result != cm::ErrorCode::Ok);
    EXPECT_NE(std::string::npos, err.find("mysql.user"));

    RemoveFile(path);
}

TEST(ServerConfigTest, RangeValidationAndPasswordRedaction) {
    const std::string path = MakeTempFile("range");
    ASSERT_TRUE(WriteFile(path, BuildConfig(true, 8004)));

    cm::ServerConfig cfg;
    std::string err;
    ASSERT_TRUE(cm::loadServerConfig(path, &cfg, &err) ==
                cm::ErrorCode::Ok);
    EXPECT_EQ(8004, cfg.listenPort);
    EXPECT_EQ(std::string::npos, cfg.describe().find("secret"));

    ASSERT_TRUE(WriteFile(path, BuildConfig(true, 70000)));
    cm::ServerConfig invalid;
    std::string invalidErr;
    EXPECT_TRUE(cm::loadServerConfig(path, &invalid, &invalidErr) !=
                cm::ErrorCode::Ok);
    EXPECT_NE(std::string::npos, invalidErr.find("listen_port"));

    RemoveFile(path);
}
