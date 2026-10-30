#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QVBoxLayout;

namespace cm {

namespace player {
class PlayerCore;
}

namespace transfer {
class TransferQueue;
}

namespace ui {

class VideoSurface;
class EmptyState;

// 播放区：渲染画布 + 控制条（播放/暂停、进度、音量、全屏）+ 缓冲/失败态。
// 只调 PlayerCore 的 Qt 接口，不碰 FFmpeg/SDL（契约 §1.2、依赖红线）。
class PlayerPane : public QWidget {
    Q_OBJECT

public:
    // forcePainter=true 时强制走 QPainter 后端（远程桌面/无 GL 环境）
    explicit PlayerPane(transfer::TransferQueue* queue, QWidget* parent = nullptr,
                        bool forcePainter = false);
    ~PlayerPane() override;

public slots:
    // 里程碑 A：本地完整文件
    void playLocalFile(const QString& path);
    // 里程碑 B：边下边播（下载写 .part，播放器读这个增长中的文件）
    void playRemoteFile(quint64 ufId, const QString& name, quint64 size);
    // keepCache=true：切换影片时保留 .part 断点，切回来能续传/秒开；
    // false：真正清理（程序退出时才用）。
    void stop(bool keepCache = false);
    // 主窗口析构前调用：此时兄弟控件都还活着，取消任务/发 changed 是安全的
    void shutdown();
    void toggleFullscreen();

signals:
    void notice(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;
    // 画布尺寸变化 → 通知播放器按显示像素出帧；全屏窗口 → Esc/F11/Alt+F4 退出
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onTogglePlay();
    void onSliderPressed();
    void onSliderReleased();
    void onPosition(qint64 ms);
    void onStateChanged(int state);
    void onBufferingChanged(bool buffering);
    void onPlayerError(const QString& text);
    void onQueueChanged();
    void onVolumeChanged(int value);
    void onBufferTick();

private:
    // totalBytes：远端真实总大小，边下边播必须传（demuxer 要靠它定位尾部索引）
    void openPath(const QString& path, bool progressive, qint64 totalBytes = 0);
    void showError(const QString& text);
    void hideOverlays();
    void updateTimeLabel();
    void layoutOverlays();
    void cleanupSource(bool keepCache);
    // 全屏：只把渲染控件搬进一个独立的全屏窗口。
    // 整个 QMainWindow showFullScreen() 会让 Qt 为三栏界面申请一张全屏尺寸的
    // 光栅后备存储（1464x830 逻辑 @dpr2 → 2928x1660 DIB ≈ 19MB），32 位进程
    // 上 CreateDIBSection 直接失败 → 空后备存储 → 进程崩溃。
    void enterFullscreen();
    void exitFullscreen();
    // 把画布当前尺寸（设备像素）告诉播放器，解码线程据此缩小出帧
    void syncDisplaySize();

    transfer::TransferQueue* m_queue;
    player::PlayerCore* m_player;
    VideoSurface* m_surface;
    QVBoxLayout* m_layout;
    QWidget* m_fullWindow;  // 非空表示正处于全屏（渲染控件此刻是它的子控件）
    QPushButton* m_playButton;
    QSlider* m_slider;
    QLabel* m_timeLabel;
    QSlider* m_volume;
    QPushButton* m_fullButton;
    EmptyState* m_emptyHint;
    QWidget* m_overlay;
    QLabel* m_overlayLabel;
    QWidget* m_errorPanel;
    QLabel* m_errorLabel;
    QPushButton* m_fallbackButton;
    QTimer* m_bufferTimer;
    QTimer* m_spinTimer;
    int m_spinFrame;

    int m_taskId;
    bool m_opened;
    bool m_sliderHeld;
    bool m_buffering;
    qint64 m_duration;
    QString m_localPath;
    QString m_displayName;
};

}  // namespace ui
}  // namespace cm
