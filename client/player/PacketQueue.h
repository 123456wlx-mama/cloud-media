#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

struct AVPacket;

namespace cm {
namespace player {

// 解码包队列：解封装线程放，视频线程/音频回调取。
// 用 std::mutex + condition_variable 替代 legacy 的 SDL_mutex（C++11 规范）。
class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // 转移所有权（内部 av_packet_move_ref，调用方之后可 unref）。
    void put(AVPacket* packet);
    // block=true 时最多等 timeoutMs；返回 false 表示超时/已 abort/为空。
    bool get(AVPacket* out, bool block, int timeoutMs = 30);

    void flush();
    void abort();  // 唤醒阻塞的 get，并让后续 get 立即返回 false
    void reset();  // 清 abort 标志（重新打开时用）

    int count() const;
    int bytes() const;
    bool empty() const;
    bool aborted() const;

private:
    std::deque<AVPacket*> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    int m_bytes;
    bool m_abort;
};

}  // namespace player
}  // namespace cm
