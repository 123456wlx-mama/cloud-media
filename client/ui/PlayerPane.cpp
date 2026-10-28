#include "ui/PlayerPane.h"

#include <cstdio>

#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "player/PlayerCore.h"
#include "transfer/TransferQueue.h"
#include "ui/EmptyState.h"
#include "ui/VideoSurface.h"

namespace cm {
namespace ui {
namespace {

const int kBufferShowDelayMs = 1500;  // 契约 §3：缓冲 < 2s 不显示 loading

QString formatMs(qint64 ms) {
    if (ms < 0) {
        ms = 0;
    }
    const qint64 total = ms / 1000;
    const qint64 minute = total / 60;
    const qint64 second = total % 60;
    return QString::fromUtf8("%1:%2")
        .arg(minute, 2, 10, QChar('0'))
        .arg(second, 2, 10, QChar('0'));
}

const char* kSpinFrames[] = {"◐", "◓", "◑", "◒"};

// 可点击跳转的进度条：Qt 默认点滑槽只翻一页，且不触发 sliderPressed/Released，
// 业务层的 seek 收不到事件（表现为"随机点一个位置播不过去"）。这里按点击位置直接定位。
class ClickableSlider : public QSlider {
public:
    explicit ClickableSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            const int span = maximum() - minimum();
            const int extent = (orientation() == Qt::Horizontal) ? width() : height();
            const int pos = (orientation() == Qt::Horizontal) ? event->pos().x()
                                                              : event->pos().y();
            if (extent > 0) {
                setValue(minimum() + span * pos / extent);
                emit sliderPressed();
                emit sliderMoved(value());
                emit sliderReleased();
                return;
            }
        }
        QSlider::mousePressEvent(event);
    }
};

}  // namespace

PlayerPane::PlayerPane(transfer::TransferQueue* queue, QWidget* parent,
                       bool forcePainter)
    : QWidget(parent), m_queue(queue), m_player(nullptr), m_surface(nullptr),
      m_playButton(nullptr), m_slider(nullptr), m_timeLabel(nullptr),
      m_volume(nullptr), m_fullButton(nullptr), m_emptyHint(nullptr),
      m_overlay(nullptr), m_overlayLabel(nullptr), m_errorPanel(nullptr),
      m_errorLabel(nullptr), m_fallbackButton(nullptr), m_bufferTimer(nullptr),
      m_spinTimer(nullptr), m_spinFrame(0), m_taskId(0), m_opened(false),
      m_sliderHeld(false), m_buffering(false), m_duration(0) {
    setObjectName(QString::fromUtf8("playerPane"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget* header = new QWidget(this);
    header->setObjectName(QString::fromUtf8("paneHeader"));
    header->setFixedHeight(36);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    QLabel* title = new QLabel(QString::fromUtf8("播放"), header);
    title->setObjectName(QString::fromUtf8("paneTitle"));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    m_displayName.clear();
    layout->addWidget(header);

    m_player = new player::PlayerCore(this);
    m_surface = createVideoSurface(this, forcePainter);
    qInfo("player surface: %s", qPrintable(m_surface->backendName()));
    layout->addWidget(m_surface->widget(), 1);

    // 空态做成画布上方的浮层（不进布局）：否则它和渲染控件各占 1 份 stretch，
    // 播放时视频只拿到半屏、缩在一角。
    m_emptyHint = new EmptyState(
        QString::fromUtf8("暂无播放内容"),
        QString::fromUtf8("在左侧双击音视频文件即可开始播放"), this);
    m_emptyHint->setObjectName(QString::fromUtf8("emptyHint"));

    // 控制条
    QWidget* controls = new QWidget(this);
    controls->setObjectName(QString::fromUtf8("playerControls"));
    controls->setFixedHeight(48);
    QHBoxLayout* row = new QHBoxLayout(controls);
    row->setContentsMargins(12, 6, 12, 6);
    row->setSpacing(10);

    m_playButton = new QPushButton(QString::fromUtf8("▶"), controls);
    m_playButton->setObjectName(QString::fromUtf8("playButton"));
    m_playButton->setFixedWidth(40);
    m_playButton->setEnabled(false);
    row->addWidget(m_playButton);

    m_timeLabel = new QLabel(QString::fromUtf8("00:00 / 00:00"), controls);
    m_timeLabel->setObjectName(QString::fromUtf8("hintText"));
    m_timeLabel->setMinimumWidth(110);
    row->addWidget(m_timeLabel);

    m_slider = new ClickableSlider(Qt::Horizontal, controls);
    m_slider->setObjectName(QString::fromUtf8("seekSlider"));
    m_slider->setRange(0, 1000);
    m_slider->setEnabled(false);
    row->addWidget(m_slider, 1);

    QLabel* volumeIcon = new QLabel(QString::fromUtf8("音量"), controls);
    volumeIcon->setObjectName(QString::fromUtf8("hintText"));
    row->addWidget(volumeIcon);
    m_volume = new QSlider(Qt::Horizontal, controls);
    m_volume->setObjectName(QString::fromUtf8("volumeSlider"));
    m_volume->setRange(0, 100);
    m_volume->setValue(80);
    m_volume->setFixedWidth(90);
    row->addWidget(m_volume);

    m_fullButton = new QPushButton(QString::fromUtf8("全屏"), controls);
    m_fullButton->setObjectName(QString::fromUtf8("rowButton"));
    row->addWidget(m_fullButton);
    layout->addWidget(controls);

    // 缓冲浮层
    m_overlay = new QWidget(this);
    m_overlay->setObjectName(QString::fromUtf8("playerOverlay"));
    QVBoxLayout* overlayLayout = new QVBoxLayout(m_overlay);
    overlayLayout->setContentsMargins(16, 10, 16, 10);
    m_overlayLabel = new QLabel(QString::fromUtf8("◐ 缓冲中…"), m_overlay);
    m_overlayLabel->setObjectName(QString::fromUtf8("bufferLabel"));
    m_overlayLabel->setAlignment(Qt::AlignCenter);
    overlayLayout->addWidget(m_overlayLabel);
    m_overlay->hide();

    // 失败浮层：明确降级出口（契约 §3）
    m_errorPanel = new QWidget(this);
    m_errorPanel->setObjectName(QString::fromUtf8("playerErrorPanel"));
    QVBoxLayout* errorLayout = new QVBoxLayout(m_errorPanel);
    errorLayout->setContentsMargins(20, 16, 20, 16);
    errorLayout->setSpacing(10);
    m_errorLabel = new QLabel(QString::fromUtf8("无法播放该文件"), m_errorPanel);
    m_errorLabel->setObjectName(QString::fromUtf8("taskError"));
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    errorLayout->addWidget(m_errorLabel);
    m_fallbackButton = new QPushButton(QString::fromUtf8("用系统播放器打开"),
                                       m_errorPanel);
    m_fallbackButton->setObjectName(QString::fromUtf8("primaryButton"));
    errorLayout->addWidget(m_fallbackButton, 0, Qt::AlignCenter);
    m_errorPanel->hide();

    m_bufferTimer = new QTimer(this);
    m_bufferTimer->setSingleShot(true);
    m_bufferTimer->setInterval(kBufferShowDelayMs);
    m_spinTimer = new QTimer(this);
    m_spinTimer->setInterval(120);

    connect(m_playButton, &QPushButton::clicked, this, &PlayerPane::onTogglePlay);
    connect(m_slider, &QSlider::sliderPressed, this, &PlayerPane::onSliderPressed);
    connect(m_slider, &QSlider::sliderReleased, this, &PlayerPane::onSliderReleased);
    connect(m_volume, &QSlider::valueChanged, this, &PlayerPane::onVolumeChanged);
    connect(m_fullButton, &QPushButton::clicked, this, &PlayerPane::toggleFullscreen);
    connect(m_bufferTimer, &QTimer::timeout, this, &PlayerPane::onBufferTick);
    connect(m_spinTimer, &QTimer::timeout, this, [this]() {
        m_spinFrame = (m_spinFrame + 1) % 4;
        m_overlayLabel->setText(
            QString::fromUtf8("%1 缓冲中…").arg(QString::fromUtf8(kSpinFrames[m_spinFrame])));
    });
    connect(m_fallbackButton, &QPushButton::clicked, this, [this]() {
        if (m_localPath.isEmpty()) {
            return;
        }
        QString path = m_localPath;
        if (path.endsWith(QString::fromUtf8(".part")) && m_queue != nullptr &&
            m_taskId != 0) {
            const transfer::TransferTask* task = m_queue->find(m_taskId);
            if (task != nullptr && task->state == transfer::TaskState::Done) {
                path = task->localPath;
            }
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    connect(m_player, &player::PlayerCore::frameReady, this,
            [this](const QImage& image) {
                m_surface->setImage(image);
                m_emptyHint->hide();
            });
    connect(m_player, &player::PlayerCore::opened, this,
            [this](qint64 durationMs, int, int) {
                m_duration = durationMs;
                m_slider->setEnabled(durationMs > 0);
                updateTimeLabel();
            });
    connect(m_player, &player::PlayerCore::positionChanged, this,
            &PlayerPane::onPosition);
    connect(m_player, &player::PlayerCore::stateChanged, this,
            &PlayerPane::onStateChanged);
    connect(m_player, &player::PlayerCore::bufferingChanged, this,
            &PlayerPane::onBufferingChanged);
    connect(m_player, &player::PlayerCore::errorChanged, this,
            &PlayerPane::onPlayerError);
    if (m_queue != nullptr) {
        connect(m_queue, &transfer::TransferQueue::changed, this,
                &PlayerPane::onQueueChanged);
    }

    m_player->setVolume(m_volume->value());
}

void PlayerPane::shutdown() {
    if (m_queue != nullptr && m_taskId != 0) {
        transfer::TransferTask* task = m_queue->find(m_taskId);
        if (task != nullptr && !task->isTerminal()) {
            m_queue->cancel(m_taskId);  // 此刻兄弟控件还活着，changed 是安全的
        }
    }
    stop(false);  // 程序退出才真正清缓存
    if (m_queue != nullptr) {
        disconnect(m_queue, nullptr, this, nullptr);
    }
    blockSignals(true);
    m_taskId = 0;
}

PlayerPane::~PlayerPane() {
    // 控件析构阶段绝不能再碰 TransferQueue：emit changed 会进到正在销毁的
    // TransferPane，Qt 直接崩在 reparentFocusWidgets。
    if (m_queue != nullptr) {
        disconnect(m_queue, nullptr, this, nullptr);
    }
    blockSignals(true);
    m_taskId = 0;  // 让 stop() 里的 cleanupSource 直接返回
    stop(false);
}

void PlayerPane::playLocalFile(const QString& path) {
    stop(true);
    m_taskId = 0;
    m_displayName = QFileInfo(path).fileName();
    m_buffering = true;
    m_bufferTimer->start();
    m_emptyHint->hide();
    openPath(path, false);
}

void PlayerPane::playRemoteFile(quint64 ufId, const QString& name, quint64 size) {
    if (m_queue == nullptr) {
        return;
    }
    stop(true);  // 切走保留缓存：切回来走断点续传，不重新下载
    m_displayName = name;
    m_taskId = m_queue->enqueuePlaybackDownload(ufId, name, size);
    m_opened = false;
    m_buffering = true;
    m_emptyHint->hide();
    m_bufferTimer->start();
    emit notice(QString::fromUtf8("正在缓冲「%1」").arg(name));
}

void PlayerPane::openPath(const QString& path, bool progressive,
                          qint64 totalBytes) {
    m_localPath = path;
    m_player->open(path, progressive, totalBytes);
    if (!m_player->isOpen()) {
        // 数据还不够（典型：moov 在尾部、刚下到第一块）：保持缓冲态，
        // 等下一块落盘后 onQueueChanged 会再次尝试，而不是直接判死。
        m_opened = false;
        return;
    }
    m_opened = true;
    hideOverlays();
    m_player->play();
    m_playButton->setEnabled(true);
    m_playButton->setText(QString::fromUtf8("⏸"));
}

void PlayerPane::stop(bool keepCache) {
    m_player->close();
    m_bufferTimer->stop();
    m_spinTimer->stop();
    m_buffering = false;
    hideOverlays();
    m_surface->clear();
    m_slider->setValue(0);
    m_slider->setEnabled(false);
    m_playButton->setEnabled(false);
    m_playButton->setText(QString::fromUtf8("▶"));
    m_duration = 0;
    m_timeLabel->setText(QString::fromUtf8("00:00 / 00:00"));
    cleanupSource(keepCache);
    m_opened = false;
    m_localPath.clear();
}

void PlayerPane::cleanupSource(bool keepCache) {
    if (m_queue == nullptr || m_taskId == 0) {
        return;
    }
    transfer::TransferTask* task = m_queue->find(m_taskId);
    if (task != nullptr) {
        if (!task->isTerminal()) {
            m_queue->cancel(m_taskId);
            task = m_queue->find(m_taskId);
        }
        // keepCache：切换影片时保留 .part / .part.state。
        // 不保留的话，切走再切回来必须从第 0 块重新下载（几 MB 起步），
        // 表现就是「切回来很卡」；保留后走断点续传，已下完的直接秒开。
        if (!keepCache && task != nullptr && task->playback &&
            (task->state == transfer::TaskState::Done ||
             task->state == transfer::TaskState::Canceled)) {
            QFile::remove(task->localPath + QString::fromUtf8(".part"));
            QFile::remove(task->localPath + QString::fromUtf8(".part.state"));
        }
    }
    m_taskId = 0;
}

void PlayerPane::onTogglePlay() {
    if (!m_player->isOpen()) {
        return;
    }
    m_player->togglePlay();
    m_playButton->setText(m_player->isPaused() ? QString::fromUtf8("▶")
                                               : QString::fromUtf8("⏸"));
}

void PlayerPane::onSliderPressed() { m_sliderHeld = true; }

void PlayerPane::onSliderReleased() {
    m_sliderHeld = false;
    if (!m_player->isOpen() || m_duration <= 0) {
        return;
    }
    const qint64 target = m_duration * m_slider->value() / 1000;
    m_player->seek(target);
}

void PlayerPane::onPosition(qint64 ms) {
    // close() 之后事件队列里可能还压着上一部片子的 positionChanged，
    // 直接拿来更新会让新影片一打开就显示旧影片的秒数。
    if (!m_player->isOpen()) {
        return;
    }
    if (!m_sliderHeld && m_duration > 0) {
        m_slider->setValue(static_cast<int>(ms * 1000 / m_duration));
    }
    m_timeLabel->setText(
        QString::fromUtf8("%1 / %2").arg(formatMs(ms), formatMs(m_duration)));
}

void PlayerPane::onStateChanged(int state) {
    const player::PlayerCore::State s = static_cast<player::PlayerCore::State>(state);
    if (s == player::PlayerCore::Playing) {
        m_playButton->setText(QString::fromUtf8("⏸"));
    } else if (s == player::PlayerCore::Paused) {
        m_playButton->setText(QString::fromUtf8("▶"));
    } else if (s == player::PlayerCore::Ended) {
        m_playButton->setText(QString::fromUtf8("▶"));
        hideOverlays();
    } else if (s == player::PlayerCore::Idle) {
        hideOverlays();
    }
}

void PlayerPane::onBufferingChanged(bool buffering) {
    m_buffering = buffering;
    if (buffering) {
        // 只有持续 >1.5s 才显示，避免每块数据之间闪一下
        if (!m_overlay->isVisible()) {
            m_bufferTimer->start();
        }
    } else {
        m_bufferTimer->stop();
        if (m_overlay->isVisible()) {
            m_overlay->hide();
            m_spinTimer->stop();
        }
    }
}

void PlayerPane::onBufferTick() {
    if (!m_buffering) {
        return;
    }
    m_overlay->show();
    m_overlay->raise();
    layoutOverlays();
    m_spinTimer->start();
}

void PlayerPane::onPlayerError(const QString& text) {
    showError(text);
}

void PlayerPane::showError(const QString& text) {
    m_bufferTimer->stop();
    m_spinTimer->stop();
    m_overlay->hide();
    m_errorLabel->setText(
        QString::fromUtf8("无法播放该文件\n%1").arg(text));
    m_errorPanel->show();
    m_errorPanel->raise();
    layoutOverlays();
    m_playButton->setEnabled(false);
    emit notice(QString::fromUtf8("播放失败：%1").arg(text));
}

void PlayerPane::hideOverlays() {
    if (m_overlay != nullptr) {
        m_overlay->hide();
    }
    if (m_errorPanel != nullptr) {
        m_errorPanel->hide();
    }
    if (m_spinTimer != nullptr) {
        m_spinTimer->stop();
    }
}

void PlayerPane::onQueueChanged() {
    if (m_queue == nullptr || m_taskId == 0) {
        return;
    }
    const transfer::TransferTask* task = m_queue->find(m_taskId);
    if (task == nullptr) {
        return;
    }
    // 先告诉播放器可安全读到哪：写盘时文件长度会先于数据变长（Windows 先扩 EOF
    // 再落盘），那段是零，demuxer 读进去会当成 NAL 解析 → 花屏/无画面。
    // 只用「从 0 起连续写完」的边界，不能用 bytesDone（续传时可能虚高）。
    m_player->setReadableBytes(static_cast<qint64>(task->readableBytes));
    // 首块落盘后再 open：避免 GUI 线程里等数据（下载响应也要走 GUI 事件循环）
    // Done 也要 open：切回来的影片若缓存已完整，直接按本地文件播（秒开，不再等下载）。
    // 失败/取消也要 open：已经下到的那部分仍然能看，不能让用户对着空白等。
    const bool running = (task->state == transfer::TaskState::Running);
    const bool done = (task->state == transfer::TaskState::Done);
    const bool stopped = (task->state == transfer::TaskState::Failed ||
                          task->state == transfer::TaskState::Canceled);
    if (!m_opened && task->readableBytes > 0 && (running || done || stopped)) {
        const QString part = task->localPath + QString::fromUtf8(".part");
        // 必须带上远端总大小：可读字节的硬上限，也是 demuxer 定位尾部索引的依据。
        // 已全部下完时按本地完整文件播（progressive=false），不需要再等新块。
        openPath(part, !done, static_cast<qint64>(task->size));
    }
    if (task->state == transfer::TaskState::Done) {
        // 全部块已落盘：把边界推到文件末尾，最后一帧/最后一段才读得到
        m_player->setReadableBytes(static_cast<qint64>(task->size));
        m_player->setSourceComplete(true);
    } else if (task->state == transfer::TaskState::Failed ||
               task->state == transfer::TaskState::Canceled) {
        m_player->setSourceComplete(true);
        if (m_opened) {
            emit notice(QString::fromUtf8("「%1」下载中断，只能播放已下载部分")
                            .arg(task->remoteName));
        }
    }
}

void PlayerPane::onVolumeChanged(int value) {
    m_player->setVolume(value);
}

void PlayerPane::toggleFullscreen() {
    QWidget* top = window();
    if (top->isFullScreen()) {
        top->showNormal();
    } else {
        top->showFullScreen();
    }
}

void PlayerPane::updateTimeLabel() {
    m_timeLabel->setText(QString::fromUtf8("%1 / %2")
                             .arg(formatMs(m_player->position()),
                                  formatMs(m_duration)));
}

void PlayerPane::layoutOverlays() {
    if (m_surface == nullptr) {
        return;
    }
    const QRect area = m_surface->widget()->geometry();
    m_emptyHint->setGeometry(area);
    const int overlayWidth = 180;
    const int overlayHeight = 56;
    m_overlay->setGeometry(area.center().x() - overlayWidth / 2,
                           area.center().y() - overlayHeight / 2, overlayWidth,
                           overlayHeight);
    m_errorPanel->setGeometry(area.center().x() - 180, area.center().y() - 70,
                              360, 140);
}

void PlayerPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutOverlays();
}

}  // namespace ui
}  // namespace cm
