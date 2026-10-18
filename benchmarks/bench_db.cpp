#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cache/redis_pool.h"
#include "db/mysql_pool.h"
#include "db/prepared_statement.h"

namespace {

using Clock = std::chrono::steady_clock;

bool envFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string(fallback);
}

cm::db::MysqlOptions mysqlOptions() {
    cm::db::MysqlOptions options;
    options.host = envOr("CM_TEST_MYSQL_HOST", "127.0.0.1");
    options.port = static_cast<unsigned short>(
        std::atoi(envOr("CM_TEST_MYSQL_PORT", "3306").c_str()));
    options.user = envOr("CM_TEST_MYSQL_USER", "cm_test");
    options.password = envOr("CM_TEST_MYSQL_PASSWORD", "");
    options.database = envOr("CM_TEST_MYSQL_DB", "cloud_media_test");
    options.minSize = 4;
    options.maxSize = 8;
    return options;
}

cm::cache::RedisOptions redisOptions() {
    cm::cache::RedisOptions options;
    options.host = envOr("CM_TEST_REDIS_HOST", "127.0.0.1");
    options.port = std::atoi(envOr("CM_TEST_REDIS_PORT", "6379").c_str());
    options.password = envOr("CM_TEST_REDIS_PASSWORD", "");
    options.db = std::atoi(envOr("CM_TEST_REDIS_DB", "0").c_str());
    options.minSize = 4;
    options.maxSize = 8;
    return options;
}

std::string uniqueKey(const char* name) {
    static std::atomic<std::uint64_t> sequence(0);
    const std::uint64_t now = static_cast<std::uint64_t>(
        Clock::now().time_since_epoch().count());
    const std::uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << "cm:bench:" << std::hex << now << ":" << name << ":" << seq;
    return out.str();
}

double qps(std::uint64_t operations, std::chrono::milliseconds elapsed) {
    if (elapsed.count() <= 0) {
        return 0.0;
    }
    return static_cast<double>(operations) * 1000.0 /
           static_cast<double>(elapsed.count());
}

std::int64_t percentile(std::vector<std::int64_t>& values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    std::size_t index = static_cast<std::size_t>(
        p * static_cast<double>(values.size() - 1));
    if (index >= values.size()) {
        index = values.size() - 1;
    }
    return values[index];
}

std::string sanitize(const std::string& text) {
    std::string result = text;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(result[i]);
        if (c <= 0x20 || c == '"' || c == 0x7f) {
            result[i] = '_';
        }
    }
    return result;
}

void printSkipped(const char* bench, const char* reason) {
    std::cout << "bench=" << bench << " status=skipped reason=" << reason
              << std::endl;
}

void printFailed(const char* bench, const std::string& reason) {
    std::cout << "bench=" << bench << " status=failed reason="
              << sanitize(reason) << std::endl;
}

bool benchMysqlPool() {
    if (!envFlag("CM_TEST_MYSQL")) {
        printSkipped("mysql_pool", "CM_TEST_MYSQL_not_set");
        return true;
    }

    std::string error;
    std::shared_ptr<cm::db::MysqlPool> pool =
        cm::db::MysqlPool::create(mysqlOptions(), &error);
    if (!pool) {
        printFailed("mysql_pool", error);
        return false;
    }

    const int kThreads = 8;
    const int kIterations = 50;
    std::atomic<std::uint64_t> errors(0);

    const Clock::time_point start = Clock::now();
    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.push_back(std::thread([pool, &errors]() {
            for (int j = 0; j < 50; ++j) {
                std::unique_ptr<cm::db::MysqlConn> conn = pool->acquire();
                if (!conn || !conn->valid()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }));
    }
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i].join();
    }
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);

    const std::uint64_t total =
        static_cast<std::uint64_t>(kThreads) * kIterations;
    const cm::db::MysqlPoolStats stats = pool->stats();
    std::cout << "bench=mysql_pool threads=" << kThreads
              << " iterations=" << kIterations
              << " total_ops=" << total
              << " elapsed_ms=" << elapsed.count()
              << " qps=" << std::fixed << std::setprecision(2)
              << qps(total, elapsed)
              << " errors=" << errors.load()
              << " in_use=" << stats.inUse
              << " idle=" << stats.idle
              << " total_conn=" << stats.total
              << std::endl;
    return errors.load() == 0;
}

bool benchMysqlRoundtrip() {
    if (!envFlag("CM_TEST_MYSQL")) {
        printSkipped("mysql_roundtrip", "CM_TEST_MYSQL_not_set");
        return true;
    }

    cm::db::MysqlOptions options = mysqlOptions();
    options.minSize = 1;
    options.maxSize = 4;

    std::string error;
    std::shared_ptr<cm::db::MysqlPool> pool =
        cm::db::MysqlPool::create(options, &error);
    if (!pool) {
        printFailed("mysql_roundtrip", error);
        return false;
    }

    std::unique_ptr<cm::db::MysqlConn> conn = pool->acquire();
    if (!conn) {
        printFailed("mysql_roundtrip", "acquire failed");
        return false;
    }
    if (!conn->executeDdl("CREATE TEMPORARY TABLE cm_bench_rw "
                       "(id INT PRIMARY KEY, v INT)")) {
        printFailed("mysql_roundtrip", conn->lastErrorMessage());
        return false;
    }

    const int kIterations = 2000;
    std::vector<std::int64_t> readLatency;
    std::vector<std::int64_t> writeLatency;
    readLatency.reserve(kIterations);
    writeLatency.reserve(kIterations);
    std::uint64_t errors = 0;

    std::unique_ptr<cm::db::PreparedStatement> readStmt =
        conn->prepare("SELECT 1");
    if (!readStmt) {
        printFailed("mysql_roundtrip", conn->lastErrorMessage());
        return false;
    }

    for (int i = 0; i < kIterations; ++i) {
        const Clock::time_point begin = Clock::now();
        if (!readStmt->execute()) {
            ++errors;
            break;
        }
        if (!readStmt->fetch()) {
            ++errors;
        }
        while (readStmt->fetch()) {
        }
        const Clock::time_point end = Clock::now();
        readLatency.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                .count());
    }

    std::unique_ptr<cm::db::PreparedStatement> writeStmt =
        conn->prepare("INSERT INTO cm_bench_rw (id, v) VALUES (?, ?)");
    if (!writeStmt) {
        printFailed("mysql_roundtrip", conn->lastErrorMessage());
        return false;
    }

    for (int i = 0; i < kIterations; ++i) {
        if (!writeStmt->bindInt64(0, i) ||
            !writeStmt->bindInt64(1, i)) {
            ++errors;
            break;
        }
        const Clock::time_point begin = Clock::now();
        if (!writeStmt->execute()) {
            ++errors;
            break;
        }
        const Clock::time_point end = Clock::now();
        writeLatency.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                .count());
    }

    writeStmt.reset();
    conn->executeDdl("DROP TEMPORARY TABLE IF EXISTS cm_bench_rw");

    std::cout << "bench=mysql_roundtrip iterations=" << kIterations
              << " read_p50_us=" << percentile(readLatency, 0.50)
              << " read_p99_us=" << percentile(readLatency, 0.99)
              << " write_p50_us=" << percentile(writeLatency, 0.50)
              << " write_p99_us=" << percentile(writeLatency, 0.99)
              << " errors=" << errors
              << std::endl;
    return errors == 0;
}

bool benchRedis() {
    if (!envFlag("CM_TEST_REDIS")) {
        printSkipped("redis_setget", "CM_TEST_REDIS_not_set");
        return true;
    }

    std::string error;
    std::shared_ptr<cm::cache::RedisPool> pool =
        cm::cache::RedisPool::create(redisOptions(), &error);
    if (!pool) {
        printFailed("redis_setget", error);
        return false;
    }

    std::unique_ptr<cm::cache::RedisConn> conn = pool->acquire();
    if (!conn) {
        printFailed("redis_setget", "acquire failed");
        return false;
    }

    const int kIterations = 100000;
    const std::string key = uniqueKey("setget");
    std::uint64_t errors = 0;
    std::uint64_t setOps = 0;
    std::uint64_t getOps = 0;

    const Clock::time_point setBegin = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        if (!conn->set(key, "value")) {
            ++errors;
            break;
        }
        ++setOps;
    }
    const std::chrono::milliseconds setElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - setBegin);

    std::string value;
    const Clock::time_point getBegin = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        if (conn->get(key, &value) != cm::cache::RedisLookup::Hit) {
            ++errors;
            break;
        }
        ++getOps;
    }
    const std::chrono::milliseconds getElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - getBegin);

    conn->del(key);

    std::cout << "bench=redis_set iterations=" << kIterations
              << " ops=" << setOps
              << " elapsed_ms=" << setElapsed.count()
              << " qps=" << std::fixed << std::setprecision(2)
              << qps(setOps, setElapsed)
              << " errors=" << errors
              << std::endl;
    std::cout << "bench=redis_get iterations=" << kIterations
              << " ops=" << getOps
              << " elapsed_ms=" << getElapsed.count()
              << " qps=" << std::fixed << std::setprecision(2)
              << qps(getOps, getElapsed)
              << " errors=" << errors
              << std::endl;

    return errors == 0;
}

}  // namespace

int main() {
    bool ok = true;
    ok = benchMysqlPool() && ok;
    ok = benchMysqlRoundtrip() && ok;
    ok = benchRedis() && ok;
    return ok ? 0 : 1;
}
