#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "cli_common.h"
#include "cm/base/md5.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/upload.h"

namespace {

const std::uint32_t kServerChunkSize = 4194304;

struct Options {
    std::string host;
    int port;
    std::string tel;
    std::string password;
    std::string file;
    std::string remote;
    std::uint64_t parentId;
    std::uint32_t chunkSize;
    bool resume;
    bool verifyMd5;
    std::string statePath;
    std::uint32_t stopAfter;
    bool keepTask;

    Options()
        : port(8004),
          parentId(0),
          chunkSize(kServerChunkSize),
          resume(false),
          verifyMd5(false),
          stopAfter(0),
          keepTask(false) {
    }
};

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --host H --port P --tel T --password P --file F "
                 "[--remote NAME]\n"
                 "          [--parent-id N] [-c BYTES] [-r] [--md5] "
                 "[--state PATH]\n"
                 "          [--stop-after-chunks N] [--keep-task]\n"
                 "\n"
                 "  -r                      resume from --state file\n"
                 "  --md5                   recompute local MD5 (default: reuse "
                 "state on -r)\n"
                 "  --stop-after-chunks N   _exit(9) after N acks in this run\n"
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
        } else if (arg == "--file" && hasValue) {
            opt->file = argv[++i];
        } else if (arg == "--remote" && hasValue) {
            opt->remote = argv[++i];
        } else if (arg == "--parent-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->parentId)) {
                return false;
            }
        } else if (arg == "-c" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->chunkSize) ||
                opt->chunkSize == 0) {
                return false;
            }
        } else if (arg == "-r") {
            opt->resume = true;
        } else if (arg == "--md5") {
            opt->verifyMd5 = true;
        } else if (arg == "--state" && hasValue) {
            opt->statePath = argv[++i];
        } else if (arg == "--stop-after-chunks" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->stopAfter)) {
                return false;
            }
        } else if (arg == "--keep-task") {
            opt->keepTask = true;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return true;
}

struct UploadState {
    std::string taskId;
    std::string fileMd5;
    std::uint64_t fileSize;
    std::uint32_t chunkSize;
    std::uint32_t totalChunks;
    std::string bitmap;

    UploadState()
        : fileSize(0), chunkSize(0), totalChunks(0) {
    }
};

std::string serializeState(const UploadState& state) {
    std::string out;
    out += "version=1\n";
    out += "task_id=" + state.taskId + "\n";
    out += "file_md5=" + state.fileMd5 + "\n";
    out += "file_size=" + std::to_string(state.fileSize) + "\n";
    out += "chunk_size=" + std::to_string(state.chunkSize) + "\n";
    out += "total_chunks=" + std::to_string(state.totalChunks) + "\n";
    out += "bitmap=" + cm::cli::bitmapToHex(state.bitmap) + "\n";
    return out;
}

bool parseKeyValues(const std::string& text,
                    std::vector<std::pair<std::string, std::string> >* out) {
    out->clear();
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(begin, end - begin);
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string value = line.substr(eq + 1);
            if (!value.empty() && value[value.size() - 1] == '\r') {
                value.erase(value.size() - 1);
            }
            out->push_back(std::make_pair(line.substr(0, eq), value));
        }
        if (end == text.size()) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool loadState(const std::string& path, UploadState* state) {
    std::string text;
    if (!cm::cli::readFileToString(path, &text)) {
        return false;
    }
    std::vector<std::pair<std::string, std::string> > kv;
    parseKeyValues(text, &kv);
    std::string bitmapHex;
    for (std::size_t i = 0; i < kv.size(); ++i) {
        const std::string& key = kv[i].first;
        const std::string& value = kv[i].second;
        if (key == "task_id") {
            state->taskId = value;
        } else if (key == "file_md5") {
            state->fileMd5 = value;
        } else if (key == "file_size") {
            if (!cm::cli::parseU64(value, &state->fileSize)) {
                return false;
            }
        } else if (key == "chunk_size") {
            if (!cm::cli::parseU32(value, &state->chunkSize)) {
                return false;
            }
        } else if (key == "total_chunks") {
            if (!cm::cli::parseU32(value, &state->totalChunks)) {
                return false;
            }
        } else if (key == "bitmap") {
            bitmapHex = value;
        }
    }
    if (state->taskId.empty() || state->totalChunks == 0 ||
        state->chunkSize == 0) {
        return false;
    }
    return cm::cli::bitmapFromHex(bitmapHex, &state->bitmap);
}

bool expectType(const cm::protocol::Frame& frame,
                cm::protocol::MessageType type) {
    return frame.type == static_cast<std::uint16_t>(type);
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
    if (opt.host.empty() || opt.tel.empty() || opt.password.empty() ||
        opt.file.empty()) {
        printUsage(argv[0]);
        return cm::cli::kExitUsage;
    }
    if (opt.remote.empty()) {
        opt.remote = cm::cli::baseName(opt.file);
    }
    if (opt.statePath.empty()) {
        opt.statePath = opt.file + ".state";
    }

    struct stat st;
    if (::stat(opt.file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0) {
        std::fprintf(stderr, "invalid --file: %s\n", opt.file.c_str());
        return cm::cli::kExitUsage;
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(st.st_size);

    UploadState state;
    if (opt.resume) {
        if (!loadState(opt.statePath, &state)) {
            std::fprintf(stderr, "resume requires a valid state file: %s\n",
                         opt.statePath.c_str());
            return cm::cli::kExitUsage;
        }
        if (state.fileSize != fileSize) {
            std::fprintf(stderr, "state size mismatch: %llu vs %llu\n",
                         static_cast<unsigned long long>(state.fileSize),
                         static_cast<unsigned long long>(fileSize));
            return cm::cli::kExitUsage;
        }
    }

    std::string md5;
    if (!opt.resume || opt.verifyMd5 || state.fileMd5.empty()) {
        std::string error;
        if (!cm::cli::md5File(opt.file, &md5, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return cm::cli::kExitUsage;
        }
        if (opt.resume && !state.fileMd5.empty() && md5 != state.fileMd5) {
            std::fprintf(stderr, "local file changed since state was written\n");
            return cm::cli::kExitUsage;
        }
    } else {
        md5 = state.fileMd5;
    }

    std::string error;
    int exitCode = cm::cli::kExitNetwork;
    const int fd = cm::cli::connectAndLogin(opt.host, opt.port, opt.tel,
                                            opt.password, &error, &exitCode);
    if (fd < 0) {
        std::fprintf(stderr, "login failed: %s\n", error.c_str());
        return exitCode;
    }

    cm::Buffer buffer;
    cm::protocol::Frame frame;

    std::uint32_t totalChunks = 0;
    std::uint32_t chunkSize = 0;
    if (opt.resume) {
        cm::protocol::ContinueUploadRq rq;
        rq.taskId = state.taskId;
        std::string payload;
        if (!rq.encode(&payload) ||
            !cm::cli::sendFrame(
                fd,
                static_cast<std::uint16_t>(
                    cm::protocol::MessageType::ContinueUploadRq),
                1, payload, &error) ||
            !cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
            std::fprintf(stderr, "continue failed: %s\n", error.c_str());
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (frame.type == static_cast<std::uint16_t>(
                               cm::protocol::MessageType::ErrorRs)) {
            std::uint32_t code = 0;
            std::string message;
            cm::cli::decodeErrorRs(frame, &code, &message);
            std::fprintf(stderr, "continue rejected: code=%u %s\n", code,
                         message.c_str());
            ::close(fd);
            return cm::cli::kExitRejected;
        }
        cm::protocol::ContinueUploadRs rs;
        if (!expectType(frame,
                        cm::protocol::MessageType::ContinueUploadRs) ||
            !rs.decode(frame.payload)) {
            std::fprintf(stderr, "bad continue response\n");
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (rs.code != 0) {
            std::fprintf(stderr, "continue rejected: code=%u\n", rs.code);
            ::close(fd);
            return cm::cli::kExitRejected;
        }
        totalChunks = rs.totalChunks;
        chunkSize = rs.chunkSize;
        state.taskId = rs.taskId;
        state.bitmap = rs.bitmap;
        state.totalChunks = totalChunks;
        state.chunkSize = chunkSize;
        state.fileSize = fileSize;
        state.fileMd5 = md5;
        if (state.bitmap.size() < (totalChunks + 7) / 8) {
            state.bitmap.resize((totalChunks + 7) / 8, '\0');
        }
    } else {
        cm::protocol::UploadInitRq rq;
        rq.parentId = opt.parentId;
        rq.fileName = opt.remote;
        rq.totalSize = fileSize;
        rq.fileMd5 = md5;
        rq.chunkSize = opt.chunkSize;
        rq.totalChunks = static_cast<std::uint32_t>(
            (fileSize + kServerChunkSize - 1) / kServerChunkSize);
        rq.flags = 1;
        rq.reqId = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(std::time(nullptr)) << 8) ^
            static_cast<std::uint64_t>(::getpid()) ^
            static_cast<std::uint64_t>(fileSize & 0xffff));
        if (rq.reqId == 0) {
            rq.reqId = 1;
        }

        if (opt.chunkSize != kServerChunkSize) {
            std::fprintf(stderr,
                         "warning: server fixes chunk size at 4194304, "
                         "ignoring -c %u\n",
                         opt.chunkSize);
        }

        std::string payload;
        if (!rq.encode(&payload) ||
            !cm::cli::sendFrame(
                fd,
                static_cast<std::uint16_t>(
                    cm::protocol::MessageType::UploadInitRq),
                1, payload, &error) ||
            !cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
            std::fprintf(stderr, "init failed: %s\n", error.c_str());
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (frame.type == static_cast<std::uint16_t>(
                               cm::protocol::MessageType::ErrorRs)) {
            std::uint32_t code = 0;
            std::string message;
            cm::cli::decodeErrorRs(frame, &code, &message);
            std::fprintf(stderr, "init rejected: code=%u %s\n", code,
                         message.c_str());
            ::close(fd);
            return cm::cli::kExitRejected;
        }
        cm::protocol::UploadInitRs rs;
        if (!expectType(frame, cm::protocol::MessageType::UploadInitRs) ||
            !rs.decode(frame.payload)) {
            std::fprintf(stderr, "bad init response\n");
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (rs.code != 0) {
            std::fprintf(stderr, "init rejected: code=%u\n", rs.code);
            ::close(fd);
            return cm::cli::kExitRejected;
        }
        if (rs.result == 1) {
            std::printf("STATUS=INSTANT_HIT\n");
            std::printf("TASK_ID=%s\n", rs.taskId.c_str());
            std::printf("FILE_ID=%llu\n",
                        static_cast<unsigned long long>(rs.fileId));
            std::printf("UF_ID=%llu\n",
                        static_cast<unsigned long long>(rs.ufId));
            std::printf("FILE_NAME=%s\n", rs.fileName.c_str());
            std::printf("FILE_MD5=%s\n", md5.c_str());
            std::printf("SIZE=%llu\n",
                        static_cast<unsigned long long>(fileSize));
            ::close(fd);
            if (!opt.keepTask) {
                ::unlink(opt.statePath.c_str());
            }
            return cm::cli::kExitOk;
        }

        totalChunks = rs.totalChunks;
        chunkSize = rs.chunkSize;
        if (chunkSize == 0 || totalChunks == 0) {
            std::fprintf(stderr, "server returned empty task geometry\n");
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        state.taskId = rs.taskId;
        state.fileMd5 = md5;
        state.fileSize = fileSize;
        state.chunkSize = chunkSize;
        state.totalChunks = totalChunks;
        state.bitmap.assign((totalChunks + 7) / 8, '\0');
        if (!cm::cli::writeFileAtomic(opt.statePath, serializeState(state),
                                      &error)) {
            std::fprintf(stderr, "write state failed: %s\n", error.c_str());
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
    }

    std::printf("TASK_ID=%s\n", state.taskId.c_str());
    std::printf("TOTAL_CHUNKS=%u\n", totalChunks);
    std::printf("CHUNK_SIZE=%u\n", chunkSize);

    const int fileFd = ::open(opt.file.c_str(), O_RDONLY);
    if (fileFd < 0) {
        std::fprintf(stderr, "open file failed: %s\n", std::strerror(errno));
        ::close(fd);
        return cm::cli::kExitUsage;
    }

    std::vector<char> chunk(chunkSize);
    std::uint32_t sentThisRun = 0;
    std::uint32_t seq = 1;
    for (std::uint32_t index = 0; index < totalChunks; ++index) {
        if (cm::cli::bitmapTest(state.bitmap, index)) {
            continue;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(index) *
                                     static_cast<std::uint64_t>(chunkSize);
        const std::uint64_t remain = fileSize - offset;
        const std::size_t len = static_cast<std::size_t>(
            remain < chunkSize ? remain : chunkSize);
        if (!cm::cli::preadExact(fileFd, &chunk[0], len,
                                 static_cast<off_t>(offset), &error)) {
            std::fprintf(stderr, "read file failed: %s\n", error.c_str());
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitUsage;
        }

        cm::protocol::UploadChunkRq rq;
        rq.taskId = state.taskId;
        rq.chunkIndex = index;
        rq.offset = offset;
        rq.chunkMd5 = cm::md5Hex(&chunk[0], len);
        rq.data.assign(&chunk[0], len);

        std::string payload;
        if (!rq.encode(&payload)) {
            std::fprintf(stderr, "encode chunk %u failed\n", index);
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitUsage;
        }
        ++seq;
        if (!cm::cli::sendFrame(
                fd,
                static_cast<std::uint16_t>(
                    cm::protocol::MessageType::UploadChunkRq),
                seq, payload, &error) ||
            !cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
            std::fprintf(stderr, "chunk %u failed: %s\n", index,
                         error.c_str());
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (frame.seq != seq) {
            std::fprintf(stderr, "unexpected seq %u (want %u)\n", frame.seq,
                         seq);
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (frame.type == static_cast<std::uint16_t>(
                               cm::protocol::MessageType::ErrorRs)) {
            std::uint32_t code = 0;
            std::string message;
            cm::cli::decodeErrorRs(frame, &code, &message);
            std::fprintf(stderr, "chunk %u rejected: code=%u %s\n", index,
                         code, message.c_str());
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitRejected;
        }
        cm::protocol::UploadChunkRs rs;
        if (!expectType(frame, cm::protocol::MessageType::UploadChunkRs) ||
            !rs.decode(frame.payload)) {
            std::fprintf(stderr, "bad chunk response\n");
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (rs.code != 0) {
            std::fprintf(stderr, "chunk %u rejected: code=%u\n", index,
                         rs.code);
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitRejected;
        }

        cm::cli::bitmapSet(&state.bitmap, index);
        ++sentThisRun;
        if (!cm::cli::writeFileAtomic(opt.statePath, serializeState(state),
                                      &error)) {
            std::fprintf(stderr, "write state failed: %s\n", error.c_str());
            ::close(fileFd);
            ::close(fd);
            return cm::cli::kExitNetwork;
        }
        if (opt.stopAfter > 0 && sentThisRun >= opt.stopAfter) {
            std::printf("STATUS=INTERRUPTED\n");
            std::printf("SENT_CHUNKS=%u\n", sentThisRun);
            std::fflush(stdout);
            ::close(fileFd);
            ::close(fd);
            _exit(cm::cli::kExitInterrupted);
        }
    }
    ::close(fileFd);

    cm::protocol::UploadCompleteRq done;
    done.taskId = state.taskId;
    std::string donePayload;
    if (!done.encode(&donePayload) ||
        !cm::cli::sendFrame(
            fd,
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::UploadCompleteRq),
        ++seq, donePayload, &error) ||
        !cm::cli::recvFrame(fd, &buffer, &frame, &error)) {
        std::fprintf(stderr, "complete failed: %s\n", error.c_str());
        ::close(fd);
        return cm::cli::kExitNetwork;
    }
    if (frame.type == static_cast<std::uint16_t>(
                           cm::protocol::MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        cm::cli::decodeErrorRs(frame, &code, &message);
        std::fprintf(stderr, "complete rejected: code=%u %s\n", code,
                     message.c_str());
        ::close(fd);
        return cm::cli::kExitRejected;
    }
    cm::protocol::UploadCompleteRs complete;
    if (!expectType(frame, cm::protocol::MessageType::UploadCompleteRs) ||
        !complete.decode(frame.payload)) {
        std::fprintf(stderr, "bad complete response\n");
        ::close(fd);
        return cm::cli::kExitNetwork;
    }
    if (complete.code != 0) {
        std::fprintf(stderr, "complete rejected: code=%u\n", complete.code);
        ::close(fd);
        return cm::cli::kExitRejected;
    }

    std::printf("STATUS=OK\n");
    std::printf("FILE_ID=%llu\n",
                static_cast<unsigned long long>(complete.fileId));
    std::printf("UF_ID=%llu\n",
                static_cast<unsigned long long>(complete.ufId));
    std::printf("FILE_NAME=%s\n", complete.fileName.c_str());
    std::printf("FILE_MD5=%s\n", complete.fileMd5.c_str());
    std::printf("SIZE=%llu\n",
                static_cast<unsigned long long>(complete.size));
    std::printf("BAD_INDEXES=%zu\n", complete.badIndexes.size());
    ::close(fd);
    if (!opt.keepTask) {
        ::unlink(opt.statePath.c_str());
    }
    return cm::cli::kExitOk;
}
