#include "ui/MainWindow.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

namespace cm {
namespace ui {
namespace {

// 空态文案集中在契约 §3（未登录/空列表/传输中/暂停/失败/缓冲）。
const char* kFileHint = "还没有文件\n把文件拖进来，或点「上传」";
const char* kTransferHint = "没有传输任务\n上传 / 下载进度会实时显示在这里";
const char* kPlayerHint = "未选择文件\n在左侧点一个视频即可起播";

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_splitter(nullptr) {
    setWindowTitle(QString::fromUtf8("cloud-media"));
    resize(1280, 800);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QString::fromUtf8("mainSplitter"));
    m_splitter->setHandleWidth(1);
    m_splitter->addWidget(buildFilePane());
    m_splitter->addWidget(buildTransferPane());
    m_splitter->addWidget(buildPlayerPane());
    m_splitter->setStretchFactor(0, 5);
    m_splitter->setStretchFactor(1, 3);
    m_splitter->setStretchFactor(2, 4);
    setCentralWidget(m_splitter);

    statusBar()->setObjectName(QString::fromUtf8("mainStatusBar"));
    statusBar()->showMessage(QString::fromUtf8("未登录 · P8-0 骨架"));
}

QWidget* MainWindow::buildPane(const QString& title, const QString& hint,
                               const QString& objectName) {
    QWidget* pane = new QWidget(this);
    pane->setObjectName(objectName);

    QVBoxLayout* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget* header = new QWidget(pane);
    header->setObjectName(QString::fromUtf8("paneHeader"));
    header->setFixedHeight(36);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    QLabel* titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QString::fromUtf8("paneTitle"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);
    layout->addWidget(header);

    QLabel* empty = new QLabel(hint, pane);
    empty->setObjectName(QString::fromUtf8("emptyHint"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    layout->addWidget(empty, 1);
    return pane;
}

QWidget* MainWindow::buildFilePane() {
    return buildPane(QString::fromUtf8("文件"), QString::fromUtf8(kFileHint),
                     QString::fromUtf8("filePane"));
}

QWidget* MainWindow::buildTransferPane() {
    return buildPane(QString::fromUtf8("传输"), QString::fromUtf8(kTransferHint),
                     QString::fromUtf8("transferPane"));
}

QWidget* MainWindow::buildPlayerPane() {
    return buildPane(QString::fromUtf8("播放"), QString::fromUtf8(kPlayerHint),
                     QString::fromUtf8("playerPane"));
}

}  // namespace ui
}  // namespace cm
