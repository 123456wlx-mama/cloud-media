#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstddef>

namespace cm {
namespace transfer {

enum class TaskType { Upload = 0, Download = 1 };

// running/paused/retrying/failed/done/canceled + 排队（契约 §3）
enum class TaskState {
    Queued = 0,
    Running,
    Paused,
    Retrying,
    Failed,
    Done,
    Canceled
};

struct TransferTask {
    int id;
    TaskType type;
    QString localPath;
    QString remoteName;
    quint64 size;
    quint64 parentId;  // 上传目标目录（0 = 根）；下载不使用
    TaskState state;
    quint64 bytesDone;
    double speedBps;
    quint32 errCode;
    QString errMsg;
    bool instant;   // 秒传命中
    bool playback;  // 边下边播任务：完成后保留 .part（播放器还在读）

    // upload 运行态
    QString taskId;
    QString fileMd5;
    quint32 chunkSize;
    quint32 totalChunks;
    quint32 nextChunk;

    // download 运行态
    quint64 ufId;
    quint64 fId;
    QString ticket;
    QString shareTicket;  // 非空 = 分享链接下载（DownloadOpenRq 用它替代 ufId）
    quint32 blockSize;
    quint32 blockCount;
    quint32 nextBlock;
    quint32 blocksFetched;   // 本次启动后实际取回的块数（续传取证）
    quint32 writeFailCount;  // 同一块连续写盘失败次数，超过上限才判定任务失败
    // 从文件开头起、「连续且已写完」的字节数（播放器只能读到这里）。
    // 与 bytesDone 的区别：续传时 bitmap 可能中间有洞，bytesDone 会虚高，
    // 而播放器必须看到一条连续的字节流，否则会把空洞当码流解析。
    quint64 readableBytes;

    // 已完成的块（下载）或已确认的分片（上传）；LSB-first，与 t_chunk.bitmap 同构。
    QByteArray bitmap;

    // 速度计算辅助
    quint64 lastBytes;
    qint64 lastTickMs;

    TransferTask()
        : id(0), type(TaskType::Upload), size(0), parentId(0),
          state(TaskState::Queued),
          bytesDone(0), speedBps(0.0), errCode(0), instant(false),
          playback(false), chunkSize(0),
          totalChunks(0), nextChunk(0), ufId(0), fId(0), blockSize(0),
          blockCount(0), nextBlock(0), blocksFetched(0), writeFailCount(0),
          readableBytes(0), lastBytes(0), lastTickMs(0) {}

    bool isTerminal() const {
        return state == TaskState::Done || state == TaskState::Canceled;
    }

    bool isActive() const {
        return state == TaskState::Running || state == TaskState::Retrying;
    }

    double progress() const {
        if (size == 0) {
            return 0.0;
        }
        if (state == TaskState::Done) {
            return 1.0;
        }
        const double ratio = static_cast<double>(bytesDone) / static_cast<double>(size);
        return ratio > 1.0 ? 1.0 : ratio;
    }

    QString stateText() const {
        switch (state) {
            case TaskState::Queued: return QString::fromUtf8("排队中");
            case TaskState::Running: return QString::fromUtf8("传输中");
            case TaskState::Paused: return QString::fromUtf8("已暂停");
            case TaskState::Retrying: return QString::fromUtf8("重试中");
            case TaskState::Failed: return QString::fromUtf8("失败");
            case TaskState::Done:
                return instant ? QString::fromUtf8("已秒传")
                               : QString::fromUtf8("已完成");
            case TaskState::Canceled: return QString::fromUtf8("已取消");
        }
        return QString();
    }
};

// ---- bitmap（LSB-first）----
inline bool bitmapTest(const QByteArray& bitmap, quint32 index) {
    const int byte = static_cast<int>(index / 8);
    if (byte < 0 || byte >= bitmap.size()) {
        return false;
    }
    return (static_cast<unsigned char>(bitmap.at(byte)) &
            static_cast<unsigned char>(1u << (index % 8))) != 0;
}

inline void bitmapSet(QByteArray* bitmap, quint32 index) {
    if (bitmap == nullptr) {
        return;
    }
    const int byte = static_cast<int>(index / 8);
    if (byte < 0 || byte >= bitmap->size()) {
        return;
    }
    (*bitmap)[byte] = static_cast<char>(
        static_cast<unsigned char>(bitmap->at(byte)) |
        static_cast<unsigned char>(1u << (index % 8)));
}

inline void bitmapResize(QByteArray* bitmap, quint32 total) {
    if (bitmap == nullptr) {
        return;
    }
    const int bytes = static_cast<int>((total + 7) / 8);
    if (bitmap->size() < bytes) {
        bitmap->append(QByteArray(bytes - bitmap->size(), '\0'));
    }
}

inline quint32 bitmapCount(const QByteArray& bitmap, quint32 total) {
    quint32 count = 0;
    for (quint32 i = 0; i < total; ++i) {
        if (bitmapTest(bitmap, i)) {
            ++count;
        }
    }
    return count;
}

inline quint32 firstMissing(const QByteArray& bitmap, quint32 total) {
    for (quint32 i = 0; i < total; ++i) {
        if (!bitmapTest(bitmap, i)) {
            return i;
        }
    }
    return total;
}

}  // namespace transfer
}  // namespace cm
