#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace cm {

namespace player {
class PlayerCore;
}

namespace transfer {
class TransferQueue;
}

namespace ui {

class VideoSurface;

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
    void stop();
    // 主窗口析构前调用：此时兄弟控件都还活着，取消任务/发 changed 是安全的
    void shutdown();
    void toggleFullscreen();

signals:
    void notice(const QString& text);

protected:
    void resizeEvent(QResizeEvent* event) override;

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
    void openPath(const QString& path, bool progressive);
    void showError(const QString& text);
    void hideOverlays();
    void updateTimeLabel();
    void layoutOverlays();
    void cleanupSource();

    transfer::TransferQueue* m_queue;
    player::PlayerCore* m_player;
    VideoSurface* m_surface;
    QPushButton* m_playButton;
    QSlider* m_slider;
    QLabel* m_timeLabel;
    QSlider* m_volume;
    QPushButton* m_fullButton;
    QLabel* m_emptyHint;
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
