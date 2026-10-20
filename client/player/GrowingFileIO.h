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
    GrowingFileIO(const QString& path, bool progressive);
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

    qint64 availableBytes();
    qint64 position() const { return m_pos; }

    // 读等待回调：true=开始等数据（缓冲中），false=数据到了
    void setStallCallback(const std::function<void(bool)>& cb);

private:
    static int readPacket(void* opaque, std::uint8_t* buf, int size);
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

    bool openFile();
    void setStalled(bool stalled);

    QString m_path;
    bool m_progressive;
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
