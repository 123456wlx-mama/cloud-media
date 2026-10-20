#include "cm/config/server_config.h"

#include <sstream>

#include "cm/config/ini_config.h"
#include "cm/log/log_level.h"

namespace cm {
namespace {

ErrorCode requireString(const IniConfig& ini, const char* section, const char* key,
                        std::string* value, std::string* err) {
    if (!ini.has(section, key)) {
        if (err != nullptr) {
            *err = std::string(section) + "." + key + " is required";
        }
        return ErrorCode::NotFound;
    }
    *value = ini.getString(section, key, "");
    if (value->empty()) {
        if (err != nullptr) {
            *err = std::string(section) + "." + key + " must not be empty";
        }
        return ErrorCode::InvalidArgument;
    }
    return ErrorCode::Ok;
}

ErrorCode readInt(const IniConfig& ini, const char* section, const char* key,
                  int defaultValue, int minValue, int maxValue, int* value,
                  std::string* err) {
    int parsed = defaultValue;
    if (ini.has(section, key)) {
        ErrorCode result = ini.getInt(section, key, &parsed);
        if (result != ErrorCode::Ok) {
            if (err != nullptr) {
                *err = std::string(section) + "." + key + " is not an integer";
            }
            return result;
        }
    }
    if (parsed < minValue || parsed > maxValue) {
        if (err != nullptr) {
            std::ostringstream os;
            os << section << "." << key << " must be in ["
               << minValue << ", " << maxValue << "]";
            *err = os.str();
        }
        return ErrorCode::InvalidArgument;
    }
    *value = parsed;
    return ErrorCode::Ok;
}

}  // namespace

ServerConfig::ServerConfig()
    : listenIp("0.0.0.0"),
      listenPort(8004),
      ioThreads(4),
      workerThreads(8),
      dataDir("data"),
      logPath("logs/server.log"),
      logLevel("info"),
      logMaxSizeMb(100),
      logRollDays(7),
      logAsync(true),
      mysqlHost("127.0.0.1"),
      mysqlPort(3306),
      mysqlUser(""),
      mysqlPassword(""),
      mysqlDatabase("cloud_media"),
      redisHost("127.0.0.1"),
      redisPort(6379),
      redisPassword(""),
      redisDb(0),
      transferDownloadDir("downloads"),
      maxConnectionsPerFile(4),
      maxConcurrentFiles(2) {}

std::string ServerConfig::describe() const {
    std::ostringstream os;
    os << "listen_ip=" << listenIp
       << ", listen_port=" << listenPort
       << ", io_threads=" << ioThreads
       << ", worker_threads=" << workerThreads
       << ", data_dir=" << dataDir
       << ", log=" << logPath << ":" << logLevel
       << ", mysql=" << mysqlUser << "@" << mysqlHost << ":" << mysqlPort
       << "/" << mysqlDatabase
       << ", redis=" << redisHost << ":" << redisPort << "/" << redisDb
       << ", transfer=" << transferDownloadDir
       << ":" << maxConnectionsPerFile << "/" << maxConcurrentFiles;
    return os.str();
}

ErrorCode loadServerConfig(const std::string& path, ServerConfig* out,
                           std::string* err) {
    if (out == nullptr) {
        if (err != nullptr) {
            *err = "output config is null";
        }
        return ErrorCode::InvalidArgument;
    }

    IniConfig ini;
    ErrorCode result = ini.load(path, err);
    if (result != ErrorCode::Ok) {
        return result;
    }

    ServerConfig cfg;

    cfg.listenIp = ini.getString("server", "listen_ip", "0.0.0.0");
    if (!ini.has("server", "listen_port")) {
        if (err != nullptr) {
            *err = "server.listen_port is required";
        }
        return ErrorCode::NotFound;
    }

    result = readInt(ini, "server", "listen_port", 8004, 1, 65535,
                     &cfg.listenPort, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "server", "io_threads", 4, 1, 64,
                     &cfg.ioThreads, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "server", "worker_threads", 8, 1, 64,
                     &cfg.workerThreads, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = requireString(ini, "server", "data_dir", &cfg.dataDir, err);
    if (result != ErrorCode::Ok) {
        return result;
    }

    result = requireString(ini, "log", "path", &cfg.logPath, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    cfg.logLevel = ini.getString("log", "level", "info");
    LogLevel parsedLevel;
    if (!parse_log_level(cfg.logLevel, &parsedLevel)) {
        if (err != nullptr) {
            *err = "log.level is invalid: " + cfg.logLevel;
        }
        return ErrorCode::InvalidArgument;
    }
    result = readInt(ini, "log", "max_size_mb", 100, 1, 4096,
                     &cfg.logMaxSizeMb, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "log", "roll_days", 7, 1, 365,
                     &cfg.logRollDays, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    // async 只认 true/false（也接受 1/0），非法值按 true 处理，避免压测配置把日志关死。
    {
        const std::string asyncText = ini.getString("log", "async", "true");
        cfg.logAsync = !(asyncText == "false" || asyncText == "0");
    }

    result = requireString(ini, "mysql", "host", &cfg.mysqlHost, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = requireString(ini, "mysql", "user", &cfg.mysqlUser, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = requireString(ini, "mysql", "password", &cfg.mysqlPassword, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = requireString(ini, "mysql", "database", &cfg.mysqlDatabase, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "mysql", "port", 3306, 1, 65535,
                     &cfg.mysqlPort, err);
    if (result != ErrorCode::Ok) {
        return result;
    }

    result = requireString(ini, "redis", "host", &cfg.redisHost, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "redis", "port", 6379, 1, 65535,
                     &cfg.redisPort, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "redis", "db", 0, 0, 15, &cfg.redisDb, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    cfg.redisPassword = ini.getString("redis", "password", "");

    cfg.transferDownloadDir =
        ini.getString("transfer", "download_dir", "downloads");
    if (cfg.transferDownloadDir.empty()) {
        if (err != nullptr) {
            *err = "transfer.download_dir must not be empty";
        }
        return ErrorCode::InvalidArgument;
    }
    result = readInt(ini, "transfer", "max_connections_per_file", 4, 1, 8,
                     &cfg.maxConnectionsPerFile, err);
    if (result != ErrorCode::Ok) {
        return result;
    }
    result = readInt(ini, "transfer", "max_concurrent_files", 2, 1, 8,
                     &cfg.maxConcurrentFiles, err);
    if (result != ErrorCode::Ok) {
        return result;
    }

    *out = cfg;
    if (err != nullptr) {
        err->clear();
    }
    return ErrorCode::Ok;
}

}  // namespace cm
