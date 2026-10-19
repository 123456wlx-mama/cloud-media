#include "connection_decoder.h"

#include <cstdint>
#include <utility>

#include "cm/base/byte_order.h"
#include "cm/base/error_code.h"
#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"

namespace cm {
namespace logic {
namespace {

// 失败时 Buffer 尚未消费；足够 19 字节就能取出 SEQ 用于 ERROR_RS 回显。
std::uint32_t frameSeq(const Buffer& in) {
    if (in.readableBytes() < protocol::kHeaderSize) {
        return 0;
    }
    return readU32BE(
        reinterpret_cast<const std::uint8_t*>(in.peek()) + 11);
}

}  // namespace

ConnectionDecoder::ConnectionDecoder(FrameHandler frameHandler,
                                     BulkHandler bulkHandler,
                                     Action pauseRead, Action resumeRead,
                                     Action forceClose, Post post,
                                     Action drain, ErrorHandler errorHandler)
    : frameHandler_(frameHandler),
      bulkHandler_(bulkHandler),
      pauseRead_(pauseRead),
      resumeRead_(resumeRead),
      forceClose_(forceClose),
      post_(post),
      drain_(drain),
      errorHandler_(errorHandler),
      bulkSeq_(0),
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
                fail(frameSeq(in), status);
                return Result::Error;
            }

            bulkSeq_ = raw.seq;
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
            fail(frameSeq(in), status);
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
        fail(bulkSeq_, protocol::DecodeStatus::BadCrc);
        return;
    }
    if (resumeRead_) {
        resumeRead_();
    }
    // ET 下粘在 Buffer 里的下一帧不会再有 EPOLLIN，必须在这里继续 drain。
    if (drain_) {
        drain_();
    }
}

void ConnectionDecoder::fail(std::uint32_t seq,
                             protocol::DecodeStatus status) {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (errorHandler_) {
        const char* message =
            status == protocol::DecodeStatus::BadCrc ? "crc mismatch"
                                                     : "bad frame";
        errorHandler_(
            seq,
            cm::decode_failure_wire_code(status == protocol::DecodeStatus::BadCrc),
            std::string(message));
    }
    if (forceClose_) {
        forceClose_();
    }
}

}  // namespace logic
}  // namespace cm
