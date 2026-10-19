#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cli_common.h"
#include "cm/base/md5.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/download.h"

namespace {

struct Options {
    std::string host;
    int port;
    std::string tel;
    std::string password;
    std::uint64_t ufId;
    std::string out;
    int jobs;
    std::uint32_t chunkSize;
    bool resume;
    bool verifyMd5;
    std::uint32_t stopAfter;
    std::uint32_t rateKb;

    Options()
        : port(8004),
          ufId(0),
          jobs(4),
          chunkSize(4194304),
          resume(false),
          verifyMd5(false),
          stopAfter(0),
          rateKb(0) {
    }
};

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s --host H --port P --tel T --password P --uf-id N "
                 "--out FILE\n"
                 "          [-j CONN] [-c BYTES] [-r] [--md5] "
                 "[--stop-after-blocks N] [--rate KBPS]\n"
                 "\n"
                 "  -j N                    concurrent connections (default 4)\n"
                 "  -r                      resume from <out>.part.state\n"
                 "  --stop-after-blocks N   _exit(9) after N blocks in this run\n"
                 "  --rate KBPS             overall throttle (split across -j)\n"
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
        } else if (arg == "--uf-id" && hasValue) {
            if (!cm::cli::parseU64(argv[++i], &opt->ufId) || opt->ufId == 0) {
                return false;
            }
        } else if (arg == "--out" && hasValue) {
            opt->out = argv[++i];
        } else if (arg == "-j" && hasValue) {
            if (!cm::cli::parsePositiveInt(argv[++i], &opt->jobs) ||
                opt->jobs > 32) {
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
        } else if (arg == "--stop-after-blocks" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->stopAfter)) {
                return false;
            }
        } else if (arg == "--rate" && hasValue) {
            if (!cm::cli::parseU32(argv[++i], &opt->rateKb)) {
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

struct PartState {
    std::uint64_t size;
    std::string fileMd5;
    std::uint32_t blockSize;
    std::uint32_t blockCount;
    std::string bitmap;

    PartState()
        : size(0), blockSize(0), blockCount(0) {
    }
};

void parseKeyValues(const std::string& text,
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
}

bool loadState(const std::string& path, PartState* state) {
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
        if (key == "file_size") {
            if (!cm::cli::parseU64(value, &state->size)) {
                return false;
            }
        } else if (key == "file_md5") {
            state->fileMd5 = value;
        } else if (key == "block_size") {
            if (!cm::cli::parseU32(value, &state->blockSize)) {
                return false;
            }
        } else if (key == "block_count") {
            if (!cm::cli::parseU32(value, &state->blockCount)) {
                return false;
            }
        } else if (key == "bitmap") {
            bitmapHex = value;
        }
    }
    if (state->blockCount == 0 || state->blockSize == 0) {
        return false;
    }
    return cm::cli::bitmapFromHex(bitmapHex, &state->bitmap);
}

std::string serializeState(const PartState& state) {
    std::string out;
    out += "version=1\n";
    out += "file_size=" + std::to_string(state.size) + "\n";
    out += "file_md5=" + state.fileMd5 + "\n";
    out += "block_size=" + std::to_string(state.blockSize) + "\n";
    out += "block_count=" + std::to_string(state.blockCount) + "\n";
    out += "bitmap=" + cm::cli::bitmapToHex(state.bitmap) + "\n";
    return out;
}

struct Shared {
    std::mutex mutex;
    PartState state;
    std::atomic<std::uint32_t> nextIndex;
    std::atomic<bool> failed;
    std::atomic<int> exitCode;
    std::uint32_t completedThisRun;
    std::uint32_t stopAfter;
    std::string error;

    Shared()
        : nextIndex(0), failed(false), exitCode(cm::cli::kExitNetwork),
          completedThisRun(0), stopAfter(0) {
    }
};

void throttle(std::uint32_t bytes, std::uint32_t rateKb, int jobs) {
    if (rateKb == 0 || jobs <= 0) {
        return;
    }
    const std::uint64_t perConn = static_cast<std::uint64_t>(rateKb) * 1024ULL /
                                  static_cast<std::uint64_t>(jobs);
    if (perConn == 0) {
        return;
    }
    const std::uint64_t ms = static_cast<std::uint64_t>(bytes) * 1000ULL / perConn;
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
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
        opt.ufId == 0 || opt.out.empty()) {
        printUsage(argv[0]);
        return cm::cli::kExitUsage;
    }

    const std::string partPath = opt.out + ".part";
    const std::string statePath = opt.out + ".part.state";

    std::string error;
    int exitCode = cm::cli::kExitNetwork;
    int mainFd = cm::cli::connectAndLogin(opt.host, opt.port, opt.tel,
                                          opt.password, &error, &exitCode);
    if (mainFd < 0) {
        std::fprintf(stderr, "login failed: %s\n", error.c_str());
        return exitCode;
    }

    cm::Buffer mainBuffer;
    cm::protocol::Frame mainFrame;

    cm::protocol::DownloadOpenRq openRq;
    openRq.ufId = opt.ufId;
    openRq.startBlock = 0;
    std::string payload;
    if (!openRq.encode(&payload) ||
        !cm::cli::sendFrame(
            mainFd,
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadOpenRq),
            1, payload, &error) ||
        !cm::cli::recvFrame(mainFd, &mainBuffer, &mainFrame, &error)) {
        std::fprintf(stderr, "open failed: %s\n", error.c_str());
        ::close(mainFd);
        return cm::cli::kExitNetwork;
    }
    if (mainFrame.type == static_cast<std::uint16_t>(
                              cm::protocol::MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        cm::cli::decodeErrorRs(mainFrame, &code, &message);
        std::fprintf(stderr, "open rejected: code=%u %s\n", code,
                     message.c_str());
        ::close(mainFd);
        return cm::cli::kExitRejected;
    }
    cm::protocol::DownloadOpenRs openRs;
    if (mainFrame.type != static_cast<std::uint16_t>(
                              cm::protocol::MessageType::DownloadOpenRs) ||
        !openRs.decode(mainFrame.payload)) {
        std::fprintf(stderr, "bad open response\n");
        ::close(mainFd);
        return cm::cli::kExitNetwork;
    }
    if (openRs.code != 0) {
        std::fprintf(stderr, "open rejected: code=%u\n", openRs.code);
        ::close(mainFd);
        return cm::cli::kExitRejected;
    }
    if (openRs.blockSize == 0 || openRs.blockCount == 0 ||
        openRs.size == 0) {
        std::fprintf(stderr, "server returned empty geometry\n");
        ::close(mainFd);
        return cm::cli::kExitNetwork;
    }

    Shared shared;
    shared.stopAfter = opt.stopAfter;
    shared.state.size = openRs.size;
    shared.state.fileMd5 = openRs.fileMd5;
    shared.state.blockSize = openRs.blockSize;
    shared.state.blockCount = openRs.blockCount;
    shared.state.bitmap.assign((openRs.blockCount + 7) / 8, '\0');

    bool fresh = true;
    if (opt.resume) {
        PartState loaded;
        if (loadState(statePath, &loaded) && loaded.size == openRs.size &&
            loaded.fileMd5 == openRs.fileMd5 &&
            loaded.blockSize == openRs.blockSize &&
            loaded.blockCount == openRs.blockCount) {
            shared.state.bitmap = loaded.bitmap;
            if (shared.state.bitmap.size() < (openRs.blockCount + 7) / 8) {
                shared.state.bitmap.resize((openRs.blockCount + 7) / 8, '\0');
            }
            fresh = false;
        } else {
            std::fprintf(stderr,
                         "warning: state mismatch, starting from scratch\n");
        }
    }

    const int partFd = ::open(partPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (partFd < 0) {
        std::fprintf(stderr, "open %s failed: %s\n", partPath.c_str(),
                     std::strerror(errno));
        ::close(mainFd);
        return cm::cli::kExitUsage;
    }
    if (fresh && ::ftruncate(partFd, static_cast<off_t>(openRs.size)) != 0) {
        std::fprintf(stderr, "ftruncate failed: %s\n", std::strerror(errno));
        ::close(partFd);
        ::close(mainFd);
        return cm::cli::kExitUsage;
    }
    if (fresh &&
        !cm::cli::writeFileAtomic(statePath, serializeState(shared.state),
                                  &error)) {
        std::fprintf(stderr, "write state failed: %s\n", error.c_str());
        ::close(partFd);
        ::close(mainFd);
        return cm::cli::kExitUsage;
    }

    std::printf("FILE_ID=%llu\n", static_cast<unsigned long long>(openRs.fId));
    std::printf("SIZE=%llu\n", static_cast<unsigned long long>(openRs.size));
    std::printf("FILE_MD5=%s\n", openRs.fileMd5.c_str());
    std::printf("BLOCK_SIZE=%u\n", openRs.blockSize);
    std::printf("BLOCK_COUNT=%u\n", openRs.blockCount);
    std::printf("CONNECTIONS=%d\n", opt.jobs);

    const int jobs = opt.jobs;
    std::vector<std::thread> threads;

    // 每个 worker 独占一条连接与自己的 Buffer；主连接作为 0 号 worker。
    for (int worker = 1; worker < jobs; ++worker) {
        threads.push_back(std::thread([&, worker]() {
            (void)worker;
            std::string err;
            int code = cm::cli::kExitNetwork;
            const int fd = cm::cli::connectAndLogin(
                opt.host, opt.port, opt.tel, opt.password, &err, &code);
            if (fd < 0) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (!shared.failed.exchange(true)) {
                    shared.error = "worker login failed: " + err;
                    shared.exitCode.store(code);
                }
                return;
            }

            cm::Buffer buffer;
            cm::protocol::Frame frame;
            for (;;) {
                if (shared.failed.load()) {
                    break;
                }
                const std::uint32_t index = shared.nextIndex.fetch_add(1);
                if (index >= shared.state.blockCount) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (cm::cli::bitmapTest(shared.state.bitmap, index)) {
                        continue;
                    }
                }

                const std::uint64_t offset =
                    static_cast<std::uint64_t>(index) *
                    static_cast<std::uint64_t>(shared.state.blockSize);
                const std::uint64_t remain = shared.state.size - offset;
                const std::size_t expected = static_cast<std::size_t>(
                    remain < shared.state.blockSize ? remain
                                                    : shared.state.blockSize);

                cm::protocol::DownloadRangeRq rq;
                rq.ticket = openRs.ticket;
                rq.fId = openRs.fId;
                rq.blockIndex = index;
                std::string rangePayload;
                if (!rq.encode(&rangePayload)) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "encode range failed";
                        shared.exitCode.store(cm::cli::kExitUsage);
                    }
                    break;
                }
                const std::uint32_t seq = index + 1;
                if (!cm::cli::sendFrame(
                        fd,
                        static_cast<std::uint16_t>(
                            cm::protocol::MessageType::DownloadRangeRq),
                        seq, rangePayload, &err) ||
                    !cm::cli::recvFrame(fd, &buffer, &frame, &err)) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "block " + std::to_string(index) +
                                       " failed: " + err;
                        shared.exitCode.store(cm::cli::kExitNetwork);
                    }
                    break;
                }
                if (frame.seq != seq) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "unexpected seq for block " +
                                       std::to_string(index);
                        shared.exitCode.store(cm::cli::kExitNetwork);
                    }
                    break;
                }
                if (frame.type == static_cast<std::uint16_t>(
                                       cm::protocol::MessageType::ErrorRs)) {
                    std::uint32_t code = 0;
                    std::string message;
                    cm::cli::decodeErrorRs(frame, &code, &message);
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "block " + std::to_string(index) +
                                       " rejected: code=" +
                                       std::to_string(code) + " " + message;
                        shared.exitCode.store(cm::cli::kExitRejected);
                    }
                    break;
                }
                if (frame.type != static_cast<std::uint16_t>(
                                       cm::protocol::MessageType::DownloadDataRs) ||
                    frame.payload.size() != expected) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "bad block payload for " +
                                       std::to_string(index);
                        shared.exitCode.store(cm::cli::kExitNetwork);
                    }
                    break;
                }
                if (!cm::cli::pwriteAll(partFd, frame.payload.data(),
                                        frame.payload.size(),
                                        static_cast<off_t>(offset), &err)) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    if (!shared.failed.exchange(true)) {
                        shared.error = "write block failed: " + err;
                        shared.exitCode.store(cm::cli::kExitUsage);
                    }
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    cm::cli::bitmapSet(&shared.state.bitmap, index);
                    ++shared.completedThisRun;
                    if (!cm::cli::writeFileAtomic(
                            statePath, serializeState(shared.state), &err)) {
                        if (!shared.failed.exchange(true)) {
                            shared.error = "write state failed: " + err;
                            shared.exitCode.store(cm::cli::kExitUsage);
                        }
                        break;
                    }
                    if (shared.stopAfter > 0 &&
                        shared.completedThisRun >= shared.stopAfter) {
                        std::printf("STATUS=INTERRUPTED\n");
                        std::printf("BLOCKS_THIS_RUN=%u\n",
                                    shared.completedThisRun);
                        std::fflush(stdout);
                        ::close(fd);
                        _exit(cm::cli::kExitInterrupted);
                    }
                }

                throttle(static_cast<std::uint32_t>(expected), opt.rateKb, jobs);
            }
            ::close(fd);
        }));
    }

    // 0 号 worker 跑在主线程，复用已登录连接。
    {
        std::string err;
        cm::Buffer buffer;
        cm::protocol::Frame frame;
        for (;;) {
            if (shared.failed.load()) {
                break;
            }
            const std::uint32_t index = shared.nextIndex.fetch_add(1);
            if (index >= shared.state.blockCount) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (cm::cli::bitmapTest(shared.state.bitmap, index)) {
                    continue;
                }
            }

            const std::uint64_t offset =
                static_cast<std::uint64_t>(index) *
                static_cast<std::uint64_t>(shared.state.blockSize);
            const std::uint64_t remain = shared.state.size - offset;
            const std::size_t expected = static_cast<std::size_t>(
                remain < shared.state.blockSize ? remain : shared.state.blockSize);

            cm::protocol::DownloadRangeRq rq;
            rq.ticket = openRs.ticket;
            rq.fId = openRs.fId;
            rq.blockIndex = index;
            std::string rangePayload;
            const std::uint32_t seq = index + 1;
            if (!rq.encode(&rangePayload) ||
                !cm::cli::sendFrame(
                    mainFd,
                    static_cast<std::uint16_t>(
                        cm::protocol::MessageType::DownloadRangeRq),
                    seq, rangePayload, &err) ||
                !cm::cli::recvFrame(mainFd, &buffer, &frame, &err)) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (!shared.failed.exchange(true)) {
                    shared.error = "block " + std::to_string(index) +
                                   " failed: " + err;
                    shared.exitCode.store(cm::cli::kExitNetwork);
                }
                break;
            }
            if (frame.seq != seq ||
                frame.type != static_cast<std::uint16_t>(
                                    cm::protocol::MessageType::DownloadDataRs) ||
                frame.payload.size() != expected) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (!shared.failed.exchange(true)) {
                    shared.error = "bad block payload for " +
                                   std::to_string(index);
                    shared.exitCode.store(cm::cli::kExitNetwork);
                }
                break;
            }
            if (!cm::cli::pwriteAll(partFd, frame.payload.data(),
                                    frame.payload.size(),
                                    static_cast<off_t>(offset), &err)) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                if (!shared.failed.exchange(true)) {
                    shared.error = "write block failed: " + err;
                    shared.exitCode.store(cm::cli::kExitUsage);
                }
                break;
            }

            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                cm::cli::bitmapSet(&shared.state.bitmap, index);
                ++shared.completedThisRun;
                if (!cm::cli::writeFileAtomic(statePath,
                                              serializeState(shared.state),
                                              &err)) {
                    if (!shared.failed.exchange(true)) {
                        shared.error = "write state failed: " + err;
                        shared.exitCode.store(cm::cli::kExitUsage);
                    }
                    break;
                }
                if (shared.stopAfter > 0 &&
                    shared.completedThisRun >= shared.stopAfter) {
                    std::printf("STATUS=INTERRUPTED\n");
                    std::printf("BLOCKS_THIS_RUN=%u\n",
                                shared.completedThisRun);
                    std::fflush(stdout);
                    ::close(partFd);
                    ::close(mainFd);
                    _exit(cm::cli::kExitInterrupted);
                }
            }
            throttle(static_cast<std::uint32_t>(expected), opt.rateKb, jobs);
        }
    }

    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    if (shared.failed.load()) {
        std::fprintf(stderr, "%s\n", shared.error.c_str());
        ::close(partFd);
        ::close(mainFd);
        return shared.exitCode.load();
    }

    for (std::uint32_t i = 0; i < openRs.blockCount; ++i) {
        if (!cm::cli::bitmapTest(shared.state.bitmap, i)) {
            std::fprintf(stderr, "block %u missing after download\n", i);
            ::close(partFd);
            ::close(mainFd);
            return cm::cli::kExitNetwork;
        }
    }

    ::fsync(partFd);
    ::close(partFd);

    std::string localMd5;
    if (!cm::cli::md5File(partPath, &localMd5, &error)) {
        std::fprintf(stderr, "md5 failed: %s\n", error.c_str());
        ::close(mainFd);
        return cm::cli::kExitUsage;
    }
    const bool verifyOk = (localMd5 == openRs.fileMd5);

    cm::protocol::DownloadDoneRq done;
    done.ticket = openRs.ticket;
    done.fId = openRs.fId;
    done.size = openRs.size;
    done.fileMd5 = openRs.fileMd5;
    done.verifyOk = verifyOk ? 1 : 0;
    std::string donePayload;
    if (!done.encode(&donePayload) ||
        !cm::cli::sendFrame(
            mainFd,
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadDoneRq),
            2, donePayload, &error) ||
        !cm::cli::recvFrame(mainFd, &mainBuffer, &mainFrame, &error)) {
        std::fprintf(stderr, "done report failed: %s\n", error.c_str());
        ::close(mainFd);
        return cm::cli::kExitNetwork;
    }
    cm::protocol::DownloadDoneRs doneRs;
    if (mainFrame.type != static_cast<std::uint16_t>(
                              cm::protocol::MessageType::DownloadDoneRs) ||
        !doneRs.decode(mainFrame.payload) || doneRs.code != 0) {
        std::fprintf(stderr, "done report rejected\n");
        ::close(mainFd);
        return cm::cli::kExitRejected;
    }
    ::close(mainFd);

    if (!verifyOk) {
        std::printf("STATUS=MD5_MISMATCH\n");
        std::printf("LOCAL_MD5=%s\n", localMd5.c_str());
        std::printf("EXPECTED_MD5=%s\n", openRs.fileMd5.c_str());
        std::printf("PART=%s\n", partPath.c_str());
        return cm::cli::kExitRejected;
    }

    if (::rename(partPath.c_str(), opt.out.c_str()) != 0) {
        std::fprintf(stderr, "rename to %s failed: %s\n", opt.out.c_str(),
                     std::strerror(errno));
        return cm::cli::kExitUsage;
    }
    ::unlink(statePath.c_str());

    std::printf("STATUS=OK\n");
    std::printf("PATH=%s\n", opt.out.c_str());
    std::printf("SIZE=%llu\n", static_cast<unsigned long long>(openRs.size));
    std::printf("FILE_MD5=%s\n", localMd5.c_str());
    std::printf("BLOCKS_TOTAL=%u\n", openRs.blockCount);
    std::printf("BLOCKS_THIS_RUN=%u\n", shared.completedThisRun);
    return cm::cli::kExitOk;
}
