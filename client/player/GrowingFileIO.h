#pragma once

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

class QFile;
struct AVIOContext;

namespace cm {
namespace player {

// 可增长的文件 IO：边下边播时 demuxer 会在文件还没写完的地方读，
// 这里阻塞等待新数据（而不是立刻 EOF），下载完成后才返回真正的 EOF。
class GrowingFileIO {
public:
    // totalBytes>0：远端文件真实总大小（边下边播必须传）。
    // 它只用于回答 AVSEEK_SIZE / SEEK_END，不能用来判断"能不能读"。
    GrowingFileIO(const QString& path, bool progressive, qint64 totalBytes = 0);
    ~GrowingFileIO();

    GrowingFileIO(const GrowingFileIO&) = delete;
    GrowingFileIO& operator=(const GrowingFileIO&) = delete;

    bool init(QString* error);
    AVIOContext* context() const { return m_avio; }

    // 下载完成：允许 read 返回 EOF（否则一直等新块）
    void setSourceComplete(bool complete);
    bool sourceComplete() const { return m_complete; }
    // 主动中止（close/停止播放）：read 返回 AVERROR_EXIT
    void abort();
    bool aborted() const { return m_abort; }

    // 供 UI 显示缓冲进度：等价于 readableBytes()，不是磁盘上的文件长度。
    qint64 availableBytes();
    // 对 demuxer 暴露的可读边界 = min(磁盘长度, 已确认写完的字节数)。
    // readPacket / sizeHint / availableBytes 都只认这个值。
    qint64 readableBytes();
    // 对 demuxer 暴露的文件长度（AVSEEK_SIZE / SEEK_END 用）
    qint64 sizeHint();
    // 下载线程每写完一块调用：把「可安全读取的边界」推进到 bytes（只增不减）。
    // 不给 demuxer 读越过这个边界，是因为写盘时文件长度会先于数据变长
    // （Windows 上 QFile::write 先扩 EOF 再落盘），读到零会被当成 NAL 解析。
    void commitReadable(qint64 bytes);
    qint64 committedBytes() const { return m_committedBytes.load(); }
    qint64 position() const { return m_pos; }

    // 读等待回调：true=开始等数据（缓冲中），false=数据到了
    void setStallCallback(const std::function<void(bool)>& cb);

private:
    static int readPacket(void* opaque, std::uint8_t* buf, int size);
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

    bool openFile();
    // 调用方必须已持有 m_mutex
    qint64 readableLocked();
    void setStalled(bool stalled);

    QString m_path;
    bool m_progressive;
    qint64 m_totalBytes;  // 远端真实总大小；0=未知
    std::atomic<qint64> m_committedBytes;  // 已完整写入、可安全读取的边界
    int m_seekLogs;                        // 诊断用：只打印前若干次 seek
    QFile* m_file;
    AVIOContext* m_avio;
    std::uint8_t* m_buffer;
    qint64 m_pos;

    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_complete;
    bool m_abort;
    bool m_stalled;
    std::function<void(bool)> m_stallCb;
};

}  // namespace player
}  // namespace cm
