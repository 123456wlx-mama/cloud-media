#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "cm/base/buffer.h"
#include "cm/protocol/frame.h"

namespace cm {
namespace logic {

// 每连接一个，只在连接所属 loop 线程调用 onData / onBulkVerified。
// 普通帧在 onData 内循环解析；10006 大帧只解析一次并停住，
// 直到业务线程校验完成并通过 post 回到 loop 调用 onBulkVerified。
// onBulkVerified(true) 之后，调用方必须在 loop 线程再次 onData，
// 处理可能已经粘在 Buffer 里的下一帧。
class ConnectionDecoder : public std::enable_shared_from_this<ConnectionDecoder> {
public:
    enum class Result {
        NeedMore,
        BulkDispatched,
        Deferred,
        Error
    };

    using FrameHandler = std::function<void(const protocol::Frame&)>;
    using BulkHandler = std::function<void(
        std::shared_ptr<protocol::RawFrame>, std::function<void(bool)>)>;
    using Action = std::function<void()>;
    using Post = std::function<void(std::function<void()>)>;
    // 解码/CRC 失败时先回 ERROR_RS 再关闭；seq 无法解析时为 0。
    using ErrorHandler = std::function<void(std::uint32_t seq,
                                            std::uint32_t wireCode,
                                            const std::string& message)>;

    // drain：bulk 校验通过、resumeRead 之后再次驱动 onData，
    // 消费 Buffer 里已经粘着的下一帧（ET 下不会再有新的 EPOLLIN）。
    ConnectionDecoder(FrameHandler frameHandler, BulkHandler bulkHandler,
                      Action pauseRead, Action resumeRead, Action forceClose,
                      Post post, Action drain = Action(),
                      ErrorHandler errorHandler = ErrorHandler());

    Result onData(Buffer& in);
    void onBulkVerified(bool ok);

    bool bulkPending() const { return bulkPending_; }

private:
    void fail(std::uint32_t seq, protocol::DecodeStatus status);

    FrameHandler frameHandler_;
    BulkHandler bulkHandler_;
    Action pauseRead_;
    Action resumeRead_;
    Action forceClose_;
    Post post_;
    Action drain_;
    ErrorHandler errorHandler_;
    std::uint32_t bulkSeq_;
    bool bulkPending_;
    bool closed_;
};

}  // namespace logic
}  // namespace cm
