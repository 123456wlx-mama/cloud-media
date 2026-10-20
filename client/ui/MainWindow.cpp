#include "ui/MainWindow.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QVBoxLayout>
#include <QWidget>

#include "netapi/NetClient.h"
#include "transfer/TransferQueue.h"
#include "ui/FileTableModel.h"
#include "ui/TransferPane.h"

namespace cm {
namespace ui {
namespace {

const char* kTransferHint = "没有传输任务\n上传 / 下载进度会实时显示在这里";
const char* kPlayerHint = "未选择文件\n在左侧点一个视频即可起播";

}  // namespace

MainWindow::MainWindow(netapi::NetClient* net, transfer::TransferQueue* queue,
                       QWidget* parent)
    : QMainWindow(parent), m_net(net), m_queue(queue), m_transferPane(nullptr),
      m_splitter(nullptr), m_model(nullptr),
      m_table(nullptr), m_fileEmpty(nullptr), m_linkBadge(nullptr),
      m_listHint(nullptr) {
    setWindowTitle(QString::fromUtf8("cloud-media"));
    resize(1280, 800);

    setAcceptDrops(true);  // 拖文件入队上传
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

    QPushButton* upload = new QPushButton(QString::fromUtf8("上传文件"), pane);
    upload->setObjectName(QString::fromUtf8("primaryButton"));
    upload->setMaximumWidth(120);
    QHBoxLayout* emptyRow = new QHBoxLayout();
    emptyRow->addStretch(1);
    emptyRow->addWidget(upload);
    emptyRow->addStretch(1);
    layout->addLayout(emptyRow);
    connect(upload, SIGNAL(clicked()), this, SLOT(onUploadClicked()));

    // 右键下载（契约 §1.2 文件区职责）
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = m_table->indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        QMenu menu(this);
        QAction* download = menu.addAction(QString::fromUtf8("下载"));
        if (menu.exec(m_table->viewport()->mapToGlobal(pos)) == download) {
            onDownloadSelected();
        }
    });
    return pane;
}

QWidget* MainWindow::buildTransferPane() {
    if (m_queue == nullptr) {
        return buildPane(QString::fromUtf8("传输"), QString::fromUtf8(kTransferHint),
                         QString::fromUtf8("transferPane"));
    }
    m_transferPane = new TransferPane(m_queue, this);
    m_transferPane->setObjectName(QString::fromUtf8("transferPane"));
    return m_transferPane;
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

void MainWindow::onDownloadSelected() {
    if (m_queue == nullptr || m_table == nullptr) {
        return;
    }
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    const int row = rows.first().row();
    const quint64 ufId = m_model->ufIdAt(row);
    if (ufId == 0) {
        return;
    }
    const QString name = m_model->index(row, FileTableModel::ColName).data().toString();
    const quint64 size = m_model->sizeAt(row);
    m_queue->enqueueDownload(ufId, name, size);
}

void MainWindow::onUploadClicked() {
    // P8-2 的骨架入口：拖拽是主路径，按钮先给提示（P8-3 接 QFileDialog）。
    statusBar()->showMessage(QString::fromUtf8("把文件拖进窗口即可上传"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (m_queue == nullptr) {
        return;
    }
    const QList<QUrl> urls = event->mimeData()->urls();
    for (int i = 0; i < urls.size(); ++i) {
        const QString path = urls.at(i).toLocalFile();
        if (!path.isEmpty() && QFileInfo(path).isFile()) {
            m_queue->enqueueUpload(path, QFileInfo(path).fileName());
        }
    }
    event->acceptProposedAction();
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
    if (m_transferPane != nullptr) {
        m_transferPane->setLinkText(state == 1 ? QString() : detail);
    }
    if (state != 1 && m_queue != nullptr) {
        m_queue->onLinkLost();
    }
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
