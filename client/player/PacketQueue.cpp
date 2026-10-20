#include "player/PacketQueue.h"

#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace cm {
namespace player {

PacketQueue::PacketQueue() : m_bytes(0), m_abort(false) {}

PacketQueue::~PacketQueue() { flush(); }

void PacketQueue::put(AVPacket* packet) {
    if (packet == nullptr) {
        return;
    }
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr) {
        av_packet_unref(packet);
        return;
    }
    av_packet_move_ref(copy, packet);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(copy);
        m_bytes += copy->size;
    }
    m_cond.notify_one();
}

bool PacketQueue::get(AVPacket* out, bool block, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (block) {
        m_cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
            return m_abort || !m_queue.empty();
        });
    }
    if (m_abort || m_queue.empty()) {
        return false;
    }
    AVPacket* head = m_queue.front();
    m_queue.pop_front();
    m_bytes -= head->size;
    av_packet_move_ref(out, head);
    av_packet_free(&head);
    return true;
}

void PacketQueue::flush() {
    std::deque<AVPacket*> drained;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        drained.swap(m_queue);
        m_bytes = 0;
    }
    for (std::size_t i = 0; i < drained.size(); ++i) {
        av_packet_free(&drained[i]);
    }
}

void PacketQueue::abort() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abort = true;
    }
    m_cond.notify_all();
}

void PacketQueue::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = false;
}

int PacketQueue::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

int PacketQueue::bytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bytes;
}

bool PacketQueue::empty() const { return count() == 0; }

bool PacketQueue::aborted() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_abort;
}

}  // namespace player
}  // namespace cm
