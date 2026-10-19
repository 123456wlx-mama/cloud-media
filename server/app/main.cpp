#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "cm/config/server_config.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/upload.h"
#include "cm/protocol/serialize.h"
#include "cm/protocol/version.h"
#include "cm/version.h"

#include "EventLoop.h"
#include "SignalHandler.h"
#include "TcpServer.h"
#include "TimerQueue.h"
#include "handler_registry.h"
#include "server_app.h"

#if defined(CM_WITH_STORAGE)
#include "cm/base/thread_pool.h"
#include "cache/cache_store.h"
#include "cache/redis_pool.h"
#include "db/mysql_pool.h"
#include "logic/block_sender.h"
#include "logic/connection_decoder.h"
#include "logic/download_handlers.h"
#include "logic/download_service.h"
#include "logic/sendfile_block_sender.h"
#include "logic/upload_chunk_handler.h"
#include "logic/upload_service.h"
#endif

namespace {

void printUsage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s -c <server.ini> [-p <port>]\n"
                 "       %s --version\n",
                 program, program);
}

#if defined(CM_WITH_STORAGE)

using net::ConnectionPtr;

// 每连接状态：
//  - uid 由后续登录任务写入，当前 Phase 5 占位 0（未登录请求回 1004）；
//  - gate 由注册表的准入/释放回调驱动，绑定本连接的 pauseRead/resumeRead；
//  - input 只在 loop 线程读写，用于 bulk 校验完成后继续 drain 粘包。
struct ConnState {
    std::atomic<std::uint64_t> uid;
    std::shared_ptr<cm::logic::UploadInFlightGate> gate;
    std::shared_ptr<cm::logic::ConnectionDecoder> decoder;
    cm::Buffer* input;

    ConnState() : uid(0), input(NULL) {}
};

class ConnStateTable {
 public:
    void put(const std::string& name, const std::shared_ptr<ConnState>& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_[name] = state;
    }

    std::shared_ptr<ConnState> get(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::string, std::shared_ptr<ConnState> >::const_iterator it =
            map_.find(name);
        if (it == map_.end()) {
            return std::shared_ptr<ConnState>();
        }
        return it->second;
    }

    void erase(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.erase(name);
    }

 private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ConnState> > map_;
};

void sendFramePayload(const ConnectionPtr& conn,
                      cm::protocol::MessageType type, std::uint32_t seq,
                      const std::string& payload) {
    if (!conn || !conn->connected()) {
        return;
    }
    std::string frame;
    if (cm::protocol::Codec::encode(static_cast<std::uint16_t>(type), seq,
                                    payload, &frame) != cm::ErrorCode::Ok) {
        return;
    }
    conn->send(frame);
}

void sendErrorRs(const ConnectionPtr& conn, std::uint32_t seq,
                 std::uint32_t code, const std::string& message) {
    cm::protocol::Writer writer;
    if (!writer.writeU32(code) || !writer.writeString(message)) {
        return;
    }
    sendFramePayload(conn, cm::protocol::MessageType::ErrorRs, seq,
                     writer.data());
}

void sendUploadInitRs(const ConnectionPtr& conn, std::uint32_t seq,
                      const cm::protocol::UploadInitRs& rs) {
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    sendFramePayload(conn, cm::protocol::MessageType::UploadInitRs, seq,
                     payload);
}

void sendUploadChunkRs(const ConnectionPtr& conn, std::uint32_t seq,
                       const cm::protocol::UploadChunkRs& rs) {
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    sendFramePayload(conn, cm::protocol::MessageType::UploadChunkRs, seq,
                     payload);
}

void sendContinueUploadRs(const ConnectionPtr& conn, std::uint32_t seq,
                          const cm::protocol::ContinueUploadRs& rs) {
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    sendFramePayload(conn, cm::protocol::MessageType::ContinueUploadRs, seq,
                     payload);
}

void sendUploadCompleteRs(const ConnectionPtr& conn, std::uint32_t seq,
                          const cm::protocol::UploadCompleteRs& rs) {
    std::string payload;
    if (!rs.encode(&payload)) {
        return;
    }
    sendFramePayload(conn, cm::protocol::MessageType::UploadCompleteRs, seq,
                     payload);
}

// 下载 handler 的发送回调绑定到当前连接（logic 不 include net）。
cm::logic::DownloadHandlers::Conn makeDownloadConn(const ConnectionPtr& conn) {
    cm::logic::DownloadHandlers::Conn c;
    c.sendFrame = [conn](std::uint16_t type, std::uint32_t seq,
                         const std::string& payload) {
        sendFramePayload(conn, static_cast<cm::protocol::MessageType>(type),
                         seq, payload);
    };
    c.sendRaw = [conn](const std::string& frame) {
        if (conn && conn->connected()) {
            conn->send(frame);
        }
    };
    c.sendError = [conn](std::uint32_t seq, std::uint32_t code,
                         const std::string& message) {
        sendErrorRs(conn, seq, code, message);
    };
    c.sendFile = [conn](const std::string& header, int fd,
                        std::int64_t offset, std::size_t len) {
        if (!conn || !conn->connected()) {
            return false;
        }
        conn->send(header);
        // fd 由连接接管；完成回调里连接已断开时无需额外处理。
        return conn->sendFile(fd, offset, len, [conn](bool) {});
    };
    return c;
}

// 10004：业务线程执行；UID 未登录时直接回 1004。
void handleUploadInit(const ConnectionPtr& conn,
                      const cm::protocol::Frame& frame,
                      ConnStateTable& states,
                      cm::logic::UploadService& service) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;

    cm::protocol::UploadInitRs rs;
    if (uid == 0) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::PermissionDenied);
        sendUploadInitRs(conn, frame.seq, rs);
        return;
    }

    cm::protocol::UploadInitRq rq;
    if (!rq.decode(frame.payload)) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::InvalidArgument);
        sendUploadInitRs(conn, frame.seq, rs);
        return;
    }

    const cm::ErrorCode rc = service.init(uid, rq, &rs);
    if (rc != cm::ErrorCode::Ok && rs.code == 0) {
        rs.code = cm::to_wire_code(rc);
    }
    sendUploadInitRs(conn, frame.seq, rs);
}

// 10006：CRC 已在 bulk 路径校验；gate 的 tryBegin/end 由注册表分发路径负责，
// 因此 handler 只做字段解析与业务，不再传 gate。
void handleUploadChunk(const ConnectionPtr& conn,
                       const cm::protocol::Frame& frame,
                       ConnStateTable& states,
                       cm::logic::UploadChunkHandler& handler) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;

    cm::protocol::UploadChunkRs rs;
    if (uid == 0) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::PermissionDenied);
        sendUploadChunkRs(conn, frame.seq, rs);
        return;
    }

    cm::protocol::UploadChunkRq rq;
    if (!rq.decode(frame.payload)) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::InvalidArgument);
        sendUploadChunkRs(conn, frame.seq, rs);
        return;
    }

    const cm::ErrorCode rc = handler.handle(uid, frame.seq, rq, NULL, &rs);
    if (rc != cm::ErrorCode::Ok && rs.code == 0) {
        rs.code = cm::to_wire_code(rc);
    }
    sendUploadChunkRs(conn, frame.seq, rs);
}

// 10028：只读查询；UID 未登录时直接回 1004。
void handleContinueUpload(const ConnectionPtr& conn,
                          const cm::protocol::Frame& frame,
                          ConnStateTable& states,
                          cm::logic::UploadService& service) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;

    cm::protocol::ContinueUploadRs rs;
    if (uid == 0) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::PermissionDenied);
        sendContinueUploadRs(conn, frame.seq, rs);
        return;
    }

    cm::protocol::ContinueUploadRq rq;
    if (!rq.decode(frame.payload)) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::InvalidArgument);
        sendContinueUploadRs(conn, frame.seq, rs);
        return;
    }

    const cm::ErrorCode rc = service.continueUpload(uid, rq, &rs);
    if (rc != cm::ErrorCode::Ok && rs.code == 0) {
        rs.code = cm::to_wire_code(rc);
    }
    sendContinueUploadRs(conn, frame.seq, rs);
}

// 10030：合并 + 落库；UID 未登录时直接回 1004。
void handleUploadComplete(const ConnectionPtr& conn,
                          const cm::protocol::Frame& frame,
                          ConnStateTable& states,
                          cm::logic::UploadService& service) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;

    cm::protocol::UploadCompleteRs rs;
    if (uid == 0) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::PermissionDenied);
        sendUploadCompleteRs(conn, frame.seq, rs);
        return;
    }

    cm::protocol::UploadCompleteRq rq;
    if (!rq.decode(frame.payload)) {
        rs.code = static_cast<std::uint32_t>(cm::ErrorCode::InvalidArgument);
        sendUploadCompleteRs(conn, frame.seq, rs);
        return;
    }

    const cm::ErrorCode rc = service.completeUpload(uid, rq, &rs);
    if (rc != cm::ErrorCode::Ok && rs.code == 0) {
        rs.code = cm::to_wire_code(rc);
    }
    sendUploadCompleteRs(conn, frame.seq, rs);
}

// 10010/10027/10013/10040：下载请求统一走 DownloadHandlers。
void handleDownloadOpen(const ConnectionPtr& conn,
                        const cm::protocol::Frame& frame,
                        ConnStateTable& states,
                        cm::logic::DownloadHandlers& handlers) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;
    handlers.handleOpen(uid, frame, makeDownloadConn(conn));
}

void handleDownloadStat(const ConnectionPtr& conn,
                        const cm::protocol::Frame& frame,
                        ConnStateTable& states,
                        cm::logic::DownloadHandlers& handlers) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;
    handlers.handleStat(uid, frame, makeDownloadConn(conn));
}

void handleDownloadRange(const ConnectionPtr& conn,
                         const cm::protocol::Frame& frame,
                         ConnStateTable& states,
                         cm::logic::DownloadHandlers& handlers) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;
    handlers.handleRange(uid, frame, makeDownloadConn(conn));
}

void handleDownloadDone(const ConnectionPtr& conn,
                        const cm::protocol::Frame& frame,
                        ConnStateTable& states,
                        cm::logic::DownloadHandlers& handlers) {
    std::shared_ptr<ConnState> state = states.get(conn->name());
    const std::uint64_t uid = state ? state->uid.load() : 0;
    handlers.handleDone(uid, frame, makeDownloadConn(conn));
}

int runServer(const cm::ServerConfig& cfg) {
    // 客户端断开后写数据不能把进程写死（TcpConnection 的 send 已用 MSG_NOSIGNAL，
    // 这里再兜住 sendfile/其他路径）。
    ::signal(SIGPIPE, SIG_IGN);

    std::string error;

    cm::db::MysqlOptions mysqlOptions;
    mysqlOptions.host = cfg.mysqlHost;
    mysqlOptions.port = static_cast<unsigned short>(cfg.mysqlPort);
    mysqlOptions.user = cfg.mysqlUser;
    mysqlOptions.password = cfg.mysqlPassword;
    mysqlOptions.database = cfg.mysqlDatabase;

    std::shared_ptr<cm::db::MysqlPool> mysql =
        cm::db::MysqlPool::create(mysqlOptions, &error);
    if (!mysql) {
        std::fprintf(stderr, "mysql init failed: %s\n", error.c_str());
        return 1;
    }

    cm::cache::RedisOptions redisOptions;
    redisOptions.host = cfg.redisHost;
    redisOptions.port = cfg.redisPort;
    redisOptions.password = cfg.redisPassword;
    redisOptions.db = cfg.redisDb;

    std::shared_ptr<cm::cache::RedisPool> redis =
        cm::cache::RedisPool::create(redisOptions, &error);
    if (!redis) {
        std::fprintf(stderr, "redis init failed: %s\n", error.c_str());
        mysql->shutdown();
        return 1;
    }

    cm::cache::CacheStoreOptions cacheOptions;
    cm::cache::CacheStore cache(*redis, cacheOptions);

    cm::logic::UploadService uploadService(*mysql, cache, cfg.dataDir);
    cm::logic::UploadChunkHandler chunkHandler(*mysql, cfg.dataDir);
    cm::logic::DownloadService downloadService(*mysql, *redis, cfg.dataDir);

    net::EventLoop loop;
    net::TimerQueue timers(&loop);
    cm::ThreadPool workers(static_cast<std::size_t>(cfg.workerThreads),
                           "cm-logic");
    // 启动顺序收在 ServerApp：DB 恢复 7→2 → 清理过期 tmp → 注册过期扫描 tick。
    cm::app::ServerApp app(cfg, *mysql, timers, workers);
    ConnStateTable states;

    // 下载块发送：缓冲（D-3）恒可用；sendfile（D-4）按 CM_DOWNLOAD_ZEROCOPY。
    // 读块/懒填充走 ServerApp 的 IO 池；发送回调在 handler 里按请求绑定连接。
    cm::logic::BlockSender bufferedSender(
        *mysql, *redis,
        [&app](cm::logic::BlockSender::Task t) {
            return app.ioPool().run(std::move(t));
        },
        cfg.dataDir);
    cm::logic::DownloadHandlers downloadHandlers(downloadService, bufferedSender);
#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)
    cm::logic::SendfileBlockSender sendfileSender(
        *mysql, *redis,
        [&app](cm::logic::SendfileBlockSender::Task t) {
            return app.ioPool().run(std::move(t));
        },
        cfg.dataDir);
    downloadHandlers.setSendfileSender(sendfileSender);
#endif

    net::HandlerRegistry registry(
        &workers,
        [](const ConnectionPtr& conn, std::uint32_t seq, std::uint32_t code,
           const std::string& message) {
            sendErrorRs(conn, seq, code, message);
        });

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::UploadInitRq),
            [&states, &uploadService](const ConnectionPtr& conn,
                                      const cm::protocol::Frame& frame) {
                handleUploadInit(conn, frame, states, uploadService);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register UploadInit failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::ContinueUploadRq),
            [&states, &uploadService](const ConnectionPtr& conn,
                                      const cm::protocol::Frame& frame) {
                handleContinueUpload(conn, frame, states, uploadService);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register ContinueUpload failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::UploadCompleteRq),
            [&states, &uploadService](const ConnectionPtr& conn,
                                      const cm::protocol::Frame& frame) {
                handleUploadComplete(conn, frame, states, uploadService);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register UploadComplete failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadOpenRq),
            [&states, &downloadHandlers](const ConnectionPtr& conn,
                                         const cm::protocol::Frame& frame) {
                handleDownloadOpen(conn, frame, states, downloadHandlers);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register DownloadOpen failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadStatRq),
            [&states, &downloadHandlers](const ConnectionPtr& conn,
                                         const cm::protocol::Frame& frame) {
                handleDownloadStat(conn, frame, states, downloadHandlers);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register DownloadStat failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadRangeRq),
            [&states, &downloadHandlers](const ConnectionPtr& conn,
                                         const cm::protocol::Frame& frame) {
                handleDownloadRange(conn, frame, states, downloadHandlers);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register DownloadRange failed\n");
        return 1;
    }

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::DownloadDoneRq),
            [&states, &downloadHandlers](const ConnectionPtr& conn,
                                         const cm::protocol::Frame& frame) {
                handleDownloadDone(conn, frame, states, downloadHandlers);
            }) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register DownloadDone failed\n");
        return 1;
    }

    // 10006 的准入/释放绑定到每连接的 UploadInFlightGate：
    // tryBegin 在 IO 线程投递前执行，end 在业务线程 handler 返回后执行。
    net::HandlerRegistry::Admission admit =
        [&states](const ConnectionPtr& conn) {
            std::shared_ptr<ConnState> state = states.get(conn->name());
            return state && state->gate && state->gate->tryBegin();
        };
    net::HandlerRegistry::Release release =
        [&states](const ConnectionPtr& conn) {
            std::shared_ptr<ConnState> state = states.get(conn->name());
            if (state && state->gate) {
                state->gate->end();
            }
        };

    if (registry.registerHandler(
            static_cast<std::uint16_t>(
                cm::protocol::MessageType::UploadChunkRq),
            [&states, &chunkHandler](const ConnectionPtr& conn,
                                     const cm::protocol::Frame& frame) {
                handleUploadChunk(conn, frame, states, chunkHandler);
            },
            admit, release) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "register UploadChunk failed\n");
        return 1;
    }

    net::TcpServer server(&loop, "cloud-media");
    net::SignalHandler signals(&loop);

    server.setMessageCallback(
        [&states](const ConnectionPtr& conn, cm::Buffer* buffer,
                  net::Timestamp) {
            std::shared_ptr<ConnState> state = states.get(conn->name());
            if (!state || !state->decoder) {
                return;
            }
            state->input = buffer;
            state->decoder->onData(*buffer);
        });

    server.setConnectionCallback([&states, &registry, &workers](
                                     const ConnectionPtr& conn) {
        // handleClose 会以 connected()==false 再回调一次，不能为已断开连接建状态。
        if (!conn->connected()) {
            return;
        }
        std::shared_ptr<ConnState> state(new ConnState());
        state->gate.reset(new cm::logic::UploadInFlightGate(
            2,
            [conn]() { conn->pauseRead(); },
            [conn]() { conn->resumeRead(); }));

        std::weak_ptr<ConnState> weakState = state;
        state->decoder = std::make_shared<cm::logic::ConnectionDecoder>(
            [&registry, conn](const cm::protocol::Frame& frame) {
                registry.dispatch(frame.type, conn, frame);
            },
            [&registry, &workers, conn](
                std::shared_ptr<cm::protocol::RawFrame> raw,
                std::function<void(bool)> done) {
                std::function<void()> task = [&registry, conn, raw, done]() {
                    // 连接已断开：不做业务，回 loop 让门闩收尾。
                    if (!conn->connected()) {
                        done(false);
                        return;
                    }
                    // H2：4MB CRC 必须在业务线程校验。
                    if (!cm::protocol::Codec::verifyFrameCrc(*raw)) {
                        done(false);  // decoder 回 9005 并关闭
                        return;
                    }
                    cm::protocol::Frame frame;
                    frame.type = raw->type;
                    frame.seq = raw->seq;
                    frame.payload.swap(raw->payload);
                    registry.dispatchInline(raw->type, conn, frame);
                    done(true);
                };
                if (!workers.run(task)) {
                    done(false);
                }
            },
            [conn]() { conn->pauseRead(); },
            [conn]() { conn->resumeRead(); },
            [conn]() { conn->forceClose(); },
            // bulk 校验完成后必须回到连接所属 loop（H2）：onBulkVerified 会
            // resumeRead 并 drain Buffer，绝不能落到业务线程。
            [conn](std::function<void()> fn) {
                conn->getLoop()->runInLoop(fn);
            },
            [weakState, conn]() {
                // 连接已断开时不解析残留 Buffer，避免为死连接触发业务。
                if (!conn->connected()) {
                    return;
                }
                std::shared_ptr<ConnState> state = weakState.lock();
                if (state && state->input != NULL && state->decoder) {
                    state->decoder->onData(*state->input);
                }
            },
            [conn](std::uint32_t seq, std::uint32_t code,
                   const std::string& message) {
                sendErrorRs(conn, seq, code, message);
            });

        states.put(conn->name(), state);
    });

    server.setCloseCallback([&states](const ConnectionPtr& conn) {
        states.erase(conn->name());
    });

    signals.setCallback([&loop, &server, &timers](int) {
        server.stopAccepting();
        server.shutdownAll();
        timers.runAfter(500, [&loop]() { loop.quit(); });
    });
    if (!signals.install({SIGINT, SIGTERM})) {
        std::fprintf(stderr, "install signal handler failed\n");
        return 1;
    }

    // 先完成恢复/清理/tick 注册，再 listen。
    if (!app.start(&error)) {
        std::fprintf(stderr, "server app start failed: %s\n", error.c_str());
        app.stop();
        workers.stop();
        mysql->shutdown();
        redis->shutdown();
        return 1;
    }

    const std::uint16_t port = static_cast<std::uint16_t>(cfg.listenPort);
    if (!server.start(port)) {
        std::fprintf(stderr, "listen on port %u failed\n",
                     static_cast<unsigned>(port));
        return 1;
    }

    // Phase 5 的 TcpServer 仍是单 loop；ioThreads 留给后续多 Reactor。
    std::fprintf(stdout,
                 "cloud-media-server listening on 0.0.0.0:%u "
                 "(loop=1, workers=%d, data=%s)\n",
                 static_cast<unsigned>(server.port()), cfg.workerThreads,
                 cfg.dataDir.c_str());
    std::fflush(stdout);

    loop.loop();

    // 先断连接，再等业务任务排空（stop 会 drain 队列），最后关池。
    // registry 的 handler 任务会引用 registry，必须保证 stop 之后才析构。
    server.stop();
    workers.stop();
    app.stop();  // 停 IO 池前会 drain 已投递的目录删除
    mysql->shutdown();
    redis->shutdown();
    return 0;
}

#endif  // CM_WITH_STORAGE

}  // namespace

int main(int argc, char** argv) {
    std::string configPath;
    int portOverride = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("cloud-media-server %s (protocol v%d)\n",
                        cm::version(), cm::protocol::version());
            return 0;
        }
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            configPath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            portOverride = std::atoi(argv[++i]);
            if (portOverride <= 0 || portOverride > 65535) {
                std::fprintf(stderr, "invalid port: %s\n", argv[i]);
                return 2;
            }
            continue;
        }
        printUsage(argv[0]);
        return 2;
    }

    if (configPath.empty()) {
        printUsage(argv[0]);
        return 2;
    }

    cm::ServerConfig cfg;
    std::string error;
    if (cm::loadServerConfig(configPath, &cfg, &error) != cm::ErrorCode::Ok) {
        std::fprintf(stderr, "load config %s failed: %s\n", configPath.c_str(),
                     error.c_str());
        return 2;
    }
    if (portOverride > 0) {
        cfg.listenPort = portOverride;
    }

#if defined(CM_WITH_STORAGE)
    return runServer(cfg);
#else
    (void)cfg;
    std::fprintf(stderr,
                 "cloud-media-server requires BUILD_STORAGE=ON "
                 "(MySQL/Redis/logic are unavailable).\n");
    return 1;
#endif
}
