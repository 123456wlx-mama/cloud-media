#include "handler_registry.h"

#include <memory>
#include <utility>

#include "cm/base/thread_pool.h"

namespace net {

HandlerRegistry::HandlerRegistry(cm::ThreadPool* workers,
                                 ErrorResponder errorResponder)
    : workers_(workers), errorResponder_(std::move(errorResponder)) {
}

HandlerRegistry::~HandlerRegistry() {
}

cm::ErrorCode HandlerRegistry::registerHandler(std::uint16_t type,
                                               Handler handler,
                                               Admission admission,
                                               Release release) {
    if (type == 0 || !handler) {
        return cm::ErrorCode::InvalidArgument;
    }

    Entry entry;
    entry.handler = std::move(handler);
    entry.admission = std::move(admission);
    entry.release = std::move(release);

    std::lock_guard<std::mutex> lock(mutex_);
    if (handlers_.find(type) != handlers_.end()) {
        return cm::ErrorCode::InvalidArgument;
    }
    handlers_.insert(std::make_pair(type, std::move(entry)));
    return cm::ErrorCode::Ok;
}

bool HandlerRegistry::find(std::uint16_t type, Entry* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::uint16_t, Entry>::const_iterator it = handlers_.find(type);
    if (it == handlers_.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

cm::ErrorCode HandlerRegistry::dispatch(std::uint16_t type,
                                        const ConnectionPtr& conn,
                                        const cm::protocol::Frame& frame) {
    Entry entry;
    if (!find(type, &entry)) {
        respond(conn, frame.seq, unregisteredTypeCode(), "unknown type");
        return cm::ErrorCode::ProtocolError;
    }

    // 准入必须在投递前完成：满额时 UploadInFlightGate 内部已经 pauseRead，
    // 当前帧无法再退回 Buffer，直接回 1006 让客户端退避重试。
    if (entry.admission && !entry.admission(conn)) {
        respond(conn, frame.seq,
                static_cast<std::uint32_t>(cm::ErrorCode::Timeout), "busy");
        return cm::ErrorCode::Timeout;
    }

    if (workers_ == NULL) {
        runHandler(entry, conn, frame);
        return cm::ErrorCode::Ok;
    }

    // Frame 可能到 8MB（普通帧），但只在 IO 线程复制一次；10006 走 dispatchInline。
    std::shared_ptr<cm::protocol::Frame> shared(
        new cm::protocol::Frame(frame));
    if (!workers_->run([this, entry, conn, shared]() {
            runHandler(entry, conn, *shared);
        })) {
        if (entry.release) {
            entry.release(conn);
        }
        respond(conn, frame.seq,
                static_cast<std::uint32_t>(cm::ErrorCode::Timeout), "busy");
        return cm::ErrorCode::Timeout;
    }

    return cm::ErrorCode::Ok;
}

cm::ErrorCode HandlerRegistry::dispatchInline(std::uint16_t type,
                                              const ConnectionPtr& conn,
                                              const cm::protocol::Frame& frame) {
    Entry entry;
    if (!find(type, &entry)) {
        respond(conn, frame.seq, unregisteredTypeCode(), "unknown type");
        return cm::ErrorCode::ProtocolError;
    }
    if (entry.admission && !entry.admission(conn)) {
        respond(conn, frame.seq,
                static_cast<std::uint32_t>(cm::ErrorCode::Timeout), "busy");
        return cm::ErrorCode::Timeout;
    }

    runHandler(entry, conn, frame);
    return cm::ErrorCode::Ok;
}

void HandlerRegistry::runHandler(const Entry& entry, const ConnectionPtr& conn,
                                 const cm::protocol::Frame& frame) {
    if (entry.handler) {
        entry.handler(conn, frame);
    }
    // 无条件释放准入名额；handler 内部早返回不影响这里。
    if (entry.release) {
        entry.release(conn);
    }
}

bool HandlerRegistry::hasHandler(std::uint16_t type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.find(type) != handlers_.end();
}

std::size_t HandlerRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

void HandlerRegistry::respond(const ConnectionPtr& conn, std::uint32_t seq,
                              std::uint32_t wireCode,
                              const char* message) const {
    if (errorResponder_) {
        errorResponder_(conn, seq, wireCode,
                        message == NULL ? std::string() : std::string(message));
    }
}

std::uint32_t HandlerRegistry::unregisteredTypeCode() {
    return static_cast<std::uint32_t>(cm::ProtocolError::UnknownType);
}

std::uint32_t HandlerRegistry::decodeFailureCode() {
    return static_cast<std::uint32_t>(cm::ErrorCode::InvalidArgument);
}

std::uint32_t HandlerRegistry::wireCodeForDecodeStatus(
    cm::protocol::DecodeStatus status) {
    if (status == cm::protocol::DecodeStatus::Ok ||
        status == cm::protocol::DecodeStatus::NeedMore) {
        return 0;
    }
    return cm::decode_failure_wire_code(
        status == cm::protocol::DecodeStatus::BadCrc);
}

}  // namespace net
