#pragma once

#include <string>

#include "cm/base/error_code.h"

namespace cm {

struct ServerConfig {
    std::string listenIp;
    int listenPort;
    int ioThreads;
    int workerThreads;
    std::string dataDir;

    std::string logPath;
    std::string logLevel;
    int logMaxSizeMb;
    int logRollDays;
    bool logAsync;  // [log] async；默认 true，false 用于压测对照

    std::string mysqlHost;
    int mysqlPort;
    std::string mysqlUser;
    std::string mysqlPassword;
    std::string mysqlDatabase;

    std::string redisHost;
    int redisPort;
    std::string redisPassword;
    int redisDb;

    std::string transferDownloadDir;
    int maxConnectionsPerFile;
    int maxConcurrentFiles;

    ServerConfig();

    // 输出配置摘要；不得包含 mysqlPassword / redisPassword。
    std::string describe() const;
};

ErrorCode loadServerConfig(const std::string& path, ServerConfig* out,
                           std::string* err);

}  // namespace cm
