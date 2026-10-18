#pragma once

#include <functional>
#include <memory>

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

    ConnectionDecoder(FrameHandler frameHandler, BulkHandler bulkHandler,
                      Action pauseRead, Action resumeRead, Action forceClose,
                      Post post);

    Result onData(Buffer& in);
    void onBulkVerified(bool ok);

    bool bulkPending() const { return bulkPending_; }

private:
    void fail();

    FrameHandler frameHandler_;
    BulkHandler bulkHandler_;
    Action pauseRead_;
    Action resumeRead_;
    Action forceClose_;
    Post post_;
    bool bulkPending_;
    bool closed_;
};

}  // namespace logic
}  // namespace cm
