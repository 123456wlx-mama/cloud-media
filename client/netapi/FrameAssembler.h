#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cm/base/buffer.h"
#include "cm/protocol/frame.h"

namespace cm {
namespace netapi {

// 帧重组：喂字节流，吐完整帧。负责粘包/半包，坏帧返回 false 由调用方断链。
// 与 Qt 无关，便于单测（client/tests/test_frame_assembler.cpp）。
class FrameAssembler {
public:
    FrameAssembler();

    // 追加 len 字节，把能解析出的完整帧依次 append 到 out。
    // 返回 false 表示字节流损坏（BadMagic/BadVersion/BadLength/BadCrc），调用方应断开。
    bool feed(const char* data, std::size_t len,
              std::vector<protocol::Frame>* out);

    void reset();

private:
    Buffer m_buffer;
};

}  // namespace netapi
}  // namespace cm
