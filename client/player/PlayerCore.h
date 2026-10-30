#pragma once

#include <QImage>
#include <QObject>
#include <QString>

namespace cm {
namespace player {

// 播放器内核：唯一允许 include libav*/SDL 的层（ui/ 只能看到这个头文件）。
// 解封装/解码/音视频同步在内部线程，帧用 QImage 抛给界面。
class PlayerCore : public QObject {
    Q_OBJECT

public:
    enum State {
        Idle = 0,
        Opening,
        Buffering,
        Playing,
        Paused,
        Ended,
        Error
    };

    explicit PlayerCore(QObject* parent = nullptr);
    ~PlayerCore() override;

    // progressive=true：边下边播，读到文件末尾会等新块（直到 setSourceComplete(true)）。
    // totalBytes>0：远端文件真实总大小，作为可读字节的硬上限（防止读到比文件更长的垃圾）。
    void open(const QString& path, bool progressive, qint64 totalBytes = 0);
    void close();

    // 边下边播：把「从文件开头起、连续且已写完」的字节数告诉播放器。
    // 必须在 open 之后持续调用（每写完一块调一次）：播放器只读到这里，
    // 否则会读到「文件已扩容、数据还没落盘」的零区（花屏/无画面）。
    void setReadableBytes(qint64 bytes);

    void play();
    void pause();
    void togglePlay();
    void seek(qint64 ms);
    void setVolume(int percent);
    // 下载完成（或已确定不会再有新数据）：让 demuxer 在文件末尾正常结束
    void setSourceComplete(bool complete);

    // 渲染画布的实际像素尺寸（设备像素 = 控件尺寸 × devicePixelRatio）。
    // 解码线程按它缩放输出（只缩不放，另有像素上限），避免「大源 + 小画布」时
    // 每帧生成十几 MB 的 ARGB 帧。传 0 表示按源尺寸输出。
    void setDisplaySize(int pixelWidth, int pixelHeight);
    // 界面消费掉一帧后必须回执：解码线程据此做背压（上一帧没消费完就丢掉新帧）。
    // 不回执的话 GUI 一慢，事件队列里会堆几十份大图，32 位进程直接被吃穿。
    void notifyFrameConsumed();

    State state() const;
    qint64 position() const;
    qint64 duration() const;
    int videoWidth() const;
    int videoHeight() const;
    bool isOpen() const;
    bool isPaused() const;
    QString path() const;
    QString errorText() const;
    qint64 availableBytes() const;
    // 诊断：当前音频时钟 / 累计丢帧数 / 是否有音频轨（回归日志用）
    qint64 audioClockMs() const;
    int droppedFrames() const;
    bool hasAudio() const;

signals:
    void opened(qint64 durationMs, int width, int height);
    void positionChanged(qint64 ms);
    void stateChanged(int state);
    void bufferingChanged(bool buffering);
    void frameReady(const QImage& image);
    void ended();
    void errorChanged(const QString& text);
    // 每渲染一帧发一次：视频 pts 与当前主时钟（音频或系统），用于同步漂移取证
    void frameSync(qint64 videoPtsMs, qint64 clockMs);

private:
    struct Impl;
    Impl* m_impl;
};

}  // namespace player
}  // namespace cm
