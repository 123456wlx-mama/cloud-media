#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cli_common.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/list.h"

namespace {

struct Options {
    std::string host;
    int port;
    std::string tel;
    std::string password;
    std::uint64_t parentId;
    std::uint8_t sort;
    std::uint32_t limit;
    std::string cursorKey;
    std::uint64_t cursorId;

    // limit 默认 50（与用法文案「0=50」一致；传 0 也按 50 处理）。
    Options() : port(8004), parentId(0), sort(0), limit(50), cursorId(0) {
    }
};

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --host H --port P --tel T --password P "
                 "[--parent-id N]\n"
                 "          [--sort 0|1|2] [--limit N] [--cursor-key KEY] "
                 "[--cursor-id N]\n"
                 "\n"
                 "  sort 0=name asc 1=created desc 2=updated desc, limit 0=50\n"
                 "  password may also come from CM_CLI_PASSWORD\n",
                 program);
}

bool parseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = (i + 1 < argc);
        if (arg == "--host" && hasValue) {
            opt->host = argv[++i];
        } else if (arg == "--port" && hasValue) {
            if (!cm::cli::parsePositiveInt(argv[++i], &opt->port) ||
                opt->port > 65535) {
                return false;
            }
        } else if (arg == "--tel" && hasValue) {
            opt->tel = argv[++i];
        } else if (arg == "--password" && hasValue) {
            opt->password = argv[++i];
        } else if (arg == "--parent-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->parentId)) {
                return false;
            }
        } else if (arg == "--sort" && hasValue) {
            std::uint32_t sort = 0;
            if (!cm::cli::parseU32(argv[++i], &sort) || sort > 2) {
                return false;
            }
            opt->sort = static_cast<std::uint8_t>(sort);
        } else if (arg == "--limit" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->limit) ||
                opt->limit > 200) {
                return false;
            }
        } else if (arg == "--cursor-key" && hasValue) {
            opt->cursorKey = argv[++i];
        } else if (arg == "--cursor-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->cursorId)) {
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        printUsage(argv[0]);
        return cm::cli::kExitUsage;
    }
    if (opt.password.empty()) {
        opt.password = cm::cli::passwordFromEnv();
    }
    if (opt.host.empty() || opt.tel.empty() || opt.password.empty()) {
        printUsage(argv[0]);
        return cm::cli::kExitUsage;
    }

    std::string error;
    int exitCode = cm::cli::kExitNetwork;
    const int fd = cm::cli::connectAndLogin(opt.host, opt.port, opt.tel,
                                            opt.password, &error, &exitCode);
    if (fd < 0) {
        std::fprintf(stderr, "login failed: %s\n", error.c_str());
        return exitCode;
    }

    cm::protocol::ListFileRq rq;
    rq.parentId = opt.parentId;
    rq.sort = opt.sort;
    rq.limit = opt.limit == 0 ? 50 : opt.limit;
    rq.cursorKey = opt.cursorKey;
    rq.cursorId = opt.cursorId;

    std::string payload;
    if (!rq.encode(&payload) ||
        !cm::cli::sendFrame(
            fd,
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::ListFileRq),
            1, payload, &error)) {
        std::fprintf(stderr, "list send failed: %s\n", error.c_str());
        ::close(fd);
        return cm::cli::kExitUsage;
    }

    cm::Buffer buffer;
    cm::protocol::Frame frame;
    if (!cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
        std::fprintf(stderr, "list recv failed: %s\n", error.c_str());
        ::close(fd);
        return cm::cli::kExitNetwork;
    }
    ::close(fd);

    if (frame.type == static_cast<std::uint16_t>(
                          cm::protocol::MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        cm::cli::decodeErrorRs(frame, &code, &message);
        std::printf("CODE=%u\n", code);
        std::fprintf(stderr, "list rejected: %s\n", message.c_str());
        return cm::cli::kExitRejected;
    }

    cm::protocol::ListFileRs rs;
    if (frame.type != static_cast<std::uint16_t>(
                          cm::protocol::MessageType::ListFileRs) ||
        !rs.decode(frame.payload)) {
        std::fprintf(stderr, "bad list response\n");
        return cm::cli::kExitNetwork;
    }
    if (rs.code != 0) {
        // 降级演练依赖这里：MySQL 挂时必须回 4001/4002，而不是空列表。
        std::printf("CODE=%u\n", rs.code);
        return cm::cli::kExitRejected;
    }

    for (std::size_t i = 0; i < rs.items.size(); ++i) {
        const cm::protocol::FileItem& item = rs.items[i];
        std::printf("UF_ID=%llu\tNAME=%s\tIS_DIR=%u\tSIZE=%llu\t"
                    "UPDATED_AT=%llu\n",
                    static_cast<unsigned long long>(item.ufId),
                    item.name.c_str(), static_cast<unsigned>(item.isDir),
                    static_cast<unsigned long long>(item.size),
                    static_cast<unsigned long long>(item.updatedAtMs));
    }
    std::printf("HAS_MORE=%u\n", static_cast<unsigned>(rs.hasMore));
    std::printf("NEXT_KEY=%s\n", rs.nextKey.c_str());
    std::printf("NEXT_ID=%llu\n", static_cast<unsigned long long>(rs.nextId));
    std::printf("TOTAL_COUNT=%llu\n",
                static_cast<unsigned long long>(rs.totalCount));
    return cm::cli::kExitOk;
}
