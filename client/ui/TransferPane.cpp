#include "ui/TransferPane.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "transfer/TransferQueue.h"

namespace cm {
namespace ui {
namespace {

QString formatSpeed(double bps) {
    if (bps <= 0.0) {
        return QString::fromUtf8("—");
    }
    if (bps < 1024.0 * 1024.0) {
        return QString::fromUtf8("%1 KB/s").arg(bps / 1024.0, 0, 'f', 0);
    }
    return QString::fromUtf8("%1 MB/s").arg(bps / (1024.0 * 1024.0), 0, 'f', 1);
}

QString formatEta(const transfer::TransferTask& task) {
    if (task.speedBps <= 1.0 || task.size <= task.bytesDone) {
        return QString::fromUtf8("—");
    }
    const double seconds =
        static_cast<double>(task.size - task.bytesDone) / task.speedBps;
    if (seconds > 3600.0) {
        return QString::fromUtf8(">1h");
    }
    return QString::fromUtf8("%1s").arg(static_cast<int>(seconds + 0.5));
}

}  // namespace

TransferPane::TransferPane(transfer::TransferQueue* queue, QWidget* parent)
    : QWidget(parent), m_queue(queue), m_refreshTimer(nullptr), m_tabs(nullptr),
      m_uploadList(nullptr), m_downloadList(nullptr), m_linkHint(nullptr) {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget* header = new QWidget(this);
    header->setObjectName(QString::fromUtf8("paneHeader"));
    header->setFixedHeight(36);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    QLabel* title = new QLabel(QString::fromUtf8("传输"), header);
    title->setObjectName(QString::fromUtf8("paneTitle"));
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    m_linkHint = new QLabel(header);
    m_linkHint->setObjectName(QString::fromUtf8("hintText"));
    headerLayout->addWidget(m_linkHint);
    layout->addWidget(header);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QString::fromUtf8("transferTabs"));
    m_tabs->addTab(buildPage(true), QString::fromUtf8("上传"));
    m_tabs->addTab(buildPage(false), QString::fromUtf8("下载"));
    layout->addWidget(m_tabs, 1);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(150);
    connect(m_refreshTimer, &QTimer::timeout, this, &TransferPane::refresh);

    if (m_queue != nullptr) {
        connect(m_queue, SIGNAL(changed()), this, SLOT(onQueueChanged()));
    }
    refresh();
}

void TransferPane::onQueueChanged() {
    if (m_refreshTimer != nullptr) {
        // 合并刷新：150ms 内的多次 changed 只重建一次
        m_refreshTimer->start();
        return;
    }
    refresh();
}

QWidget* TransferPane::buildPage(bool upload) {
    QListWidget* list = new QListWidget(this);
    list->setObjectName(upload ? QString::fromUtf8("uploadList")
                               : QString::fromUtf8("downloadList"));
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    list->setSpacing(0);
    if (upload) {
        m_uploadList = list;
    } else {
        m_downloadList = list;
    }
    return list;
}

QWidget* TransferPane::buildRow(const transfer::TransferTask& task, bool upload) {
    Q_UNUSED(upload);
    QWidget* row = new QWidget(m_tabs);
    row->setObjectName(QString::fromUtf8("transferRow"));
    QVBoxLayout* layout = new QVBoxLayout(row);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    QHBoxLayout* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    QLabel* name = new QLabel(task.remoteName, row);
    name->setObjectName(QString::fromUtf8("taskName"));
    top->addWidget(name, 1);
    // 契约 §3：秒传命中 → 绿色勾 + "已秒传 · 0 字节"，不显示进度条
    const bool instant = task.instant && task.state == transfer::TaskState::Done;
    // 边下边播的缓存命中：状态写「已缓存」比「已完成」更准确（这次没有真实下载）
    const bool cached =
        task.playback && task.state == transfer::TaskState::Done && !task.instant;
    QString stateText = task.stateText();
    if (instant) {
        stateText = QString::fromUtf8("✓ 已秒传 · 0 字节");
    } else if (cached) {
        stateText = QString::fromUtf8("✓ 已缓存 · 秒开");
    } else if (task.playback && (task.isActive() || task.state == transfer::TaskState::Paused)) {
        // 边下边播：带上百分比，和普通下载区分开（正在缓冲 / 缓冲暂停）
        stateText = QString::fromUtf8("%1 %2%")
                        .arg(task.state == transfer::TaskState::Paused
                                 ? QString::fromUtf8("缓冲暂停")
                                 : QString::fromUtf8("缓冲中"))
                        .arg(static_cast<int>(task.progress() * 100.0));
    }
    QLabel* state = new QLabel(stateText, row);
    state->setObjectName((instant || cached) ? QString::fromUtf8("taskStateInstant")
                                             : QString::fromUtf8("taskState"));
    top->addWidget(state);
    QLabel* speed = new QLabel(
        task.isActive() ? formatSpeed(task.speedBps) : QString::fromUtf8("—"), row);
    speed->setObjectName(QString::fromUtf8("hintText"));
    speed->setMinimumWidth(80);
    speed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(speed);
    QLabel* eta = new QLabel(task.isActive() ? formatEta(task) : QString(), row);
    eta->setObjectName(QString::fromUtf8("hintText"));
    eta->setMinimumWidth(50);
    eta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(eta);
    layout->addLayout(top);

    QHBoxLayout* bottom = new QHBoxLayout();
    bottom->setContentsMargins(0, 0, 0, 0);
    QProgressBar* bar = new QProgressBar(row);
    bar->setRange(0, 1000);
    bar->setValue(static_cast<int>(task.progress() * 1000.0));
    bar->setTextVisible(false);
    bar->setVisible(!instant && !cached);
    bottom->addWidget(bar, 1);

    if (task.state == transfer::TaskState::Failed) {
        QLabel* err = new QLabel(task.errMsg, row);
        err->setObjectName(QString::fromUtf8("taskError"));
        bottom->addWidget(err);
    }

    const int id = task.id;
    if (task.isActive()) {
        QPushButton* pause = new QPushButton(QString::fromUtf8("暂停"), row);
        pause->setObjectName(QString::fromUtf8("rowButton"));
        connect(pause, &QPushButton::clicked, this, [this, id]() {
            if (m_queue != nullptr) {
                m_queue->pause(id);
            }
        });
        bottom->addWidget(pause);
    } else if (task.state == transfer::TaskState::Paused ||
               task.state == transfer::TaskState::Failed) {
        QPushButton* resume = new QPushButton(
            task.state == transfer::TaskState::Failed ? QString::fromUtf8("重试")
                                                      : QString::fromUtf8("继续"),
            row);
        resume->setObjectName(QString::fromUtf8("rowButton"));
        connect(resume, &QPushButton::clicked, this, [this, id]() {
            if (m_queue != nullptr) {
                m_queue->resume(id);
            }
        });
        bottom->addWidget(resume);
    }
    if (!task.isTerminal()) {
        QPushButton* cancel = new QPushButton(QString::fromUtf8("取消"), row);
        cancel->setObjectName(QString::fromUtf8("rowButton"));
        connect(cancel, &QPushButton::clicked, this, [this, id]() {
            if (m_queue != nullptr) {
                m_queue->cancel(id);
            }
        });
        bottom->addWidget(cancel);
    }
    layout->addLayout(bottom);
    return row;
}

void TransferPane::refresh() {
    if (m_queue == nullptr || m_uploadList == nullptr || m_downloadList == nullptr) {
        return;
    }
    m_uploadList->clear();
    m_downloadList->clear();
    const QVector<transfer::TransferTask>& tasks = m_queue->tasks();
    for (int i = 0; i < tasks.size(); ++i) {
        const transfer::TransferTask& task = tasks.at(i);
        const bool upload = task.type == transfer::TaskType::Upload;
        QListWidget* list = upload ? m_uploadList : m_downloadList;
        QListWidgetItem* item = new QListWidgetItem(list);
        item->setSizeHint(QSize(0, 64));
        list->setItemWidget(item, buildRow(task, upload));
    }
    m_tabs->setTabText(0, QString::fromUtf8("上传 %1").arg(m_uploadList->count()));
    m_tabs->setTabText(1, QString::fromUtf8("下载 %1").arg(m_downloadList->count()));
}

void TransferPane::setLinkText(const QString& text) {
    if (m_linkHint != nullptr) {
        m_linkHint->setText(text);
    }
}

}  // namespace ui
}  // namespace cm
