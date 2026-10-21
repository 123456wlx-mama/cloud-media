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

    m_emptyHint = new QLabel(
        QString::fromUtf8("未选择文件\n在左侧双击视频即可起播"), this);
    m_emptyHint->setObjectName(QString::fromUtf8("emptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_emptyHint, 1);

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
    stop();
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
    stop();
}

void PlayerPane::playLocalFile(const QString& path) {
    stop();
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
    stop();
    m_displayName = name;
    m_taskId = m_queue->enqueuePlaybackDownload(ufId, name, size);
    m_opened = false;
    m_buffering = true;
    m_emptyHint->hide();
    m_bufferTimer->start();
    emit notice(QString::fromUtf8("正在缓冲「%1」").arg(name));
}

void PlayerPane::openPath(const QString& path, bool progressive) {
    m_localPath = path;
    m_opened = true;
    m_player->open(path, progressive);
    m_player->play();
    m_playButton->setEnabled(true);
    m_playButton->setText(QString::fromUtf8("⏸"));
}

void PlayerPane::stop() {
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
    cleanupSource();
    m_opened = false;
    m_localPath.clear();
}

void PlayerPane::cleanupSource() {
    if (m_queue == nullptr || m_taskId == 0) {
        return;
    }
    transfer::TransferTask* task = m_queue->find(m_taskId);
    if (task != nullptr) {
        if (!task->isTerminal()) {
            m_queue->cancel(m_taskId);
            task = m_queue->find(m_taskId);
        }
        if (task != nullptr && task->playback &&
            (task->state == transfer::TaskState::Done ||
             task->state == transfer::TaskState::Canceled)) {
            // 播放结束/取消：清掉临时 .part 与断点状态，避免临时目录堆积
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
    // 首块落盘后再 open：避免 GUI 线程里等数据（下载响应也要走 GUI 事件循环）
    if (!m_opened && task->bytesDone > 0 &&
        task->state == transfer::TaskState::Running) {
        const QString part = task->localPath + QString::fromUtf8(".part");
        openPath(part, true);
    }
    if (task->state == transfer::TaskState::Done) {
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
