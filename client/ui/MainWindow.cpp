#include "ui/MainWindow.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "netapi/NetClient.h"
#include "ui/FileTableModel.h"

namespace cm {
namespace ui {
namespace {

const char* kTransferHint = "没有传输任务\n上传 / 下载进度会实时显示在这里";
const char* kPlayerHint = "未选择文件\n在左侧点一个视频即可起播";

}  // namespace

MainWindow::MainWindow(netapi::NetClient* net, QWidget* parent)
    : QMainWindow(parent), m_net(net), m_splitter(nullptr), m_model(nullptr),
      m_table(nullptr), m_fileEmpty(nullptr), m_linkBadge(nullptr),
      m_listHint(nullptr) {
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
    m_linkBadge = new QLabel(QString::fromUtf8("● 连接中…"), this);
    m_linkBadge->setObjectName(QString::fromUtf8("linkBadge"));
    statusBar()->addPermanentWidget(m_linkBadge);
    statusBar()->showMessage(QString::fromUtf8("未登录"));

    if (m_net != nullptr) {
        connect(m_net, SIGNAL(linkState(int, QString)), this,
                SLOT(onLinkState(int, QString)));
        connect(m_net, SIGNAL(loginFinished(quint32, quint64, QString)), this,
                SLOT(onLoginSucceeded(quint32, quint64, QString)));
        connect(m_net, SIGNAL(listFinished(quint32, QVariantList)), this,
                SLOT(onListFinished(quint32, QVariantList)));
        connect(m_net, SIGNAL(requestFailed(quint16, QString)), this,
                SLOT(onRequestFailed(quint16, QString)));
        if (m_net->uid() != 0) {
            refreshList();
        }
    }
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
    QWidget* pane = new QWidget(this);
    pane->setObjectName(QString::fromUtf8("filePane"));

    QVBoxLayout* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget* header = new QWidget(pane);
    header->setObjectName(QString::fromUtf8("paneHeader"));
    header->setFixedHeight(36);
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    QLabel* title = new QLabel(QString::fromUtf8("文件"), header);
    title->setObjectName(QString::fromUtf8("paneTitle"));
    headerLayout->addWidget(title);
    m_listHint = new QLabel(header);
    m_listHint->setObjectName(QString::fromUtf8("hintText"));
    headerLayout->addWidget(m_listHint);
    headerLayout->addStretch(1);
    layout->addWidget(header);

    m_model = new FileTableModel(this);
    m_table = new QTableView(pane);
    m_table->setObjectName(QString::fromUtf8("fileTable"));
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->setColumnWidth(FileTableModel::ColName, 320);
    m_table->setColumnWidth(FileTableModel::ColSize, 100);
    m_table->setColumnWidth(FileTableModel::ColKind, 80);
    m_table->setVisible(false);
    layout->addWidget(m_table, 1);

    m_fileEmpty = new QLabel(
        QString::fromUtf8("还没有文件\n把文件拖进来，或点「上传」"), pane);
    m_fileEmpty->setObjectName(QString::fromUtf8("emptyHint"));
    m_fileEmpty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_fileEmpty, 1);
    return pane;
}

QWidget* MainWindow::buildTransferPane() {
    return buildPane(QString::fromUtf8("传输"), QString::fromUtf8(kTransferHint),
                     QString::fromUtf8("transferPane"));
}

QWidget* MainWindow::buildPlayerPane() {
    return buildPane(QString::fromUtf8("播放"), QString::fromUtf8(kPlayerHint),
                     QString::fromUtf8("playerPane"));
}

void MainWindow::refreshList() {
    if (m_net == nullptr) {
        return;
    }
    m_listHint->setText(QString::fromUtf8("加载中…"));
    m_net->listFiles(0, 50);
}

void MainWindow::onLoginSucceeded(quint32 code, quint64 uid,
                                 const QString& name) {
    Q_UNUSED(uid);
    if (code != 0) {
        return;
    }
    statusBar()->showMessage(QString::fromUtf8("已登录 · %1").arg(name));
    refreshList();
}

void MainWindow::onLinkState(int state, const QString& detail) {
    setLinkBadge(state, detail);
    if (state == 1 && m_net != nullptr && m_net->uid() != 0) {
        refreshList();  // 重连后自动补一次列表
    }
}

void MainWindow::setLinkBadge(int state, const QString& detail) {
    QString text;
    QString color;
    switch (state) {
        case 1:
            text = QString::fromUtf8("● 已连接");
            color = QString::fromUtf8("#35C46B");
            break;
        case 2:
            text = QString::fromUtf8("● %1").arg(detail);
            color = QString::fromUtf8("#F0A63A");
            break;
        case 3:
            text = QString::fromUtf8("● %1").arg(detail);
            color = QString::fromUtf8("#E5544B");
            break;
        default:
            text = QString::fromUtf8("● %1").arg(detail);
            color = QString::fromUtf8("#F0A63A");
            break;
    }
    m_linkBadge->setText(text);
    m_linkBadge->setStyleSheet(
        QString::fromUtf8("color:%1; font-size:12px; padding-right:8px;").arg(color));
}

void MainWindow::onListFinished(quint32 code, const QVariantList& rows) {
    if (code != 0) {
        m_listHint->setText(QString::fromUtf8("加载失败 · code=%1").arg(code));
        return;
    }
    m_model->setRows(rows);
    m_listHint->setText(QString::fromUtf8("%1 项").arg(rows.size()));
    const bool empty = rows.isEmpty();
    m_table->setVisible(!empty);
    m_fileEmpty->setVisible(empty);
    statusBar()->showMessage(QString::fromUtf8("已登录 · %1").arg(m_net->userName()));
}

void MainWindow::onRequestFailed(quint16 type, const QString& reason) {
    Q_UNUSED(type);
    m_listHint->setText(QString::fromUtf8("加载失败 · %1").arg(reason));
    statusBar()->showMessage(reason);
}

}  // namespace ui
}  // namespace cm
