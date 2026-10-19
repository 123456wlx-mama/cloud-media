#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "cli_common.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/recycle.h"

namespace {

struct Options {
    std::string host;
    int port;
    std::string tel;
    std::string password;
    std::string command;
    std::uint64_t ufId;
    std::uint32_t limit;
    std::uint64_t cursorMs;
    std::uint64_t cursorId;
    std::string newName;

    Options()
        : port(8004), ufId(0), limit(0), cursorMs(0), cursorId(0) {
    }
};

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s <list|delete|restore|purge> --host H --port P "
                 "--tel T --password P\n"
                 "  list:    [--limit N] [--cursor-ms N] [--cursor-id N]\n"
                 "  delete:  --uf-id N\n"
                 "  restore: --uf-id N [--name NEW]\n"
                 "  purge:   --uf-id N\n"
                 "  password may also come from CM_CLI_PASSWORD\n",
                 program);
}

bool parseArgs(int argc, char** argv, Options* opt) {
    if (argc < 2) {
        return false;
    }
    opt->command = argv[1];
    if (opt->command != "list" && opt->command != "delete" &&
        opt->command != "restore" && opt->command != "purge") {
        return false;
    }
    for (int i = 2; i < argc; ++i) {
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
        } else if (arg == "--uf-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->ufId) || opt->ufId == 0) {
                return false;
            }
        } else if (arg == "--limit" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->limit) ||
                opt->limit == 0 || opt->limit > 200) {
                return false;
            }
        } else if (arg == "--cursor-ms" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->cursorMs)) {
                return false;
            }
        } else if (arg == "--cursor-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->cursorId)) {
                return false;
            }
        } else if (arg == "--name" && hasValue) {
            opt->newName = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            return false;
        }
    }
    if (opt->command != "list" && opt->ufId == 0) {
        return false;
    }
    if (opt->command == "list" && opt->limit == 0) {
        opt->limit = 50;
    }
    return true;
}

// 统一发送 + 收包；返回 true 表示 Rs 已解码（code 可能非 0）。
template <typename Rq, typename Rs>
bool callOnce(const Options& opt, cm::protocol::MessageType reqType,
              cm::protocol::MessageType rsType, const Rq& rq, Rs* rs) {
    std::string error;
    int exitCode = cm::cli::kExitNetwork;
    const int fd = cm::cli::connectAndLogin(opt.host, opt.port, opt.tel,
                                            opt.password, &error, &exitCode);
    if (fd < 0) {
        std::fprintf(stderr, "login failed: %s\n", error.c_str());
        std::printf("CODE=2\n");
        return false;
    }

    std::string payload;
    const bool sent =
        rq.encode(&payload) &&
        cm::cli::sendFrame(fd, static_cast<std::uint16_t>(reqType), 1, payload,
                           &error);
    cm::Buffer buffer;
    cm::protocol::Frame frame;
    if (!sent || !cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
        std::fprintf(stderr, "request failed: %s\n", error.c_str());
        ::close(fd);
        std::printf("CODE=3\n");
        return false;
    }
    ::close(fd);

    if (frame.type == static_cast<std::uint16_t>(
                          cm::protocol::MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        cm::cli::decodeErrorRs(frame, &code, &message);
        std::printf("CODE=%u\n", code);
        std::fprintf(stderr, "rejected: %s\n", message.c_str());
        return false;
    }
    if (frame.type != static_cast<std::uint16_t>(rsType) ||
        !rs->decode(frame.payload)) {
        std::fprintf(stderr, "bad response type\n");
        std::printf("CODE=3\n");
        return false;
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

    if (opt.command == "list") {
        cm::protocol::RecycleListRq rq;
        rq.limit = opt.limit;
        rq.cursorDeletedMs = opt.cursorMs;
        rq.cursorId = opt.cursorId;
        cm::protocol::RecycleListRs rs;
        if (!callOnce(opt, cm::protocol::MessageType::RecycleListRq,
                      cm::protocol::MessageType::RecycleListRs, rq, &rs)) {
            return cm::cli::kExitRejected;
        }
        if (rs.code != 0) {
            std::printf("CODE=%u\n", rs.code);
            return cm::cli::kExitRejected;
        }
        for (std::size_t i = 0; i < rs.items.size(); ++i) {
            const cm::protocol::RecycleItem& item = rs.items[i];
            std::printf("UF_ID=%llu\tNAME=%s\tIS_DIR=%u\tSIZE=%llu\t"
                        "DELETED_AT=%llu\tPARENT_ID=%llu\n",
                        static_cast<unsigned long long>(item.ufId),
                        item.name.c_str(),
                        static_cast<unsigned>(item.isDir),
                        static_cast<unsigned long long>(item.size),
                        static_cast<unsigned long long>(item.deletedAtMs),
                        static_cast<unsigned long long>(item.parentId));
        }
        std::printf("HAS_MORE=%u\n", static_cast<unsigned>(rs.hasMore));
        std::printf("NEXT_MS=%llu\n",
                    static_cast<unsigned long long>(rs.nextDeletedMs));
        std::printf("NEXT_ID=%llu\n",
                    static_cast<unsigned long long>(rs.nextId));
        std::printf("CODE=0\n");
        return cm::cli::kExitOk;
    }

    if (opt.command == "delete") {
        cm::protocol::DeleteFileRq rq;
        rq.ufId = opt.ufId;
        cm::protocol::DeleteFileRs rs;
        if (!callOnce(opt, cm::protocol::MessageType::DeleteFileRq,
                      cm::protocol::MessageType::DeleteFileRs, rq, &rs)) {
            return cm::cli::kExitRejected;
        }
        std::printf("CODE=%u\n", rs.code);
        std::printf("DELETED=%u\n", static_cast<unsigned>(rs.deleted));
        std::printf("ALREADY=%u\n", static_cast<unsigned>(rs.already));
        return rs.code == 0 ? cm::cli::kExitOk : cm::cli::kExitRejected;
    }

    if (opt.command == "restore") {
        cm::protocol::RestoreRq rq;
        rq.ufId = opt.ufId;
        rq.newName = opt.newName;
        rq.autoRename = 1;
        cm::protocol::RestoreRs rs;
        if (!callOnce(opt, cm::protocol::MessageType::RestoreRq,
                      cm::protocol::MessageType::RestoreRs, rq, &rs)) {
            return cm::cli::kExitRejected;
        }
        std::printf("CODE=%u\n", rs.code);
        std::printf("UF_ID=%llu\n",
                    static_cast<unsigned long long>(rs.ufId));
        std::printf("PARENT_ID=%llu\n",
                    static_cast<unsigned long long>(rs.parentId));
        std::printf("NAME=%s\n", rs.name.c_str());
        std::printf("RENAMED=%u\n", static_cast<unsigned>(rs.renamed));
        return rs.code == 0 ? cm::cli::kExitOk : cm::cli::kExitRejected;
    }

    cm::protocol::PurgeRq rq;
    rq.ufId = opt.ufId;
    rq.confirm = 1;
    cm::protocol::PurgeRs rs;
    if (!callOnce(opt, cm::protocol::MessageType::PurgeRq,
                  cm::protocol::MessageType::PurgeRs, rq, &rs)) {
        return cm::cli::kExitRejected;
    }
    std::printf("CODE=%u\n", rs.code);
    std::printf("FILES_DELETED=%u\n", rs.filesDeleted);
    std::printf("DIRS_DELETED=%u\n", rs.dirsDeleted);
    std::printf("BLOBS_ORPHANED=%u\n", rs.blobsOrphaned);
    return rs.code == 0 ? cm::cli::kExitOk : cm::cli::kExitRejected;
}
