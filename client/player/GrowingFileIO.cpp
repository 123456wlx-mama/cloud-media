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
// 下载完全停滞的上限：超过就放弃本次读取。
// open 是在 GUI 线程同步跑的，这里无限等待会直接把界面卡成"未响应"。
const int kMaxStallMs = 5000;

}  // namespace

GrowingFileIO::GrowingFileIO(const QString& path, bool progressive,
                             qint64 totalBytes)
    : m_path(path), m_progressive(progressive),
      m_totalBytes(totalBytes > 0 ? totalBytes : 0),
      // open 时文件里已有的字节一定来自已写完的块（首块落盘后才 open），
      // 以此作为初始可读边界；后续由下载线程通过 commitReadable 推进。
      m_committedBytes(progressive ? QFileInfo(path).size() : 0), m_seekLogs(0),
      m_file(nullptr), m_avio(nullptr), m_buffer(nullptr), m_pos(0),
      m_complete(!progressive), m_abort(false), m_stalled(false) {}

void GrowingFileIO::commitReadable(qint64 bytes) {
    if (bytes <= 0) {
        return;
    }
    qint64 cur = m_committedBytes.load();
    while (bytes > cur && !m_committedBytes.compare_exchange_weak(cur, bytes)) {
    }
    m_cond.notify_all();
}

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
    // 对外暴露"现在能读到多少"，而不是磁盘上的文件长度。
    // Windows 上 QFile::write(4MB) 会先把长度撑到目标值、数据再逐页落盘，
    // 直接返回 size() 会让缓冲进度条超前于真实可读数据。
    return readableBytes();
}

qint64 GrowingFileIO::readableLocked() {
    // 调用方必须已持有 m_mutex。
    // 对 demuxer 暴露的可读边界 = min(磁盘上的长度, 下载线程确认写完的长度)。
    //
    // 为什么不能只用 QFile::size()：Windows 上 QFile::write(4MB) 会先把文件
    // 长度撑到写完之后的位置，数据才逐页落盘；这段时间里 size() 是"超前"的，
    // demuxer 冲进去读到的是一片零，H.264 会把它当 NAL 解析，
    // 表现为 "reference picture missing" / "error while decoding MB ... bytestream N"，
    // 而音频流容错强，听感上常常还是"有声音没画面"。
    if (m_file == nullptr || !m_file->isOpen()) {
        return 0;
    }
    qint64 limit = m_file->size();
    const qint64 committed = m_committedBytes.load();
    // 未调用过 commitReadable（本地文件/老调用方）：退化为磁盘大小，行为与以前一致
    if (committed > 0 && committed < limit) {
        limit = committed;
    }
    // 远端总大小是硬上限：磁盘上绝不该有比它更多的有效字节
    if (m_totalBytes > 0 && m_totalBytes < limit) {
        limit = m_totalBytes;
    }
    return limit;
}

qint64 GrowingFileIO::readableBytes() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file != nullptr && m_file->isOpen()) {
        return readableLocked();
    }
    // open 之前（例如 openMedia 里打印诊断）：只读元数据，不抢文件句柄
    qint64 limit = QFileInfo(m_path).size();
    const qint64 committed = m_committedBytes.load();
    if (committed > 0 && committed < limit) {
        limit = committed;
    }
    if (m_totalBytes > 0 && m_totalBytes < limit) {
        limit = m_totalBytes;
    }
    return limit;
}

qint64 GrowingFileIO::sizeHint() {
    // 只暴露「已经落盘且确认写完的大小」。
    // 绝对不能返回远端总大小：那会让 demuxer seek 到尚未下载的区域，
    // 而推进下载进度的回调跑在 GUI 线程——open 又在 GUI 线程同步执行，
    // 于是互相等待，整个界面卡死（未响应）。
    // moov 在头部的 MP4 本来就不需要总大小；需要尾部索引的容器，
    // 只能靠 HLS Remux 或等下载完成，不能靠这一个数字硬撑。
    return readableBytes();
}

int GrowingFileIO::readPacket(void* opaque, std::uint8_t* buf, int size) {
    GrowingFileIO* self = static_cast<GrowingFileIO*>(opaque);
    std::unique_lock<std::mutex> lock(self->m_mutex);

    qint64 lastAvail = -1;  // 上一轮看到的数据量，用来判断下载是否还在推进
    int stalledMs = 0;      // 连续停滞的累计时长
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
        // 注意用 readableLocked()：不能直接看 QFile::size()，否则会读到
        // "文件已扩容、数据还没写完"的零区（见函数注释）。
        const qint64 avail = self->readableLocked();
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
            if (self->m_pos == 0 && got >= 16) {
                // 诊断：与"服务端前 16 字节"对照，判断读到的到底是不是文件真身
                qWarning("player: io first16 %s",
                         QByteArray(reinterpret_cast<const char*>(buf), 16)
                             .toHex(' ')
                             .constData());
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
        // 只要下载还在推进就不算卡住；完全停滞才放弃，避免 GUI 永久未响应。
        if (avail == lastAvail) {
            stalledMs += kWaitMs;
            if (stalledMs >= kMaxStallMs) {
                return AVERROR_EXIT;
            }
        } else {
            stalledMs = 0;
            lastAvail = avail;
        }
    }
}

std::int64_t GrowingFileIO::seek(void* opaque, std::int64_t offset, int whence) {
    GrowingFileIO* self = static_cast<GrowingFileIO*>(opaque);
    if ((whence & AVSEEK_SIZE) != 0) {
        return self->sizeHint();
    }
    const int base = whence & ~AVSEEK_FORCE;
    std::int64_t target = offset;
    if (base == SEEK_CUR) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        target = self->m_pos + offset;
    } else if (base == SEEK_END) {
        target = self->sizeHint() + offset;
    }
    if (target < 0) {
        target = 0;
    }
    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        // 诊断：只打前 24 次，观察 demuxer 的定位模式是否异常
        if (self->m_seekLogs < 24) {
            ++self->m_seekLogs;
            qWarning("player: io seek from=%lld to=%lld (whence=%d)",
                     static_cast<long long>(self->m_pos),
                     static_cast<long long>(target), base);
        }
        self->m_pos = target;
    }
    return target;
}

}  // namespace player
}  // namespace cm
