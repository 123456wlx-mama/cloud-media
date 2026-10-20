#include "player/GrowingFileIO.h"

#include <QFile>
#include <QFileInfo>

#include <chrono>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace cm {
namespace player {
namespace {

const int kIoBufferSize = 64 * 1024;
const int kWaitMs = 30;  // 等新数据时每次最多睡 30ms，保证 close 能及时退出

}  // namespace

GrowingFileIO::GrowingFileIO(const QString& path, bool progressive)
    : m_path(path), m_progressive(progressive), m_file(nullptr),
      m_avio(nullptr), m_buffer(nullptr), m_pos(0), m_complete(!progressive),
      m_abort(false), m_stalled(false) {}

GrowingFileIO::~GrowingFileIO() {
    abort();
    if (m_avio != nullptr) {
        av_freep(&m_avio->buffer);
        avio_context_free(&m_avio);
    }
    if (m_file != nullptr) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
}

bool GrowingFileIO::init(QString* error) {
    m_buffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferSize));
    if (m_buffer == nullptr) {
        *error = QString::fromUtf8("内存不足");
        return false;
    }
    m_avio = avio_alloc_context(m_buffer, kIoBufferSize, 0, this,
                                &GrowingFileIO::readPacket, nullptr,
                                &GrowingFileIO::seek);
    if (m_avio == nullptr) {
        av_free(m_buffer);
        m_buffer = nullptr;
        *error = QString::fromUtf8("初始化 IO 失败");
        return false;
    }
    m_file = new QFile(m_path);
    return true;
}

void GrowingFileIO::setStallCallback(const std::function<void(bool)>& cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stallCb = cb;
}

void GrowingFileIO::setSourceComplete(bool complete) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_complete = complete;
    }
    m_cond.notify_all();
}

void GrowingFileIO::abort() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abort = true;
    }
    m_cond.notify_all();
}

void GrowingFileIO::setStalled(bool stalled) {
    std::function<void(bool)> cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stalled == stalled) {
            return;
        }
        m_stalled = stalled;
        cb = m_stallCb;
    }
    if (cb) {
        cb(stalled);
    }
}

bool GrowingFileIO::openFile() {
    if (m_file->isOpen()) {
        return true;
    }
    return m_file->open(QIODevice::ReadOnly);
}

qint64 GrowingFileIO::availableBytes() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file == nullptr) {
        return 0;
    }
    if (!m_file->isOpen() && !openFile()) {
        return 0;
    }
    return m_file->size();
}

int GrowingFileIO::readPacket(void* opaque, std::uint8_t* buf, int size) {
    GrowingFileIO* self = static_cast<GrowingFileIO*>(opaque);
    std::unique_lock<std::mutex> lock(self->m_mutex);

    while (true) {
        if (self->m_abort) {
            return AVERROR_EXIT;
        }
        if (self->m_file != nullptr && !self->m_file->isOpen()) {
            // 边下边播时文件可能还没落盘：等一会儿再试
            if (!self->m_file->open(QIODevice::ReadOnly) && !self->m_complete) {
                lock.unlock();
                self->setStalled(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
                lock.lock();
                continue;
            }
        }
        const qint64 avail = (self->m_file != nullptr && self->m_file->isOpen())
                                 ? self->m_file->size()
                                 : 0;
        if (self->m_pos < avail) {
            // setStalled 会自己加锁，先放开再调，避免自死锁
            lock.unlock();
            self->setStalled(false);
            lock.lock();
            const qint64 want = avail - self->m_pos;
            const int toRead = want < size ? static_cast<int>(want) : size;
            if (!self->m_file->seek(self->m_pos)) {
                return AVERROR(EIO);
            }
            const qint64 got = self->m_file->read(reinterpret_cast<char*>(buf), toRead);
            if (got <= 0) {
                return AVERROR(EIO);
            }
            self->m_pos += got;
            return static_cast<int>(got);
        }
        if (self->m_complete || !self->m_progressive) {
            return AVERROR_EOF;
        }
        // 还没下到这一块：解锁等待（被 abort/complete 唤醒）
        lock.unlock();
        self->setStalled(true);
        {
            std::unique_lock<std::mutex> waitLock(self->m_mutex);
            self->m_cond.wait_for(waitLock, std::chrono::milliseconds(kWaitMs));
        }
        lock.lock();
    }
}

std::int64_t GrowingFileIO::seek(void* opaque, std::int64_t offset, int whence) {
    GrowingFileIO* self = static_cast<GrowingFileIO*>(opaque);
    if ((whence & AVSEEK_SIZE) != 0) {
        return self->availableBytes();
    }
    const int base = whence & ~AVSEEK_FORCE;
    std::int64_t target = offset;
    if (base == SEEK_CUR) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        target = self->m_pos + offset;
    } else if (base == SEEK_END) {
        target = self->availableBytes() + offset;
    }
    if (target < 0) {
        target = 0;
    }
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_pos = target;
    }
    return target;
}

}  // namespace player
}  // namespace cm
