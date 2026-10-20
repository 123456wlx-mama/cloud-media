// 业务压测客户端（协议 v2 直连，不 spawn CLI）。
//
// 骨架沿用 bench_conn：每线程独立 epoll、连接分组、单连接同一时刻只有一个在途请求。
// 业务层直接组 v2 帧：登录 / 上传(init+chunk+complete) / 下载(open+range+done) / 列表。
//
// 用法：
//   bencher -op upload|download|list|login -c 1000 -t 4 -size 10 -duration 60
//           [-rate QPS] [-warmup 10] [-runs 1] [--inflight 64] [--csv out.csv]
//           [--setup-files 2] [-h 127.0.0.1] [-p 8004] [--tel T] [--password P]
//   bencher -op connect -c 8000 -t 4        # 建连风暴：快速建连后立即关闭
//
// 语义：
//   一次请求 = 一次完整文件上传 / 一次完整文件下载 / 一次列表 / 一次登录。
//   大块请求（upload/download）受全局在途上限 --inflight 约束（默认 64）：
//   2 vCPU + 固定 4MB 分片下不限流会把 4GB 内存的 VM 打爆
//   （客户端与服务端各约 12MB/在途分片）。
//   唯一内容：最后一个分片掺 seed，前缀分片全请求共享只读缓冲，
//   整文件 MD5 因此唯一，秒传不会短路。
//
// 退出码：0 成功 / 1 参数错误 / 3 环境或登录失败。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cm/base/buffer.h"
#include "cm/base/md5.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/auth.h"
#include "cm/protocol/messages/download.h"
#include "cm/protocol/messages/list.h"
#include "cm/protocol/messages/upload.h"
#include "cm/protocol/serialize.h"

namespace {

const std::uint32_t kChunkSize = 4 * 1024 * 1024;
const std::size_t kLatencyCapPerWorker = 200000;
const std::size_t kMaxEvents = 512;
const std::size_t kMaxPendingConnect = 128;
const std::uint32_t kWakeMarker = 0xFFFFFFF0u;

enum class Op { Login, Upload, Download, ListFile, Connect };

struct Options {
    int conns;
    int threads;
    Op op;
    int sizeMb;
    int durationSec;
    int warmupSec;
    int runs;
    int rateQps;
    int inflight;
    int setupFiles;
    int listLimit;
    std::string host;
    int port;
    std::string tel;
    std::string password;
    std::string csvPath;

    Options()
        : conns(100),
          threads(1),
          op(Op::Login),
          sizeMb(10),
          durationSec(30),
          warmupSec(3),
          runs(1),
          rateQps(0),
          inflight(64),
          setupFiles(2),
          listLimit(50),
          host("127.0.0.1"),
          port(8004) {}
};

std::int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* opName(Op op) {
    switch (op) {
        case Op::Login: return "login";
        case Op::Upload: return "upload";
        case Op::Download: return "download";
        case Op::ListFile: return "list";
        case Op::Connect: return "connect";
    }
    return "?";
}

bool isBigOp(Op op) { return op == Op::Upload || op == Op::Download; }

void printUsage(const char* program) {
    std::fprintf(
        stderr,
        "usage: %s -op login|upload|download|list|connect -c N -t N [-size MB]\n"
        "          [-duration S] [-warmup S] [-runs N] [-rate QPS]\n"
        "          [--inflight K] [--csv PATH] [--setup-files N] [--list-limit N]\n"
        "          [-h host] [-p port] [--tel TEL] [--password PWD]\n"
        "\n"
        "  -c            连接数（常驻长连接）\n"
        "  -t            压测线程数（每线程一个 epoll）\n"
        "  -size         单次请求的文件大小（MB），upload/download 用\n"
        "  -duration     测量时长（秒），不含 warmup\n"
        "  -warmup       预热时长（秒），warmup 内开始的请求不计入统计\n"
        "  -runs         重复轮数，每轮新建连接；末尾打印中位数\n"
        "  -rate         全局 QPS 上限（0=不限速）\n"
        "  --inflight    大块请求全局在途上限（0=不限，默认 64）\n"
        "  --setup-files download/list 前预置的文件个数\n"
        "\n"
        "  password 也可由 CM_CLI_PASSWORD 提供，tel 默认取 CM_CLI_TEL。\n",
        program);
}

bool parseOp(const std::string& text, Op* op) {
    if (text == "login") { *op = Op::Login; return true; }
    if (text == "upload") { *op = Op::Upload; return true; }
    if (text == "download") { *op = Op::Download; return true; }
    if (text == "list") { *op = Op::ListFile; return true; }
    if (text == "connect") { *op = Op::Connect; return true; }
    return false;
}

bool parseInt(const std::string& text, int* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    if (v < -2147483647L || v > 2147483647L) {
        return false;
    }
    *value = static_cast<int>(v);
    return true;
}

bool parseArgs(int argc, char** argv, Options* opt) {
    bool haveOp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = (i + 1 < argc);
        if (arg == "-c" && hasValue) {
            if (!parseInt(argv[++i], &opt->conns) || opt->conns <= 0) return false;
        } else if (arg == "-t" && hasValue) {
            if (!parseInt(argv[++i], &opt->threads) || opt->threads <= 0) return false;
        } else if (arg == "-op" && hasValue) {
            if (!parseOp(argv[++i], &opt->op)) return false;
            haveOp = true;
        } else if (arg == "-size" && hasValue) {
            if (!parseInt(argv[++i], &opt->sizeMb) || opt->sizeMb <= 0) return false;
        } else if (arg == "-duration" && hasValue) {
            if (!parseInt(argv[++i], &opt->durationSec) || opt->durationSec <= 0) return false;
        } else if (arg == "-warmup" && hasValue) {
            if (!parseInt(argv[++i], &opt->warmupSec) || opt->warmupSec < 0) return false;
        } else if (arg == "-runs" && hasValue) {
            if (!parseInt(argv[++i], &opt->runs) || opt->runs <= 0) return false;
        } else if (arg == "-rate" && hasValue) {
            if (!parseInt(argv[++i], &opt->rateQps) || opt->rateQps < 0) return false;
        } else if (arg == "--inflight" && hasValue) {
            if (!parseInt(argv[++i], &opt->inflight) || opt->inflight < 0) return false;
        } else if (arg == "--setup-files" && hasValue) {
            if (!parseInt(argv[++i], &opt->setupFiles) || opt->setupFiles <= 0) return false;
        } else if (arg == "--list-limit" && hasValue) {
            if (!parseInt(argv[++i], &opt->listLimit) || opt->listLimit <= 0) return false;
        } else if (arg == "--csv" && hasValue) {
            opt->csvPath = argv[++i];
        } else if (arg == "-h" && hasValue) {
            opt->host = argv[++i];
        } else if (arg == "-p" && hasValue) {
            if (!parseInt(argv[++i], &opt->port) || opt->port <= 0 || opt->port > 65535) {
                return false;
            }
        } else if (arg == "--tel" && hasValue) {
            opt->tel = argv[++i];
        } else if (arg == "--password" && hasValue) {
            opt->password = argv[++i];
        } else {
            return false;
        }
    }
    if (!haveOp) {
        return false;
    }
    if (opt->password.empty()) {
        const char* env = std::getenv("CM_CLI_PASSWORD");
        if (env != nullptr) {
            opt->password = env;
        }
    }
    if (opt->tel.empty()) {
        const char* env = std::getenv("CM_CLI_TEL");
        opt->tel = env != nullptr ? std::string(env) : std::string("13800000001");
    }
    if (opt->conns < opt->threads) {
        opt->threads = opt->conns;
    }
    return !opt->password.empty();
}

// ---------------------------------------------------------------- 内容模型

// 共享前缀分片 + 每请求唯一尾片：前缀内容全请求相同（4MB 一份全局只读），
// 只有最后一个分片掺 seed，整文件 MD5 因此唯一。
struct ContentModel {
    std::uint32_t totalSize;
    std::uint32_t totalChunks;
    std::size_t tailLen;
    std::vector<char> prefix;
    std::string prefixMd5;
    cm::Md5 prefixState;

    ContentModel() : totalSize(0), totalChunks(0), tailLen(0) {}
};

void fillPattern(char* data, std::size_t len, std::uint32_t seed) {
    std::uint32_t x = seed * 2654435761u + 1013904223u;
    for (std::size_t i = 0; i < len; ++i) {
        x = x * 1664525u + 1013904223u;
        data[i] = static_cast<char>((x >> 24) & 0xffu);
    }
    if (len >= 4) {
        for (int i = 0; i < 4; ++i) {
            data[i] = static_cast<char>((seed >> (8 * i)) & 0xffu);
        }
    }
}

void initContent(const Options& opt, ContentModel* model) {
    model->totalSize = static_cast<std::uint32_t>(opt.sizeMb) * 1024u * 1024u;
    model->totalChunks = (model->totalSize + kChunkSize - 1) / kChunkSize;
    model->tailLen = model->totalSize -
                     static_cast<std::size_t>(model->totalChunks - 1) * kChunkSize;
    if (model->totalChunks > 1) {
        model->prefix.resize(kChunkSize);
        fillPattern(&model->prefix[0], kChunkSize, 0x5EED0001u);
        model->prefixMd5 = cm::md5Hex(&model->prefix[0], kChunkSize);
        for (std::uint32_t i = 0; i + 1 < model->totalChunks; ++i) {
            model->prefixState.update(&model->prefix[0], kChunkSize);
        }
    }
}

void buildTail(const ContentModel& model, std::uint32_t seed,
               std::vector<char>* tail, std::string* tailMd5,
               std::string* fileMd5) {
    tail->resize(model.tailLen);
    fillPattern(&(*tail)[0], model.tailLen, seed);
    *tailMd5 = cm::md5Hex(&(*tail)[0], model.tailLen);
    cm::Md5 state = model.prefixState;
    state.update(&(*tail)[0], model.tailLen);
    *fileMd5 = state.hex();
}

// ---------------------------------------------------------------- 阻塞客户端（预置/探测）

bool blockingConnect(const Options& opt, int* fd, std::string* error) {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opt.port));
    if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
        *error = "bad host (IPv4 literal only)";
        ::close(sock);
        return false;
    }
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
        *error = std::string("connect: ") + std::strerror(errno);
        ::close(sock);
        return false;
    }
    int one = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    *fd = sock;
    return true;
}

bool sendAll(int fd, const char* data, std::size_t len) {
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::send(fd, data + written, len - written, MSG_NOSIGNAL);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool sendBlocking(int fd, std::uint16_t type, std::uint32_t seq,
                  const std::string& payload, std::string* error) {
    std::string frame;
    if (cm::protocol::Codec::encode(type, seq, payload, &frame) != cm::ErrorCode::Ok) {
        *error = "encode failed";
        return false;
    }
    if (!sendAll(fd, frame.data(), frame.size())) {
        *error = std::string("send: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool recvBlocking(int fd, cm::Buffer* buffer, cm::protocol::Frame* frame,
                  std::string* error) {
    for (;;) {
        cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
        const cm::ErrorCode rc = cm::protocol::Codec::decode(*buffer, frame, &status);
        if (rc == cm::ErrorCode::Ok) {
            return true;
        }
        if (rc != cm::ErrorCode::NeedMore) {
            *error = "bad frame";
            return false;
        }
        char chunk[65536];
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n > 0) {
            if (!buffer->append(chunk, static_cast<std::size_t>(n))) {
                *error = "buffer overflow";
                return false;
            }
            continue;
        }
        if (n == 0) {
            *error = "peer closed";
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        *error = std::string("recv: ") + std::strerror(errno);
        return false;
    }
}

bool decodeErrorRs(const cm::protocol::Frame& frame, std::uint32_t* code,
                   std::string* message) {
    cm::protocol::Reader reader(frame.payload);
    std::string text;
    std::uint32_t value = 0;
    if (!reader.readU32(&value) || !reader.readString(&text)) {
        return false;
    }
    *code = value;
    *message = text;
    return true;
}

bool blockingLogin(const Options& opt, int* fd, std::string* error) {
    if (!blockingConnect(opt, fd, error)) {
        return false;
    }
    cm::protocol::LoginRq rq;
    rq.tel = opt.tel;
    rq.password = opt.password;
    std::string payload;
    if (!rq.encode(&payload) ||
        !sendBlocking(*fd,
                      static_cast<std::uint16_t>(cm::protocol::MessageType::LoginRq),
                      1, payload, error)) {
        ::close(*fd);
        *fd = -1;
        return false;
    }
    cm::Buffer buffer;
    cm::protocol::Frame frame;
    if (!recvBlocking(*fd, &buffer, &frame, error)) {
        ::close(*fd);
        *fd = -1;
        return false;
    }
    cm::protocol::LoginRs rs;
    if (frame.type !=
            static_cast<std::uint16_t>(cm::protocol::MessageType::LoginRs) ||
        !rs.decode(frame.payload) || rs.code != 0) {
        *error = "login rejected";
        ::close(*fd);
        *fd = -1;
        return false;
    }
    return true;
}

// 预置文件：完整走 init + chunk + complete。
bool setupOneFile(const Options& opt, const ContentModel& model, std::uint32_t seed,
                  const std::string& name, std::uint64_t* ufId, std::string* error) {
    int fd = -1;
    if (!blockingLogin(opt, &fd, error)) {
        return false;
    }
    std::vector<char> tail;
    std::string tailMd5;
    std::string fileMd5;
    buildTail(model, seed, &tail, &tailMd5, &fileMd5);

    cm::protocol::UploadInitRq init;
    init.parentId = 0;
    init.fileName = name;
    init.totalSize = model.totalSize;
    init.fileMd5 = fileMd5;
    init.chunkSize = kChunkSize;
    init.totalChunks = model.totalChunks;
    init.flags = 1;
    init.reqId = seed;
    std::string payload;
    if (!init.encode(&payload) ||
        !sendBlocking(fd,
                      static_cast<std::uint16_t>(cm::protocol::MessageType::UploadInitRq),
                      1, payload, error)) {
        ::close(fd);
        return false;
    }
    cm::Buffer buffer;
    cm::protocol::Frame frame;
    if (!recvBlocking(fd, &buffer, &frame, error)) {
        ::close(fd);
        return false;
    }
    cm::protocol::UploadInitRs initRs;
    if (frame.type !=
            static_cast<std::uint16_t>(cm::protocol::MessageType::UploadInitRs) ||
        !initRs.decode(frame.payload) || initRs.code != 0) {
        *error = "upload init rejected";
        ::close(fd);
        return false;
    }
    if (initRs.result == 1) {
        *error = "setup upload hit instant-upload (md5 collision)";
        ::close(fd);
        return false;
    }

    const std::uint32_t totalChunks = initRs.totalChunks;
    std::uint32_t seq = 1;
    for (std::uint32_t i = 0; i < totalChunks; ++i) {
        const bool last = (i + 1 == totalChunks);
        cm::protocol::UploadChunkRq rq;
        rq.taskId = initRs.taskId;
        rq.chunkIndex = i;
        rq.offset = static_cast<std::uint64_t>(i) * kChunkSize;
        const char* data = last ? &tail[0] : &model.prefix[0];
        const std::size_t len = last ? tail.size() : kChunkSize;
        rq.chunkMd5 = last ? tailMd5 : model.prefixMd5;
        rq.data.assign(data, len);
        std::string chunkPayload;
        if (!rq.encode(&chunkPayload) ||
            !sendBlocking(fd,
                          static_cast<std::uint16_t>(
                              cm::protocol::MessageType::UploadChunkRq),
                          ++seq, chunkPayload, error) ||
            !recvBlocking(fd, &buffer, &frame, error)) {
            ::close(fd);
            return false;
        }
        cm::protocol::UploadChunkRs rs;
        if (frame.type !=
                static_cast<std::uint16_t>(cm::protocol::MessageType::UploadChunkRs) ||
            !rs.decode(frame.payload) || rs.code != 0) {
            char text[96];
            std::snprintf(text, sizeof text, "upload chunk %u rejected: code=%u",
                          i, rs.code);
            *error = text;
            ::close(fd);
            return false;
        }
    }

    cm::protocol::UploadCompleteRq done;
    done.taskId = initRs.taskId;
    std::string donePayload;
    if (!done.encode(&donePayload) ||
        !sendBlocking(fd,
                      static_cast<std::uint16_t>(
                          cm::protocol::MessageType::UploadCompleteRq),
                      ++seq, donePayload, error) ||
        !recvBlocking(fd, &buffer, &frame, error)) {
        ::close(fd);
        return false;
    }
    cm::protocol::UploadCompleteRs complete;
    if (frame.type !=
            static_cast<std::uint16_t>(cm::protocol::MessageType::UploadCompleteRs) ||
        !complete.decode(frame.payload) || complete.code != 0) {
        *error = "upload complete rejected";
        ::close(fd);
        return false;
    }
    *ufId = complete.ufId;
    ::close(fd);
    return true;
}

// ---------------------------------------------------------------- 在途闸门

class InflightGate {
public:
    struct Waiter {
        int worker;
        int conn;
    };

    InflightGate(std::size_t limit, int workers)
        : limit_(limit),
          used_(0),
          wakeFds_(static_cast<std::size_t>(workers), -1),
          granted_(static_cast<std::size_t>(workers)) {}

    void setWakeFd(int worker, int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        wakeFds_[static_cast<std::size_t>(worker)] = fd;
    }

    bool unlimited() const { return limit_ == 0; }

    bool tryAcquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (limit_ == 0 || used_ < limit_) {
            ++used_;
            return true;
        }
        return false;
    }

    void waitFor(int worker, int conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        Waiter w;
        w.worker = worker;
        w.conn = conn;
        waiters_.push_back(w);
    }

    void release() {
        int wakeWorker = -1;
        int wakeFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (used_ > 0) {
                --used_;
            }
            // 名额直接转交队首等待者，避免唤醒后被别的线程抢走造成饥饿。
            if (!waiters_.empty()) {
                const Waiter w = waiters_.front();
                waiters_.pop_front();
                granted_[static_cast<std::size_t>(w.worker)].push_back(w.conn);
                ++used_;
                wakeWorker = w.worker;
                wakeFd = wakeFds_[static_cast<std::size_t>(w.worker)];
            }
        }
        if (wakeWorker >= 0 && wakeFd >= 0) {
            std::uint64_t one = 1;
            const ssize_t ignored = ::write(wakeFd, &one, sizeof one);
            (void)ignored;
        }
    }

    void takeGranted(int worker, std::vector<int>* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int>& list = granted_[static_cast<std::size_t>(worker)];
        out->swap(list);
        list.clear();
    }

    std::size_t inUse() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return used_;
    }

private:
    mutable std::mutex mutex_;
    std::size_t limit_;
    std::size_t used_;
    std::deque<Waiter> waiters_;
    std::vector<int> wakeFds_;
    std::vector<std::vector<int> > granted_;
};

// ---------------------------------------------------------------- 运行期全局

std::atomic<std::int64_t> g_measureStartUs(0);
std::atomic<std::int64_t> g_deadlineUs(0);
std::atomic<std::uint32_t> g_seedSeq(1);
std::atomic<long long> g_completed(0);
std::atomic<long long> g_failed(0);
std::atomic<long long> g_bytes(0);
std::atomic<long long> g_chunks(0);

std::vector<std::uint64_t> g_files;
unsigned long long g_runTag = 0;

// ---------------------------------------------------------------- 连接与工作线程

struct Env;

enum class Phase {
    Queued,
    Backoff,
    Connecting,
    Handshake,
    Ready,
    GateWait,
    Busy,
    Closed
};

struct Conn {
    int fd;
    int index;
    Phase phase;
    std::uint32_t seq;
    std::int64_t reqStartUs;
    std::int64_t nextAllowedUs;
    std::uint32_t requests;
    bool holdsSlot;
    bool disabled;

    std::string out;
    std::size_t outSent;
    cm::Buffer in;

    std::string taskId;
    std::uint32_t chunkIndex;
    std::uint32_t totalChunks;
    std::string fileMd5;
    std::vector<char> tail;
    std::string tailMd5;

    std::string ticket;
    std::uint64_t fId;
    std::uint64_t fileSize;
    std::string downloadMd5;
    std::uint32_t blockSize;
    std::uint32_t blockCount;
    std::uint32_t blockIndex;

    Conn()
        : fd(-1),
          index(0),
          phase(Phase::Queued),
          seq(1),
          reqStartUs(0),
          nextAllowedUs(0),
          requests(0),
          holdsSlot(false),
          disabled(false),
          outSent(0),
          chunkIndex(0),
          totalChunks(0),
          fId(0),
          fileSize(0),
          blockSize(0),
          blockCount(0),
          blockIndex(0) {}
};

struct Worker {
    Env* env;
    int index;
    int epfd;
    int wakeFd;
    int connecting;
    std::vector<Conn> conns;
    std::vector<int> waiting;
    std::deque<int> connectQueue;
    std::vector<int> backoff;
    std::vector<int> granted;

    long long completed;
    long long failed;
    long long bytes;
    long long connectFails;
    std::vector<std::int64_t> latencyUs;
    std::map<std::string, long long> errors;

    Worker()
        : env(nullptr),
          index(0),
          epfd(-1),
          wakeFd(-1),
          connecting(0),
          completed(0),
          failed(0),
          bytes(0),
          connectFails(0) {}
};

struct Env {
    Options opt;
    ContentModel model;
    InflightGate gate;
    std::atomic<bool> stop;
    struct sockaddr_in addr;

    explicit Env(const Options& o)
        : opt(o),
          gate(static_cast<std::size_t>(o.inflight), o.threads),
          stop(false) {
        std::memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(o.port));
        const int ok = ::inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr);
        (void)ok;
    }
};

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        const int ignored = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        (void)ignored;
    }
}

std::string errKey(const char* prefix, std::uint32_t code) {
    char text[64];
    std::snprintf(text, sizeof text, "%s=%u", prefix, code);
    return std::string(text);
}

void addError(Worker* worker, const std::string& key) {
    std::map<std::string, long long>::iterator it = worker->errors.find(key);
    if (it == worker->errors.end()) {
        worker->errors.insert(std::make_pair(key, 1LL));
    } else {
        ++it->second;
    }
}

bool wasMeasured(const Conn* conn) {
    return conn->reqStartUs != 0 && conn->reqStartUs >= g_measureStartUs.load();
}

// 前向声明
void startRequest(Worker* worker, int connIndex);
void finishRequest(Worker* worker, Conn* conn, bool ok, const std::string& reason);
void onFrame(Worker* worker, Conn* conn, const cm::protocol::Frame& frame);
void scheduleNext(Worker* worker, Conn* conn);
void closeTransport(Worker* worker, Conn* conn, const std::string& reason);

void updateInterest(Worker* worker, Conn* conn) {
    if (conn->fd < 0) {
        return;
    }
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLRDHUP;
    if (conn->outSent < conn->out.size()) {
        ev.events |= EPOLLOUT;
    }
    ev.data.u32 = static_cast<std::uint32_t>(conn->index);
    ::epoll_ctl(worker->epfd, EPOLL_CTL_MOD, conn->fd, &ev);
}

// 返回 false 表示写失败（调用方按传输错误处理）。
bool flushOut(Worker* worker, Conn* conn) {
    while (conn->outSent < conn->out.size()) {
        const ssize_t n = ::send(conn->fd, conn->out.data() + conn->outSent,
                                 conn->out.size() - conn->outSent, MSG_NOSIGNAL);
        if (n > 0) {
            conn->outSent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
    // 大帧发完立刻归还容量，避免上千条长连接各留 4MB。
    std::string().swap(conn->out);
    conn->outSent = 0;
    (void)worker;
    return true;
}

bool queueFrame(Worker* worker, Conn* conn, std::uint16_t type,
                const std::string& payload) {
    std::string frame;
    if (cm::protocol::Codec::encode(type, conn->seq, payload, &frame) !=
        cm::ErrorCode::Ok) {
        return false;
    }
    ++conn->seq;
    if (conn->out.empty()) {
        conn->out.swap(frame);
        conn->outSent = 0;
    } else {
        conn->out += frame;
    }
    if (!flushOut(worker, conn)) {
        return false;
    }
    updateInterest(worker, conn);
    return true;
}

void scheduleNext(Worker* worker, Conn* conn) {
    const Options& opt = worker->env->opt;
    conn->phase = Phase::Ready;
    if (opt.rateQps > 0) {
        const double intervalUs =
            1000000.0 * static_cast<double>(opt.conns) / static_cast<double>(opt.rateQps);
        conn->nextAllowedUs = nowUs() + static_cast<std::int64_t>(intervalUs);
    } else {
        conn->nextAllowedUs = 0;
    }
    worker->waiting.push_back(conn->index);
}

void finishRequest(Worker* worker, Conn* conn, bool ok, const std::string& reason) {
    if (wasMeasured(conn)) {
        const std::int64_t elapsed = nowUs() - conn->reqStartUs;
        if (worker->latencyUs.size() < kLatencyCapPerWorker) {
            worker->latencyUs.push_back(elapsed);
        }
        if (ok) {
            ++worker->completed;
            ++g_completed;
        } else {
            ++worker->failed;
            ++g_failed;
            addError(worker, reason);
        }
    }
    conn->reqStartUs = 0;
    if (conn->holdsSlot) {
        conn->holdsSlot = false;
        worker->env->gate.release();
    }
    scheduleNext(worker, conn);
}

void closeTransport(Worker* worker, Conn* conn, const std::string& reason) {
    if (conn->fd >= 0) {
        ::epoll_ctl(worker->epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
        ::close(conn->fd);
        conn->fd = -1;
    }
    if (wasMeasured(conn)) {
        ++worker->failed;
        ++g_failed;
        addError(worker, reason);
    }
    conn->reqStartUs = 0;
    if (conn->holdsSlot) {
        conn->holdsSlot = false;
        worker->env->gate.release();
    }
    std::string().swap(conn->out);
    conn->outSent = 0;
    conn->in.reset();
    std::vector<char>().swap(conn->tail);
    conn->phase = Phase::Closed;

    if (!conn->disabled && !worker->env->stop.load()) {
        conn->phase = Phase::Backoff;
        conn->nextAllowedUs = nowUs() + 200000;
        worker->backoff.push_back(conn->index);
    }
}

// ---------------------------------------------------------------- 业务状态机

void beginUpload(Worker* worker, Conn* conn) {
    const ContentModel& model = worker->env->model;
    const std::uint32_t seed = g_seedSeq.fetch_add(1);
    buildTail(model, seed, &conn->tail, &conn->tailMd5, &conn->fileMd5);

    char name[96];
    std::snprintf(name, sizeof name, "bench_%llu_%d_%u.bin", g_runTag, conn->index,
                  seed);

    cm::protocol::UploadInitRq rq;
    rq.parentId = 0;
    rq.fileName = name;
    rq.totalSize = model.totalSize;
    rq.fileMd5 = conn->fileMd5;
    rq.chunkSize = kChunkSize;
    rq.totalChunks = model.totalChunks;
    rq.flags = 1;
    rq.reqId = seed;

    std::string payload;
    if (!rq.encode(&payload)) {
        finishRequest(worker, conn, false, "encode_init");
        return;
    }
    conn->chunkIndex = 0;
    conn->totalChunks = model.totalChunks;
    if (!queueFrame(worker, conn,
                    static_cast<std::uint16_t>(
                        cm::protocol::MessageType::UploadInitRq),
                    payload)) {
        closeTransport(worker, conn, "init_send");
    }
}

void sendNextChunk(Worker* worker, Conn* conn) {
    if (conn->chunkIndex >= conn->totalChunks) {
        cm::protocol::UploadCompleteRq done;
        done.taskId = conn->taskId;
        std::string payload;
        if (!done.encode(&payload)) {
            finishRequest(worker, conn, false, "encode_complete");
            return;
        }
        if (!queueFrame(worker, conn,
                        static_cast<std::uint16_t>(
                            cm::protocol::MessageType::UploadCompleteRq),
                        payload)) {
            closeTransport(worker, conn, "complete_send");
        }
        return;
    }

    const ContentModel& model = worker->env->model;
    const bool last = (conn->chunkIndex + 1 == conn->totalChunks);
    cm::protocol::UploadChunkRq rq;
    rq.taskId = conn->taskId;
    rq.chunkIndex = conn->chunkIndex;
    rq.offset = static_cast<std::uint64_t>(conn->chunkIndex) * kChunkSize;
    const char* data = last ? &conn->tail[0] : &model.prefix[0];
    const std::size_t len = last ? conn->tail.size() : kChunkSize;
    rq.chunkMd5 = last ? conn->tailMd5 : model.prefixMd5;
    rq.data.assign(data, len);

    std::string payload;
    if (!rq.encode(&payload)) {
        finishRequest(worker, conn, false, "encode_chunk");
        return;
    }
    if (last) {
        std::vector<char>().swap(conn->tail);
    }
    ++conn->chunkIndex;
    if (!queueFrame(worker, conn,
                    static_cast<std::uint16_t>(
                        cm::protocol::MessageType::UploadChunkRq),
                    payload)) {
        closeTransport(worker, conn, "chunk_send");
    }
}

void beginDownload(Worker* worker, Conn* conn) {
    if (g_files.empty()) {
        finishRequest(worker, conn, false, "no_setup_files");
        return;
    }
    const std::size_t pick =
        (static_cast<std::size_t>(conn->index) + conn->requests) % g_files.size();
    cm::protocol::DownloadOpenRq rq;
    rq.ufId = g_files[pick];
    rq.shareTicket.clear();
    rq.startBlock = 0;

    std::string payload;
    if (!rq.encode(&payload)) {
        finishRequest(worker, conn, false, "encode_open");
        return;
    }
    if (!queueFrame(worker, conn,
                    static_cast<std::uint16_t>(
                        cm::protocol::MessageType::DownloadOpenRq),
                    payload)) {
        closeTransport(worker, conn, "open_send");
    }
}

void sendNextBlock(Worker* worker, Conn* conn) {
    if (conn->blockIndex >= conn->blockCount) {
        cm::protocol::DownloadDoneRq done;
        done.ticket = conn->ticket;
        done.fId = conn->fId;
        done.size = conn->fileSize;
        done.fileMd5 = conn->downloadMd5;
        done.verifyOk = 1;
        std::string payload;
        if (!done.encode(&payload)) {
            finishRequest(worker, conn, false, "encode_done");
            return;
        }
        if (!queueFrame(worker, conn,
                        static_cast<std::uint16_t>(
                            cm::protocol::MessageType::DownloadDoneRq),
                        payload)) {
            closeTransport(worker, conn, "done_send");
        }
        return;
    }

    cm::protocol::DownloadRangeRq rq;
    rq.ticket = conn->ticket;
    rq.fId = conn->fId;
    rq.blockIndex = conn->blockIndex;
    std::string payload;
    if (!rq.encode(&payload)) {
        finishRequest(worker, conn, false, "encode_range");
        return;
    }
    if (!queueFrame(worker, conn,
                    static_cast<std::uint16_t>(
                        cm::protocol::MessageType::DownloadRangeRq),
                    payload)) {
        closeTransport(worker, conn, "range_send");
    }
}

void startRequest(Worker* worker, int connIndex) {
    Conn* conn = &worker->conns[static_cast<std::size_t>(connIndex)];
    const Options& opt = worker->env->opt;
    conn->phase = Phase::Busy;
    conn->reqStartUs = nowUs();
    ++conn->requests;

    switch (opt.op) {
        case Op::Login: {
            cm::protocol::LoginRq rq;
            rq.tel = opt.tel;
            rq.password = opt.password;
            std::string payload;
            if (!rq.encode(&payload)) {
                finishRequest(worker, conn, false, "encode_login");
                return;
            }
            if (!queueFrame(worker, conn,
                            static_cast<std::uint16_t>(
                                cm::protocol::MessageType::LoginRq),
                            payload)) {
                closeTransport(worker, conn, "login_send");
            }
            break;
        }
        case Op::ListFile: {
            cm::protocol::ListFileRq rq;
            rq.parentId = 0;
            rq.sort = 1;
            rq.limit = static_cast<std::uint32_t>(opt.listLimit);
            rq.cursorKey.clear();
            rq.cursorId = 0;
            std::string payload;
            if (!rq.encode(&payload)) {
                finishRequest(worker, conn, false, "encode_list");
                return;
            }
            if (!queueFrame(worker, conn,
                            static_cast<std::uint16_t>(
                                cm::protocol::MessageType::ListFileRq),
                            payload)) {
                closeTransport(worker, conn, "list_send");
            }
            break;
        }
        case Op::Upload:
            beginUpload(worker, conn);
            break;
        case Op::Download:
            beginDownload(worker, conn);
            break;
        case Op::Connect:
            finishRequest(worker, conn, true, "");
            break;
    }
}

void onFrame(Worker* worker, Conn* conn, const cm::protocol::Frame& frame) {
    using cm::protocol::MessageType;
    const std::uint16_t type = frame.type;

    if (type == static_cast<std::uint16_t>(MessageType::ErrorRs)) {
        std::uint32_t code = 0;
        std::string message;
        decodeErrorRs(frame, &code, &message);
        if (conn->phase == Phase::Handshake) {
            conn->disabled = true;
            closeTransport(worker, conn, errKey("handshake", code));
            return;
        }
        finishRequest(worker, conn, false, errKey("err", code));
        return;
    }

    if (conn->phase == Phase::Handshake) {
        if (type == static_cast<std::uint16_t>(MessageType::LoginRs)) {
            cm::protocol::LoginRs rs;
            if (!rs.decode(frame.payload) || rs.code != 0) {
                conn->disabled = true;
                closeTransport(worker, conn, errKey("handshake", rs.code));
                return;
            }
            conn->phase = Phase::Ready;
            conn->nextAllowedUs = 0;
            worker->waiting.push_back(conn->index);
        }
        return;
    }

    if (conn->phase != Phase::Busy) {
        return;
    }

    switch (type) {
        case static_cast<std::uint16_t>(MessageType::LoginRs): {
            cm::protocol::LoginRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_login");
                return;
            }
            finishRequest(worker, conn, rs.code == 0, errKey("login", rs.code));
            break;
        }
        case static_cast<std::uint16_t>(MessageType::UploadInitRs): {
            cm::protocol::UploadInitRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_init");
                return;
            }
            if (rs.code != 0) {
                finishRequest(worker, conn, false, errKey("init", rs.code));
                return;
            }
            if (rs.result == 1) {
                // 唯一内容失效才会走到这里（秒传短路）。
                finishRequest(worker, conn, false, "instant_hit");
                return;
            }
            conn->taskId = rs.taskId;
            conn->totalChunks = rs.totalChunks;
            conn->chunkIndex = 0;
            sendNextChunk(worker, conn);
            break;
        }
        case static_cast<std::uint16_t>(MessageType::UploadChunkRs): {
            cm::protocol::UploadChunkRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_chunk");
                return;
            }
            if (rs.code != 0) {
                finishRequest(worker, conn, false, errKey("chunk", rs.code));
                return;
            }
            ++g_chunks;
            sendNextChunk(worker, conn);
            break;
        }
        case static_cast<std::uint16_t>(MessageType::UploadCompleteRs): {
            cm::protocol::UploadCompleteRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_complete");
                return;
            }
            if (rs.code != 0) {
                finishRequest(worker, conn, false, errKey("complete", rs.code));
                return;
            }
            g_bytes.fetch_add(static_cast<long long>(worker->env->model.totalSize));
            finishRequest(worker, conn, true, "");
            break;
        }
        case static_cast<std::uint16_t>(MessageType::DownloadOpenRs): {
            cm::protocol::DownloadOpenRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_open");
                return;
            }
            if (rs.code != 0) {
                finishRequest(worker, conn, false, errKey("open", rs.code));
                return;
            }
            conn->ticket = rs.ticket;
            conn->fId = rs.fId;
            conn->fileSize = rs.size;
            conn->downloadMd5 = rs.fileMd5;
            conn->blockSize = rs.blockSize;
            conn->blockCount = rs.blockCount;
            conn->blockIndex = 0;
            sendNextBlock(worker, conn);
            break;
        }
        case static_cast<std::uint16_t>(MessageType::DownloadDataRs): {
            g_bytes.fetch_add(static_cast<long long>(frame.payload.size()));
            ++g_chunks;
            ++conn->blockIndex;
            sendNextBlock(worker, conn);
            break;
        }
        case static_cast<std::uint16_t>(MessageType::DownloadDoneRs): {
            cm::protocol::DownloadDoneRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_done");
                return;
            }
            finishRequest(worker, conn, rs.code == 0, errKey("done", rs.code));
            break;
        }
        case static_cast<std::uint16_t>(MessageType::ListFileRs): {
            cm::protocol::ListFileRs rs;
            if (!rs.decode(frame.payload)) {
                finishRequest(worker, conn, false, "decode_list");
                return;
            }
            finishRequest(worker, conn, rs.code == 0, errKey("list", rs.code));
            break;
        }
        default:
            finishRequest(worker, conn, false, "unexpected_type");
            break;
    }
}

void readConn(Worker* worker, Conn* conn) {
    for (;;) {
        int savedErrno = 0;
        const ssize_t n = conn->in.readFd(conn->fd, &savedErrno);
        if (n > 0) {
            for (;;) {
                cm::protocol::Frame frame;
                cm::protocol::DecodeStatus status = cm::protocol::DecodeStatus::NeedMore;
                const cm::ErrorCode rc =
                    cm::protocol::Codec::decode(conn->in, &frame, &status);
                if (rc == cm::ErrorCode::Ok) {
                    onFrame(worker, conn, frame);
                    if (conn->fd < 0) {
                        return;
                    }
                    if (conn->in.readableBytes() == 0 &&
                        conn->in.capacity() > 512 * 1024) {
                        conn->in.shrinkTo(64 * 1024);
                    }
                    continue;
                }
                if (rc == cm::ErrorCode::NeedMore) {
                    break;
                }
                closeTransport(worker, conn, "bad_frame");
                return;
            }
            continue;
        }
        if (n == 0) {
            closeTransport(worker, conn, "peer_closed");
            return;
        }
        if (savedErrno == EINTR) {
            continue;
        }
        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return;
        }
        closeTransport(worker, conn, "read_error");
        return;
    }
}

// ---------------------------------------------------------------- 连接管理

bool sendLogin(Worker* worker, Conn* conn) {
    const Options& opt = worker->env->opt;
    cm::protocol::LoginRq rq;
    rq.tel = opt.tel;
    rq.password = opt.password;
    std::string payload;
    if (!rq.encode(&payload)) {
        return false;
    }
    return queueFrame(worker, conn,
                      static_cast<std::uint16_t>(cm::protocol::MessageType::LoginRq),
                      payload);
}

void pumpBackoff(Worker* worker) {
    if (worker->backoff.empty()) {
        return;
    }
    const std::int64_t now = nowUs();
    std::vector<int> stay;
    for (std::size_t i = 0; i < worker->backoff.size(); ++i) {
        const int ci = worker->backoff[i];
        Conn* conn = &worker->conns[static_cast<std::size_t>(ci)];
        if (conn->phase != Phase::Backoff) {
            continue;
        }
        if (now < conn->nextAllowedUs) {
            stay.push_back(ci);
            continue;
        }
        conn->phase = Phase::Queued;
        worker->connectQueue.push_back(ci);
    }
    worker->backoff.swap(stay);
}

void pumpConnects(Worker* worker) {
    while (worker->connecting < static_cast<int>(kMaxPendingConnect) &&
           !worker->connectQueue.empty()) {
        const int ci = worker->connectQueue.front();
        worker->connectQueue.pop_front();
        Conn* conn = &worker->conns[static_cast<std::size_t>(ci)];
        if (conn->phase != Phase::Queued) {
            continue;
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            conn->phase = Phase::Backoff;
            conn->nextAllowedUs = nowUs() + 200000;
            worker->backoff.push_back(ci);
            continue;
        }
        setNonBlocking(fd);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn->fd = fd;

        const int rc = ::connect(fd,
                                 reinterpret_cast<struct sockaddr*>(&worker->env->addr),
                                 sizeof worker->env->addr);
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof ev);
        ev.data.u32 = static_cast<std::uint32_t>(ci);

        if (rc == 0) {
            ev.events = EPOLLIN | EPOLLRDHUP;
            ::epoll_ctl(worker->epfd, EPOLL_CTL_ADD, fd, &ev);
            conn->phase = Phase::Handshake;
            if (!sendLogin(worker, conn)) {
                closeTransport(worker, conn, "login_send");
            }
        } else if (errno == EINPROGRESS) {
            ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
            ::epoll_ctl(worker->epfd, EPOLL_CTL_ADD, fd, &ev);
            conn->phase = Phase::Connecting;
            ++worker->connecting;
        } else {
            ::close(fd);
            conn->fd = -1;
            ++worker->connectFails;
            conn->phase = Phase::Backoff;
            conn->nextAllowedUs = nowUs() + 200000;
            worker->backoff.push_back(ci);
        }
    }
}

void onConnectWritable(Worker* worker, Conn* conn) {
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        if (worker->connecting > 0) {
            --worker->connecting;
        }
        ++worker->connectFails;
        closeTransport(worker, conn, "connect_error");
        return;
    }
    if (worker->connecting > 0) {
        --worker->connecting;
    }
    conn->phase = Phase::Handshake;
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.u32 = static_cast<std::uint32_t>(conn->index);
    ::epoll_ctl(worker->epfd, EPOLL_CTL_MOD, conn->fd, &ev);
    if (!sendLogin(worker, conn)) {
        closeTransport(worker, conn, "login_send");
    }
}

void drainWake(Worker* worker) {
    std::uint64_t value = 0;
    for (;;) {
        const ssize_t n = ::read(worker->wakeFd, &value, sizeof value);
        if (n == static_cast<ssize_t>(sizeof value)) {
            continue;
        }
        break;
    }
    worker->env->gate.takeGranted(worker->index, &worker->granted);
    for (std::size_t i = 0; i < worker->granted.size(); ++i) {
        const int ci = worker->granted[i];
        Conn* conn = &worker->conns[static_cast<std::size_t>(ci)];
        if (conn->phase == Phase::GateWait) {
            conn->holdsSlot = true;
            startRequest(worker, ci);
        } else {
            // 连接在等待期间关了：把名额还回去。
            worker->env->gate.release();
        }
    }
    worker->granted.clear();
}

void pumpWaiting(Worker* worker) {
    if (worker->waiting.empty()) {
        return;
    }
    const Options& opt = worker->env->opt;
    std::vector<int> pending;
    pending.swap(worker->waiting);
    std::vector<int> stay;
    const std::int64_t now = nowUs();
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const int ci = pending[i];
        Conn* conn = &worker->conns[static_cast<std::size_t>(ci)];
        if (conn->phase != Phase::Ready) {
            continue;
        }
        if (now < conn->nextAllowedUs) {
            stay.push_back(ci);
            continue;
        }
        if (isBigOp(opt.op) && !worker->env->gate.unlimited()) {
            if (!worker->env->gate.tryAcquire()) {
                conn->phase = Phase::GateWait;
                worker->env->gate.waitFor(worker->index, ci);
                continue;
            }
            conn->holdsSlot = true;
        }
        startRequest(worker, ci);
    }
    worker->waiting.insert(worker->waiting.end(), stay.begin(), stay.end());
}

void workerMain(Worker* worker) {
    worker->epfd = ::epoll_create1(EPOLL_CLOEXEC);
    worker->wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (worker->epfd < 0 || worker->wakeFd < 0) {
        return;
    }
    worker->env->gate.setWakeFd(worker->index, worker->wakeFd);

    struct epoll_event wakeEv;
    std::memset(&wakeEv, 0, sizeof wakeEv);
    wakeEv.events = EPOLLIN;
    wakeEv.data.u32 = kWakeMarker;
    ::epoll_ctl(worker->epfd, EPOLL_CTL_ADD, worker->wakeFd, &wakeEv);

    for (std::size_t i = 0; i < worker->conns.size(); ++i) {
        worker->conns[i].index = static_cast<int>(i);
        worker->conns[i].phase = Phase::Queued;
        worker->connectQueue.push_back(static_cast<int>(i));
    }

    std::vector<struct epoll_event> events(kMaxEvents);
    while (!worker->env->stop.load()) {
        if (nowUs() >= g_deadlineUs.load()) {
            break;
        }
        const int timeout = worker->waiting.empty() ? 20 : 2;
        const int n = ::epoll_wait(worker->epfd, &events[0],
                                   static_cast<int>(kMaxEvents), timeout);
        for (int i = 0; i < n; ++i) {
            const std::uint32_t marker = events[i].data.u32;
            if (marker == kWakeMarker) {
                drainWake(worker);
                continue;
            }
            const std::size_t ci = static_cast<std::size_t>(marker);
            if (ci >= worker->conns.size()) {
                continue;
            }
            Conn* conn = &worker->conns[ci];
            if (conn->fd < 0) {
                continue;
            }
            const std::uint32_t ev = events[i].events;
            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                if (conn->phase == Phase::Connecting) {
                    if (worker->connecting > 0) {
                        --worker->connecting;
                    }
                    ++worker->connectFails;
                }
                closeTransport(worker, conn, "conn_reset");
                continue;
            }
            if (conn->phase == Phase::Connecting && (ev & EPOLLOUT) != 0) {
                onConnectWritable(worker, conn);
                if (conn->fd < 0) {
                    continue;
                }
            }
            if ((ev & EPOLLOUT) != 0) {
                if (!flushOut(worker, conn)) {
                    closeTransport(worker, conn, "write_error");
                    continue;
                }
                updateInterest(worker, conn);
            }
            if ((ev & EPOLLIN) != 0) {
                readConn(worker, conn);
            }
        }
        pumpBackoff(worker);
        pumpConnects(worker);
        pumpWaiting(worker);
    }

    for (std::size_t i = 0; i < worker->conns.size(); ++i) {
        if (worker->conns[i].fd >= 0) {
            ::close(worker->conns[i].fd);
            worker->conns[i].fd = -1;
        }
    }
    ::close(worker->epfd);
    ::close(worker->wakeFd);
    worker->env->gate.setWakeFd(worker->index, -1);
}

// ---------------------------------------------------------------- 统计

double percentileMs(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double pos = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    std::size_t idx = static_cast<std::size_t>(pos + 0.5);
    if (idx >= sorted.size()) {
        idx = sorted.size() - 1;
    }
    return static_cast<double>(sorted[idx]) / 1000.0;
}

struct RunSummary {
    long long completed;
    long long failed;
    long long chunks;
    long long bytes;
    double elapsedSec;
    double fileQps;
    double chunkQps;
    double mbps;
    double failRate;
    double p50;
    double p90;
    double p99;
    double p999;
    long long connectFails;
    std::string errors;

    RunSummary()
        : completed(0), failed(0), chunks(0), bytes(0), elapsedSec(0.0),
          fileQps(0.0), chunkQps(0.0), mbps(0.0), failRate(0.0), p50(0.0),
          p90(0.0), p99(0.0), p999(0.0), connectFails(0) {}
};

std::string joinErrors(const std::map<std::string, long long>& errors) {
    std::string out;
    for (std::map<std::string, long long>::const_iterator it = errors.begin();
         it != errors.end(); ++it) {
        if (!out.empty()) {
            out += ";";
        }
        char text[128];
        std::snprintf(text, sizeof text, "%s:%lld", it->first.c_str(), it->second);
        out += text;
    }
    return out;
}

void writeCsvHeaderIfNeeded(FILE* csv) {
    if (csv == nullptr) {
        return;
    }
    const long pos = std::ftell(csv);
    if (pos == 0) {
        std::fprintf(csv,
                     "ts,kind,run,op,conns,threads,slots,elapsed_s,requests,"
                     "completed,failed,fail_rate_pct,file_qps,chunk_qps,mbps,"
                     "p50_ms,p90_ms,p99_ms,p999_ms,bytes,errors\n");
    }
}

void writeCsvSample(FILE* csv, long long ts, int run, const char* op, int conns,
                    int threads, int slots, double elapsed, long long completed,
                    long long failed, double qps, double chunkQps, double mbps,
                    long long bytes) {
    if (csv == nullptr) {
        return;
    }
    const long long total = completed + failed;
    const double failRate =
        total > 0 ? 100.0 * static_cast<double>(failed) / static_cast<double>(total)
                  : 0.0;
    std::fprintf(csv,
                 "%lld,sample,%d,%s,%d,%d,%d,%.1f,%lld,%lld,%lld,%.2f,%.1f,%.1f,"
                 "%.2f,,,,,%lld,\n",
                 ts, run, op, conns, threads, slots, elapsed, total, completed,
                 failed, failRate, qps, chunkQps, mbps, bytes);
}

void writeCsvSummary(FILE* csv, long long ts, int run, const RunSummary& s,
                     const char* op, int conns, int threads, int slots) {
    if (csv == nullptr) {
        return;
    }
    std::fprintf(csv,
                 "%lld,summary,%d,%s,%d,%d,%d,%.1f,%lld,%lld,%lld,%.2f,%.1f,%.1f,"
                 "%.2f,%.2f,%.2f,%.2f,%.2f,%lld,%s\n",
                 ts, run, op, conns, threads, slots, s.elapsedSec,
                 s.completed + s.failed, s.completed, s.failed, s.failRate,
                 s.fileQps, s.chunkQps, s.mbps, s.p50, s.p90, s.p99, s.p999,
                 s.bytes, s.errors.c_str());
}

double medianOf(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// ---------------------------------------------------------------- 一轮压测

RunSummary executeRun(Env* env, int runIndex, FILE* csv) {
    const Options& opt = env->opt;
    const char* opText = opName(opt.op);
    const int threads = opt.threads;

    std::vector<Worker> workers(static_cast<std::size_t>(threads));
    const int base = opt.conns / threads;
    const int extra = opt.conns % threads;

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        Worker& worker = workers[static_cast<std::size_t>(i)];
        worker.env = env;
        worker.index = i;
        const int count = base + (i < extra ? 1 : 0);
        worker.conns.resize(static_cast<std::size_t>(count));
    }

    g_measureStartUs.store(0);
    const std::int64_t t0 = nowUs();
    const std::int64_t measureStart = t0 + static_cast<std::int64_t>(opt.warmupSec) * 1000000;
    const std::int64_t deadline = measureStart + static_cast<std::int64_t>(opt.durationSec) * 1000000;
    g_deadlineUs.store(deadline);
    g_measureStartUs.store(measureStart);

    for (int i = 0; i < threads; ++i) {
        pool.push_back(std::thread(workerMain, &workers[static_cast<std::size_t>(i)]));
    }

    while (nowUs() < measureStart) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const long long baseCompleted = g_completed.load();
    const long long baseFailed = g_failed.load();
    const long long baseChunks = g_chunks.load();
    const long long baseBytes = g_bytes.load();

    long long prevCompleted = baseCompleted;
    long long prevFailed = baseFailed;
    long long prevChunks = baseChunks;
    long long prevBytes = baseBytes;
    std::int64_t prevUs = nowUs();

    while (true) {
        const std::int64_t now = nowUs();
        if (now >= deadline) {
            break;
        }
        const std::int64_t waitUs = deadline - now;
        const std::int64_t chunkUs = waitUs < 1000000 ? waitUs : 1000000;
        std::this_thread::sleep_for(std::chrono::microseconds(chunkUs));

        const std::int64_t sampleUs = nowUs() < deadline ? nowUs() : deadline;
        const long long curCompleted = g_completed.load();
        const long long curFailed = g_failed.load();
        const long long curChunks = g_chunks.load();
        const long long curBytes = g_bytes.load();
        const double spanSec = static_cast<double>(sampleUs - prevUs) / 1000000.0;
        if (spanSec > 0.0) {
            const double qps = static_cast<double>(curCompleted - prevCompleted) / spanSec;
            const double chunkQps = static_cast<double>(curChunks - prevChunks) / spanSec;
            const double mbps = static_cast<double>(curBytes - prevBytes) / 1048576.0 / spanSec;
            writeCsvSample(csv, static_cast<long long>(std::time(nullptr)), runIndex,
                           opText, opt.conns, threads, opt.inflight,
                           static_cast<double>(sampleUs - measureStart) / 1000000.0,
                           curCompleted - prevCompleted, curFailed - prevFailed, qps,
                           chunkQps, mbps, curBytes - prevBytes);
        }
        prevCompleted = curCompleted;
        prevFailed = curFailed;
        prevChunks = curChunks;
        prevBytes = curBytes;
        prevUs = sampleUs;
    }

    const std::int64_t endUs = nowUs();
    env->stop.store(true);
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pool[i].join();
    }

    RunSummary summary;
    summary.completed = g_completed.load() - baseCompleted;
    summary.failed = g_failed.load() - baseFailed;
    summary.chunks = g_chunks.load() - baseChunks;
    summary.bytes = g_bytes.load() - baseBytes;
    summary.elapsedSec = static_cast<double>(endUs - measureStart) / 1000000.0;
    if (summary.elapsedSec <= 0.0) {
        summary.elapsedSec = 1.0;
    }
    const long long total = summary.completed + summary.failed;
    summary.failRate =
        total > 0 ? 100.0 * static_cast<double>(summary.failed) / static_cast<double>(total)
                  : 0.0;
    summary.fileQps = static_cast<double>(summary.completed) / summary.elapsedSec;
    summary.chunkQps = static_cast<double>(summary.chunks) / summary.elapsedSec;
    summary.mbps = static_cast<double>(summary.bytes) / 1048576.0 / summary.elapsedSec;

    std::vector<std::int64_t> latencies;
    std::map<std::string, long long> errors;
    for (std::size_t i = 0; i < workers.size(); ++i) {
        latencies.insert(latencies.end(), workers[i].latencyUs.begin(),
                         workers[i].latencyUs.end());
        summary.connectFails += workers[i].connectFails;
        for (std::map<std::string, long long>::const_iterator it =
                 workers[i].errors.begin();
             it != workers[i].errors.end(); ++it) {
            errors[it->first] += it->second;
        }
    }
    std::sort(latencies.begin(), latencies.end());
    summary.p50 = percentileMs(latencies, 50.0);
    summary.p90 = percentileMs(latencies, 90.0);
    summary.p99 = percentileMs(latencies, 99.0);
    summary.p999 = percentileMs(latencies, 99.9);
    summary.errors = joinErrors(errors);

    writeCsvSummary(csv, static_cast<long long>(std::time(nullptr)), runIndex, summary,
                    opText, opt.conns, threads, opt.inflight);

    std::printf(
        "[run %d] op=%s conns=%d threads=%d inflight=%d elapse=%.1fs "
        "completed=%lld failed=%lld fail=%.2f%% file_qps=%.1f chunk_qps=%.1f "
        "thr=%.1fMB/s p50=%.2fms p90=%.2fms p99=%.2fms p999=%.2fms conn_fail=%lld\n",
        runIndex, opText, opt.conns, threads, opt.inflight, summary.elapsedSec,
        summary.completed, summary.failed, summary.failRate, summary.fileQps,
        summary.chunkQps, summary.mbps, summary.p50, summary.p90, summary.p99,
        summary.p999, summary.connectFails);
    if (!summary.errors.empty()) {
        std::printf("[run %d] errors=%s\n", runIndex, summary.errors.c_str());
    }
    std::fflush(stdout);
    return summary;
}

// ---------------------------------------------------------------- 建连风暴

struct StormResult {
    long long ok;
    long long failed;
    std::vector<std::int64_t> latencyUs;

    StormResult() : ok(0), failed(0) {}
};

void stormThread(const Options& opt, long long quota, StormResult* out) {
    struct Slot {
        int fd;
        std::int64_t startUs;
    };

    const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        out->failed += quota;
        return;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opt.port));
    if (::inet_pton(AF_INET, opt.host.c_str(), &addr.sin_addr) != 1) {
        out->failed += quota;
        ::close(epfd);
        return;
    }

    std::vector<Slot> slots(kMaxPendingConnect);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].fd = -1;
        slots[i].startUs = 0;
    }
    std::size_t active = 0;
    long long issued = 0;
    std::vector<struct epoll_event> events(kMaxEvents);

    while (issued < quota || active > 0) {
        while (active < slots.size() && issued < quota) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            ++issued;
            if (fd < 0) {
                ++out->failed;
                continue;
            }
            setNonBlocking(fd);
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            const std::int64_t startUs = nowUs();
            const int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                                     sizeof addr);
            if (rc == 0) {
                out->latencyUs.push_back(nowUs() - startUs);
                ++out->ok;
                ::close(fd);
                continue;
            }
            if (errno != EINPROGRESS) {
                ++out->failed;
                ::close(fd);
                continue;
            }
            std::size_t slot = slots.size();
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].fd < 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == slots.size()) {
                // 槽位满（理论上不会发生）：同步等一次再重试。
                ++out->failed;
                ::close(fd);
                continue;
            }
            slots[slot].fd = fd;
            slots[slot].startUs = startUs;
            struct epoll_event ev;
            std::memset(&ev, 0, sizeof ev);
            ev.events = EPOLLOUT | EPOLLIN;
            ev.data.u32 = static_cast<std::uint32_t>(slot);
            ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
            ++active;
        }
        if (active == 0) {
            continue;
        }

        const int n = ::epoll_wait(epfd, &events[0], static_cast<int>(kMaxEvents), 50);
        for (int i = 0; i < n; ++i) {
            const std::size_t slot = static_cast<std::size_t>(events[i].data.u32);
            if (slot >= slots.size() || slots[slot].fd < 0) {
                continue;
            }
            int err = 0;
            socklen_t len = sizeof err;
            ::getsockopt(slots[slot].fd, SOL_SOCKET, SO_ERROR, &err, &len);
            out->latencyUs.push_back(nowUs() - slots[slot].startUs);
            if (err == 0) {
                ++out->ok;
            } else {
                ++out->failed;
            }
            ::epoll_ctl(epfd, EPOLL_CTL_DEL, slots[slot].fd, nullptr);
            ::close(slots[slot].fd);
            slots[slot].fd = -1;
            if (active > 0) {
                --active;
            }
        }
    }
    ::close(epfd);
}

RunSummary runStorm(const Options& opt, FILE* csv) {
    const int threads = opt.threads;
    const int base = opt.conns / threads;
    const int extra = opt.conns % threads;

    std::vector<StormResult> results(static_cast<std::size_t>(threads));
    std::vector<std::thread> pool;
    const std::int64_t startUs = nowUs();
    for (int i = 0; i < threads; ++i) {
        const long long quota = base + (i < extra ? 1 : 0);
        pool.push_back(std::thread(stormThread, std::cref(opt), quota,
                                   &results[static_cast<std::size_t>(i)]));
    }
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pool[i].join();
    }
    const double elapsed = static_cast<double>(nowUs() - startUs) / 1000000.0;

    RunSummary summary;
    std::vector<std::int64_t> latencies;
    for (std::size_t i = 0; i < results.size(); ++i) {
        summary.completed += results[i].ok;
        summary.failed += results[i].failed;
        latencies.insert(latencies.end(), results[i].latencyUs.begin(),
                         results[i].latencyUs.end());
    }
    std::sort(latencies.begin(), latencies.end());
    summary.elapsedSec = elapsed > 0.0 ? elapsed : 1.0;
    summary.fileQps = static_cast<double>(summary.completed) / summary.elapsedSec;
    summary.chunkQps = summary.fileQps;
    const long long total = summary.completed + summary.failed;
    summary.failRate =
        total > 0 ? 100.0 * static_cast<double>(summary.failed) / static_cast<double>(total)
                  : 0.0;
    summary.p50 = percentileMs(latencies, 50.0);
    summary.p90 = percentileMs(latencies, 90.0);
    summary.p99 = percentileMs(latencies, 99.0);
    summary.p999 = percentileMs(latencies, 99.9);

    writeCsvSummary(csv, static_cast<long long>(std::time(nullptr)), 0, summary,
                    "connect", opt.conns, threads, 0);
    std::printf(
        "[storm] op=connect conns=%d threads=%d elapse=%.2fs ok=%lld failed=%lld "
        "fail=%.2f%% rate=%.0f conns/s p50=%.2fms p90=%.2fms p99=%.2fms p999=%.2fms\n",
        opt.conns, threads, summary.elapsedSec, summary.completed, summary.failed,
        summary.failRate, summary.fileQps, summary.p50, summary.p90, summary.p99,
        summary.p999);
    std::fflush(stdout);
    return summary;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, &opt)) {
        printUsage(argv[0]);
        return 1;
    }

    g_runTag = static_cast<unsigned long long>(::getpid()) * 1000ULL +
               static_cast<unsigned long long>(std::time(nullptr) % 1000);
    // seed 同时用作 UploadInit.reqId：必须跨进程唯一，否则会命中上一次
    // 进程留在 Redis 里的 uploadReq 幂等缓存，拿到旧任务（已完成的 task 再发
    // 分片会被服务端回 3005 TaskStateInvalid）。
    g_seedSeq.store(static_cast<std::uint32_t>((nowUs() / 1000) & 0x7FFFFFFF));

    FILE* csv = nullptr;
    if (!opt.csvPath.empty()) {
        csv = std::fopen(opt.csvPath.c_str(), "a");
        if (csv == nullptr) {
            std::fprintf(stderr, "cannot open csv: %s\n", opt.csvPath.c_str());
            return 1;
        }
        writeCsvHeaderIfNeeded(csv);
    }

    std::printf(
        "bencher op=%s conns=%d threads=%d size=%dMB duration=%ds warmup=%ds "
        "runs=%d rate=%d inflight=%d target=%s:%d\n",
        opName(opt.op), opt.conns, opt.threads, opt.sizeMb, opt.durationSec,
        opt.warmupSec, opt.runs, opt.rateQps, opt.inflight, opt.host.c_str(),
        opt.port);
    std::fflush(stdout);

    if (opt.op == Op::Connect) {
        runStorm(opt, csv);
        if (csv != nullptr) {
            std::fclose(csv);
        }
        return 0;
    }

    ContentModel model;
    initContent(opt, &model);
    std::printf("content model: size=%uMB chunks=%u tail=%zuB prefix_md5=%s\n",
                model.totalSize / (1024u * 1024u), model.totalChunks,
                model.tailLen,
                model.prefixMd5.empty() ? "-" : model.prefixMd5.c_str());

    if ((opt.op == Op::Download || opt.op == Op::ListFile) && opt.setupFiles > 0) {
        std::printf("setup: uploading %d files of %dMB ...\n", opt.setupFiles,
                    opt.sizeMb);
        std::fflush(stdout);
        for (int i = 0; i < opt.setupFiles; ++i) {
            char name[96];
            std::snprintf(name, sizeof name, "bench_setup_%llu_%d.bin", g_runTag, i);
            std::uint64_t ufId = 0;
            std::string error;
            const std::uint32_t seed = g_seedSeq.fetch_add(1);
            if (!setupOneFile(opt, model, seed, name, &ufId, &error)) {
                std::fprintf(stderr, "setup failed: %s\n", error.c_str());
                if (csv != nullptr) {
                    std::fclose(csv);
                }
                return 3;
            }
            g_files.push_back(ufId);
            std::printf("setup: uf_id=%llu name=%s\n",
                        static_cast<unsigned long long>(ufId), name);
            std::fflush(stdout);
        }
    }

    std::vector<RunSummary> runs;
    for (int run = 0; run < opt.runs; ++run) {
        Env env(opt);
        env.model = model;
        runs.push_back(executeRun(&env, run, csv));
    }

    if (runs.size() > 1) {
        std::vector<double> qps;
        std::vector<double> chunkQps;
        std::vector<double> mbps;
        std::vector<double> p99;
        std::vector<double> failRate;
        for (std::size_t i = 0; i < runs.size(); ++i) {
            qps.push_back(runs[i].fileQps);
            chunkQps.push_back(runs[i].chunkQps);
            mbps.push_back(runs[i].mbps);
            p99.push_back(runs[i].p99);
            failRate.push_back(runs[i].failRate);
        }
        std::printf(
            "SUMMARY op=%s conns=%d threads=%d inflight=%d runs=%zu "
            "median_file_qps=%.1f median_chunk_qps=%.1f median_mbps=%.2f "
            "median_p99=%.2fms median_fail=%.2f%%\n",
            opName(opt.op), opt.conns, opt.threads, opt.inflight, runs.size(),
            medianOf(qps), medianOf(chunkQps), medianOf(mbps), medianOf(p99),
            medianOf(failRate));
    } else if (!runs.empty()) {
        std::printf(
            "SUMMARY op=%s conns=%d threads=%d inflight=%d runs=1 "
            "median_file_qps=%.1f median_chunk_qps=%.1f median_mbps=%.2f "
            "median_p99=%.2fms median_fail=%.2f%%\n",
            opName(opt.op), opt.conns, opt.threads, opt.inflight,
            runs[0].fileQps, runs[0].chunkQps, runs[0].mbps, runs[0].p99,
            runs[0].failRate);
    }
    std::fflush(stdout);

    if (csv != nullptr) {
        std::fclose(csv);
    }
    return 0;
}

#else

int main() {
    std::fprintf(stderr, "bencher requires Linux (epoll).\n");
    return 1;
}

#endif
