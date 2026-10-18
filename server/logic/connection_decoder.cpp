#include "connection_decoder.h"

#include <cstdint>
#include <utility>

#include "cm/base/byte_order.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"

namespace cm {
namespace logic {

ConnectionDecoder::ConnectionDecoder(FrameHandler frameHandler,
                                     BulkHandler bulkHandler,
                                     Action pauseRead, Action resumeRead,
                                     Action forceClose, Post post)
    : frameHandler_(frameHandler),
      bulkHandler_(bulkHandler),
      pauseRead_(pauseRead),
      resumeRead_(resumeRead),
      forceClose_(forceClose),
      post_(post),
      bulkPending_(false),
      closed_(false) {
}

ConnectionDecoder::Result ConnectionDecoder::onData(Buffer& in) {
    if (closed_) {
        return Result::Error;
    }
    if (bulkPending_) {
        return Result::Deferred;
    }

    for (;;) {
        if (in.readableBytes() < protocol::kHeaderSize) {
            return Result::NeedMore;
        }

        const std::uint8_t* header =
            reinterpret_cast<const std::uint8_t*>(in.peek());
        const std::uint16_t type = readU16BE(header + 5);

        if (type == static_cast<std::uint16_t>(protocol::MessageType::UploadChunkRq)) {
            protocol::RawFrame raw;
            protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
            const ErrorCode code = protocol::Codec::decodeBulk(in, &raw, &status);
            if (code == ErrorCode::NeedMore) {
                return Result::NeedMore;
            }
            if (code != ErrorCode::Ok || status != protocol::DecodeStatus::Ok) {
                fail();
                return Result::Error;
            }

            bulkPending_ = true;
            if (pauseRead_) {
                pauseRead_();
            }

            std::shared_ptr<protocol::RawFrame> frame(
                new protocol::RawFrame(std::move(raw)));
            std::weak_ptr<ConnectionDecoder> weak = shared_from_this();
            const Post post = post_;
            std::function<void(bool)> done =
                [weak, post](bool ok) {
                    post([weak, ok]() {
                        std::shared_ptr<ConnectionDecoder> self = weak.lock();
                        if (self) {
                            self->onBulkVerified(ok);
                        }
                    });
                };
            if (bulkHandler_) {
                bulkHandler_(frame, done);
            } else {
                done(false);
            }
            return Result::BulkDispatched;
        }

        protocol::Frame frame;
        protocol::DecodeStatus status = protocol::DecodeStatus::NeedMore;
        const ErrorCode code = protocol::Codec::decode(in, &frame, &status);
        if (code == ErrorCode::NeedMore) {
            return Result::NeedMore;
        }
        if (code != ErrorCode::Ok || status != protocol::DecodeStatus::Ok) {
            fail();
            return Result::Error;
        }
        if (frameHandler_) {
            frameHandler_(frame);
        }
        in.shrinkTo(64 * 1024);
    }
}

void ConnectionDecoder::onBulkVerified(bool ok) {
    if (closed_ || !bulkPending_) {
        return;
    }
    bulkPending_ = false;
    if (!ok) {
        fail();
        return;
    }
    if (resumeRead_) {
        resumeRead_();
    }
}

void ConnectionDecoder::fail() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (forceClose_) {
        forceClose_();
    }
}

}  // namespace logic
}  // namespace cm
