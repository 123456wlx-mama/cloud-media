#include "ui/MainWindow.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTableView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "netapi/NetClient.h"
#include "recycleapi/RecycleFlow.h"
#include "shareapi/ShareFlow.h"
#include "transfer/FolderUploader.h"
#include "transfer/TransferQueue.h"
#include "ui/FileTableModel.h"
#include "ui/PlayerPane.h"
#include "ui/RecycleDialog.h"
#include "ui/ShareDialog.h"
#include "ui/Toast.h"
#include "ui/TransferPane.h"

namespace cm {
namespace ui {
namespace {

const char* kTransferHint = "没有传输任务\n上传 / 下载进度会实时显示在这里";
const char* kPlayerHint = "暂无播放内容\n在左侧双击音视频文件即可开始播放";
const quint32 kPageSize = 50;

// 双击这些后缀走播放器（其余给提示，不做"用系统播放器打开"的自动降级）
bool isVideoFile(const QString& name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QString::fromUtf8("mp4") || suffix == QString::fromUtf8("mkv") ||
           suffix == QString::fromUtf8("avi") || suffix == QString::fromUtf8("mov") ||
           suffix == QString::fromUtf8("flv") || suffix == QString::fromUtf8("ts") ||
           suffix == QString::fromUtf8("m4v") || suffix == QString::fromUtf8("webm");
}

// 自绘文本输入框：QInputDialog 在深色 QSS 下尺寸给不足（用户要手动拉伸），
// 还带个没用的"?"按钮。这里自己搭：固定最小宽、深色、两个按钮。
bool askText(QWidget* parent, const QString& title, const QString& label,
             const QString& initial, QString* out) {
    if (out == nullptr) {
        return false;
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.setObjectName(QString::fromUtf8("formDialog"));
    dialog.setModal(true);
    dialog.setMinimumWidth(420);

    QVBoxLayout* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 20, 24, 18);
    root->setSpacing(12);

    QLabel* tip = new QLabel(label, &dialog);
    root->addWidget(tip);

    QLineEdit* edit = new QLineEdit(initial, &dialog);
    edit->selectAll();
    root->addWidget(edit);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    QPushButton* cancel = new QPushButton(QString::fromUtf8("取消"), &dialog);
    cancel->setObjectName(QString::fromUtf8("rowButton"));
    QPushButton* ok = new QPushButton(QString::fromUtf8("确定"), &dialog);
    ok->setObjectName(QString::fromUtf8("primaryButton"));
    ok->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    root->addLayout(buttons);

    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(edit, &QLineEdit::returnPressed, &dialog, &QDialog::accept);

    edit->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    *out = edit->text().trimmed();
    return !out->isEmpty();
}

}  // namespace

MainWindow::MainWindow(netapi::NetClient* net, transfer::TransferQueue* queue,
                       QWidget* parent, bool forcePainter)
    : QMainWindow(parent), m_net(net), m_queue(queue), m_share(nullptr),
      m_recycle(nullptr), m_folderUploader(nullptr), m_transferPane(nullptr),
      m_playerPane(nullptr),
      m_splitter(nullptr),
      m_model(nullptr), m_table(nullptr), m_fileEmpty(nullptr),
      m_linkBadge(nullptr), m_listHint(nullptr), m_pathLabel(nullptr),
      m_upButton(nullptr), m_moreButton(nullptr), m_toast(nullptr),
      m_forcePainter(forcePainter), m_cursorId(0), m_hasMore(false) {
    setWindowTitle(QString::fromUtf8("cloud-media"));
    resize(1280, 800);

    m_path.append(qMakePair(static_cast<quint64>(0),
                            QString::fromUtf8("全部文件")));
    if (m_net != nullptr) {
        m_share = new shareapi::ShareFlow(m_net, this);
        m_recycle = new recycleapi::RecycleFlow(m_net, this);
    }

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
    // 初始比例：文件区宽一点、传输区窄一点，避免三栏都空的时候中间留一大片黑
    m_splitter->setSizes(QList<int>() << 560 << 280 << 480);
    // QSplitter 不给子控件最小宽度时会被挤成 0（截图里播放区/传输区时隐时现）。
    if (QWidget* w = m_splitter->widget(0)) {
        w->setMinimumWidth(420);
    }
    if (QWidget* w = m_splitter->widget(1)) {
        w->setMinimumWidth(200);
    }
    if (QWidget* w = m_splitter->widget(2)) {
        w->setMinimumWidth(320);
    }
    setCentralWidget(m_splitter);

    statusBar()->setObjectName(QString::fromUtf8("mainStatusBar"));
    m_linkBadge = new QLabel(QString::fromUtf8("● 连接中…"), this);
    m_linkBadge->setObjectName(QString::fromUtf8("linkBadge"));
    statusBar()->addPermanentWidget(m_linkBadge);
    statusBar()->showMessage(QString::fromUtf8("未登录"));
    // 链路可能在本窗口创建前就连上了（connectTo 早于 MainWindow），
    // 那时 linkState(1) 已经发过，这里补一次当前状态，否则徽标永远停在"连接中"。
    if (m_net != nullptr) {
        setLinkBadge(m_net->linkStateValue(), QString());
    }

    m_toast = new Toast(centralWidget());
    if (m_recycle != nullptr) {
        connect(m_toast, &Toast::actionTriggered, this, [this]() {
            m_recycle->undo();  // Toast 上的「撤销」= RestoreRq（契约 §2.2）
        });
        connect(m_recycle, &recycleapi::RecycleFlow::deleted, this,
                &MainWindow::onDeleted);
        connect(m_recycle, &recycleapi::RecycleFlow::restored, this,
                [this](quint64, const QString& name) {
                    m_toast->showText(
                        name.isEmpty() ? QString::fromUtf8("已恢复")
                                       : QString::fromUtf8("「%1」已恢复").arg(name));
                    refreshList();
                });
        connect(m_recycle, &recycleapi::RecycleFlow::failed, this,
                [this](const QString& text) { m_toast->showText(text); });
    }
    if (m_share != nullptr) {
        connect(m_share, &shareapi::ShareFlow::created, this,
                &MainWindow::onShareCreated);
        connect(m_share, &shareapi::ShareFlow::opened, this,
                &MainWindow::onShareOpened);
        connect(m_share, &shareapi::ShareFlow::cancelled, this,
                [this](const QString&) {
                    m_toast->showText(QString::fromUtf8("已取消分享"));
                });
        connect(m_share, &shareapi::ShareFlow::failed, this,
                [this](quint32, const QString& text) {
                    if (!m_share->busy()) {
                        m_toast->showText(text);
                    }
                });
    }
    if (m_queue != nullptr) {
        connect(m_queue, &transfer::TransferQueue::instantUploaded, this,
                [this](const QString& name) {
                    m_toast->showText(
                        QString::fromUtf8("「%1」已秒传 · 0 字节").arg(name));
                });
    }

    if (m_net != nullptr && m_queue != nullptr) {
        m_folderUploader = new transfer::FolderUploader(m_net, m_queue, this);
        connect(m_folderUploader, &transfer::FolderUploader::finished, this,
                [this](int dirs, int files) {
                    if (m_toast != nullptr) {
                        m_toast->showText(
                            QString::fromUtf8("文件夹已入队：%1 个目录 / %2 个文件")
                                .arg(dirs)
                                .arg(files));
                    }
                });
        connect(m_folderUploader, &transfer::FolderUploader::failed, this,
                [this](const QString& reason) {
                    if (m_toast != nullptr) {
                        m_toast->showText(reason);
                    }
                });
    }

    if (m_net != nullptr) {
        connect(m_net, SIGNAL(linkState(int, QString)), this,
                SLOT(onLinkState(int, QString)));
        connect(m_net, SIGNAL(loginFinished(quint32, quint64, QString)), this,
                SLOT(onLoginSucceeded(quint32, quint64, QString)));
        connect(m_net,
                SIGNAL(listFinished(quint32, QVariantList, bool, QString, quint64)),
                this,
                SLOT(onListFinished(quint32, QVariantList, bool, QString, quint64)));
        connect(m_net, SIGNAL(requestFailed(quint16, QString)), this,
                SLOT(onRequestFailed(quint16, QString)));
        connect(m_net, SIGNAL(addFolderFinished(quint32, quint64, QString)), this,
                SLOT(onAddFolderFinished(quint32, quint64, QString)));
        if (m_net->uid() != 0) {
            refreshList();
        }
    }
}

MainWindow::~MainWindow() {
    // 先停播放（此时所有子控件还活着），再让子控件自己销毁
    if (m_playerPane != nullptr) {
        m_playerPane->shutdown();
    }
    m_toast = nullptr;
    m_playerPane = nullptr;
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

    // 面包屑 + 工具栏（契约 §1.2：目录浏览、上传入口）
    QWidget* bar = new QWidget(pane);
    bar->setObjectName(QString::fromUtf8("pathBar"));
    bar->setFixedHeight(38);
    QHBoxLayout* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 0, 8, 0);
    barLayout->setSpacing(6);
    m_upButton = new QPushButton(QString::fromUtf8("← 上级"), bar);
    m_upButton->setObjectName(QString::fromUtf8("rowButton"));
    m_upButton->setEnabled(false);
    barLayout->addWidget(m_upButton);
    m_pathLabel = new QLabel(bar);
    m_pathLabel->setObjectName(QString::fromUtf8("pathLabel"));
    barLayout->addWidget(m_pathLabel, 1);

    // 工具栏空间有限，标签取短名 + 悬浮提示（否则字号一大就被裁字）
    QPushButton* openShare = new QPushButton(QString::fromUtf8("分享码"), bar);
    openShare->setObjectName(QString::fromUtf8("rowButton"));
    openShare->setToolTip(QString::fromUtf8("输入分享码打开别人的分享"));
    barLayout->addWidget(openShare);
    QPushButton* recycle = new QPushButton(QString::fromUtf8("回收站"), bar);
    recycle->setObjectName(QString::fromUtf8("rowButton"));
    recycle->setToolTip(QString::fromUtf8("已删除的文件"));
    barLayout->addWidget(recycle);
    QPushButton* addFolder = new QPushButton(QString::fromUtf8("新建"), bar);
    addFolder->setObjectName(QString::fromUtf8("rowButton"));
    addFolder->setToolTip(QString::fromUtf8("新建文件夹"));
    barLayout->addWidget(addFolder);
    QPushButton* refresh = new QPushButton(QString::fromUtf8("刷新"), bar);
    refresh->setObjectName(QString::fromUtf8("rowButton"));
    barLayout->addWidget(refresh);
    layout->addWidget(bar);

    connect(m_upButton, &QPushButton::clicked, this, &MainWindow::onGoUp);
    connect(openShare, &QPushButton::clicked, this, &MainWindow::onOpenShare);
    connect(recycle, &QPushButton::clicked, this, &MainWindow::onOpenRecycle);
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshList);
    connect(addFolder, &QPushButton::clicked, this,
            &MainWindow::onAddFolderClicked);

    m_model = new FileTableModel(this);
    m_table = new QTableView(pane);
    m_table->setObjectName(QString::fromUtf8("fileTable"));
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    // 文件名过长时右省略 + 悬浮看全名（配合 FileTableModel 的 ToolTipRole）
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setWordWrap(false);
    m_table->setIconSize(QSize(20, 20));
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(58);  // 字号调大后行高同步
    QHeaderView* tableHeader = m_table->horizontalHeader();
    tableHeader->setStretchLastSection(false);
    tableHeader->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 名称列吃剩余宽度，其余按内容自适应；否则窄面板下"名称"会被挤掉半截。
    tableHeader->setSectionResizeMode(FileTableModel::ColName, QHeaderView::Stretch);
    tableHeader->setSectionResizeMode(FileTableModel::ColSize,
                                      QHeaderView::ResizeToContents);
    tableHeader->setSectionResizeMode(FileTableModel::ColKind,
                                      QHeaderView::ResizeToContents);
    tableHeader->setSectionResizeMode(FileTableModel::ColUpdatedAt,
                                      QHeaderView::ResizeToContents);
    m_table->setVisible(false);
    layout->addWidget(m_table, 1);

    m_fileEmpty = new QLabel(
        QString::fromUtf8("这里还是空的\n把文件拖进窗口，或点下面的「上传文件 / 上传文件夹」"), pane);
    m_fileEmpty->setObjectName(QString::fromUtf8("emptyHint"));
    m_fileEmpty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_fileEmpty, 1);

    m_moreButton = new QPushButton(QString::fromUtf8("加载更多"), pane);
    m_moreButton->setObjectName(QString::fromUtf8("rowButton"));
    m_moreButton->setVisible(false);
    QHBoxLayout* moreRow = new QHBoxLayout();
    moreRow->addStretch(1);
    moreRow->addWidget(m_moreButton);
    moreRow->addStretch(1);
    layout->addLayout(moreRow);
    connect(m_moreButton, &QPushButton::clicked, this, &MainWindow::onLoadMore);

    QPushButton* upload = new QPushButton(QString::fromUtf8("上传文件"), pane);
    upload->setObjectName(QString::fromUtf8("primaryButton"));
    QPushButton* uploadFolder = new QPushButton(QString::fromUtf8("上传文件夹"), pane);
    // 与「上传文件」同款主按钮：外观、hover/按下效果一致
    uploadFolder->setObjectName(QString::fromUtf8("primaryButton"));
    // 两枚按钮统一宽度：以「上传文件夹」（5 字，比「上传文件」宽）为基准，
    // 且不用 setMaximumWidth，否则窄面板下会被裁成「上传文件刂」。
    // 152 是 21px 字号 + 左右各 16px padding 的安全下限，字体变大时按 sizeHint 自动放宽。
    const int uploadButtonWidth = qMax(152, uploadFolder->sizeHint().width());
    upload->setMinimumWidth(uploadButtonWidth);
    uploadFolder->setMinimumWidth(uploadButtonWidth);
    QHBoxLayout* emptyRow = new QHBoxLayout();
    emptyRow->addStretch(1);
    emptyRow->addWidget(upload);
    emptyRow->addSpacing(8);
    emptyRow->addWidget(uploadFolder);
    emptyRow->addStretch(1);
    layout->addLayout(emptyRow);
    connect(upload, SIGNAL(clicked()), this, SLOT(onUploadClicked()));
    connect(uploadFolder, SIGNAL(clicked()), this, SLOT(onUploadFolderClicked()));

    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!index.isValid()) {
            return;
        }
        if (m_model->isDirAt(index.row())) {
            onEnterFolder();
            return;
        }
        startPlayback(index.row());  // 双击视频 → 边下边播（契约 §1.3）
    });

    // 右键菜单：打开（目录）/ 下载 / 分享 / 删除。重命名已从契约移除（协议无 RenameRq）。
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        const QModelIndex index = m_table->indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        m_table->selectRow(index.row());
        const bool isDir = m_model->isDirAt(index.row());
        QMenu menu(this);
        QAction* enter = nullptr;
        if (isDir) {
            enter = menu.addAction(QString::fromUtf8("打开文件夹"));
        }
        QAction* download = menu.addAction(QString::fromUtf8("下载"));
        QAction* share = menu.addAction(QString::fromUtf8("分享"));
        menu.addSeparator();
        QAction* del = menu.addAction(QString::fromUtf8("删除"));
        download->setEnabled(!isDir);
        share->setEnabled(!isDir);  // 分享只对已完成文件可用（契约 §1.3）
        QAction* chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
        if (chosen == nullptr) {
            return;
        }
        if (chosen == enter) {
            onEnterFolder();
        } else if (chosen == download) {
            onDownloadSelected();
        } else if (chosen == share) {
            onShareSelected();
        } else if (chosen == del) {
            onDeleteSelected();
        }
    });
    updateBreadcrumb();
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
    if (m_queue == nullptr) {
        return buildPane(QString::fromUtf8("播放"), QString::fromUtf8(kPlayerHint),
                         QString::fromUtf8("playerPane"));
    }
    m_playerPane = new PlayerPane(m_queue, this, m_forcePainter);
    connect(m_playerPane, &PlayerPane::notice, this, [this](const QString& text) {
        if (m_toast != nullptr) {
            m_toast->showText(text);
        }
    });
    return m_playerPane;
}

quint64 MainWindow::currentParent() const {
    return m_path.isEmpty() ? 0 : m_path.last().first;
}

void MainWindow::updateBreadcrumb() {
    QStringList parts;
    for (int i = 0; i < m_path.size(); ++i) {
        parts.append(m_path.at(i).second);
    }
    m_pathLabel->setText(parts.join(QString::fromUtf8("  /  ")));
    m_upButton->setEnabled(m_path.size() > 1);
}

void MainWindow::refreshList() {
    loadPage(true);
}

void MainWindow::loadPage(bool reset) {
    if (m_net == nullptr) {
        return;
    }
    if (reset) {
        m_model->clearRows();
        m_cursorKey.clear();
        m_cursorId = 0;
        m_hasMore = false;
        m_moreButton->setVisible(false);
    }
    m_listHint->setText(QString::fromUtf8("加载中…"));
    m_net->listFiles(currentParent(), kPageSize, m_cursorKey, m_cursorId);
}

void MainWindow::onLoadMore() {
    if (!m_hasMore) {
        return;
    }
    loadPage(false);
}

void MainWindow::onGoUp() {
    if (m_path.size() <= 1) {
        return;
    }
    m_path.removeLast();
    updateBreadcrumb();
    refreshList();
}

void MainWindow::onEnterFolder() {
    const int row = currentRow();
    if (row < 0 || !m_model->isDirAt(row)) {
        return;
    }
    m_path.append(qMakePair(m_model->ufIdAt(row), m_model->nameAt(row)));
    updateBreadcrumb();
    refreshList();
}

int MainWindow::currentRow() const {
    if (m_table == nullptr || m_table->selectionModel() == nullptr) {
        return -1;
    }
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return -1;
    }
    return rows.first().row();
}

void MainWindow::onDownloadSelected() {
    if (m_queue == nullptr) {
        return;
    }
    const int row = currentRow();
    if (row < 0) {
        return;
    }
    const quint64 ufId = m_model->ufIdAt(row);
    if (ufId == 0 || m_model->isDirAt(row)) {
        return;
    }
    m_queue->enqueueDownload(ufId, m_model->nameAt(row), m_model->sizeAt(row));
    m_toast->showText(
        QString::fromUtf8("「%1」已加入下载").arg(m_model->nameAt(row)));
}

void MainWindow::onShareSelected() {
    if (m_share == nullptr) {
        return;
    }
    const int row = currentRow();
    if (row < 0 || m_model->isDirAt(row)) {
        return;
    }
    m_share->create(m_model->ufIdAt(row), 2 /*7 天*/, 1 /*生成提取码*/);
}

void MainWindow::onShareCreated() {
    ShareDialog dialog(m_share->info(), this);
    connect(&dialog, &ShareDialog::cancelRequested, this,
            [this](const QString& code) { m_share->cancel(code); });
    dialog.exec();
}

void MainWindow::onShareOpened() {
    const shareapi::ShareInfo& info = m_share->info();
    if (m_queue == nullptr || info.shareTicket.isEmpty()) {
        return;
    }
    m_queue->enqueueSharedDownload(info.shareTicket, info.fileName, info.size);
    m_toast->showText(
        QString::fromUtf8("「%1」已加入下载").arg(info.fileName));
}

void MainWindow::onDeleteSelected() {
    if (m_recycle == nullptr) {
        return;
    }
    const int row = currentRow();
    if (row < 0) {
        return;
    }
    const QString name = m_model->nameAt(row);
    if (m_model->isDirAt(row)) {
        m_toast->showText(QString::fromUtf8("文件夹删除暂未开放"));
        return;
    }
    if (hasActiveTransferFor(name)) {
        // 契约 §1.3：删除不取消正在跑的任务，先提示
        m_toast->showText(
            QString::fromUtf8("「%1」正在传输，完成后再删除").arg(name));
        return;
    }
    m_recycle->deleteFile(m_model->ufIdAt(row), name);
}

bool MainWindow::hasActiveTransferFor(const QString& name) const {
    if (m_queue == nullptr) {
        return false;
    }
    const QVector<transfer::TransferTask>& tasks = m_queue->tasks();
    for (int i = 0; i < tasks.size(); ++i) {
        if (tasks.at(i).remoteName == name && !tasks.at(i).isTerminal()) {
            return true;
        }
    }
    return false;
}

void MainWindow::onDeleted(quint64 ufId, const QString& name) {
    Q_UNUSED(ufId);
    m_toast->showText(QString::fromUtf8("「%1」已移入回收站").arg(name),
                      QString::fromUtf8("撤销"));
    refreshList();
}

void MainWindow::onOpenShare() {
    if (m_share == nullptr) {
        return;
    }
    ShareOpenDialog dialog(m_share, this);
    dialog.exec();
}

void MainWindow::onOpenRecycle() {
    if (m_recycle == nullptr) {
        return;
    }
    RecycleDialog dialog(m_recycle, this);
    dialog.exec();
}

void MainWindow::startPlayback(int row) {
    if (m_playerPane == nullptr || row < 0) {
        return;
    }
    const QString name = m_model->nameAt(row);
    if (!isVideoFile(name)) {
        if (m_toast != nullptr) {
            m_toast->showText(QString::fromUtf8("「%1」不是视频文件").arg(name));
        }
        return;
    }
    m_playerPane->playRemoteFile(m_model->ufIdAt(row), name, m_model->sizeAt(row));
}

void MainWindow::playLocalFile(const QString& path) {
    if (m_playerPane != nullptr) {
        m_playerPane->playLocalFile(path);
    }
}

void MainWindow::shareByUfId(quint64 ufId) {
    if (m_model == nullptr) {
        return;
    }
    for (int i = 0; i < m_model->rowCount(); ++i) {
        if (m_model->ufIdAt(i) == ufId) {
            m_table->selectRow(i);
            onShareSelected();
            return;
        }
    }
}

void MainWindow::playRemoteByUfId(quint64 ufId) {
    if (m_model == nullptr) {
        return;
    }
    for (int i = 0; i < m_model->rowCount(); ++i) {
        if (m_model->ufIdAt(i) == ufId) {
            startPlayback(i);
            return;
        }
    }
    if (m_toast != nullptr) {
        m_toast->showText(QString::fromUtf8("当前目录里没有该文件"));
    }
}

void MainWindow::onUploadClicked() {
    // 与拖拽共用 enqueueUpload 路径：按钮走文件选择框（多选），拖拽走 dropEvent。
    if (m_queue == nullptr) {
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QString::fromUtf8("选择要上传的文件"), QString());
    const quint64 parentId = currentParent();
    for (int i = 0; i < files.size(); ++i) {
        const QString path = files.at(i);
        if (!path.isEmpty()) {
            m_queue->enqueueUpload(path, QFileInfo(path).fileName(), parentId);
        }
    }
    if (!files.isEmpty()) {
        statusBar()->showMessage(
            QString::fromUtf8("已加入 %1 个上传任务").arg(files.size()));
    }
}

void MainWindow::onUploadFolderClicked() {
    if (m_folderUploader == nullptr) {
        return;
    }
    if (m_folderUploader->busy()) {
        if (m_toast != nullptr) {
            m_toast->showText(QString::fromUtf8("上一个文件夹还没处理完"));
        }
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString::fromUtf8("选择要上传的文件夹"), QString());
    if (dir.isEmpty()) {
        return;
    }
    if (!m_folderUploader->start(dir, currentParent())) {
        return;  // 失败原因已通过 failed 信号提示
    }
    if (m_toast != nullptr) {
        m_toast->showText(QString::fromUtf8("正在创建目录并上传…"));
    }
}

void MainWindow::onAddFolderClicked() {
    if (m_net == nullptr) {
        return;
    }
    QString name;
    if (!askText(this, QString::fromUtf8("新建文件夹"),
                 QString::fromUtf8("请输入文件夹名："),
                 QString::fromUtf8("新建文件夹"), &name)) {
        return;
    }
    m_net->addFolder(currentParent(), name);
}

void MainWindow::onAddFolderFinished(quint32 code, quint64 ufId,
                                     const QString& name) {
    Q_UNUSED(ufId);
    if (m_toast == nullptr) {
        return;
    }
    // 上传文件夹时由 FolderUploader 批量建目录，这里不逐条弹提示、不反复刷新。
    if (m_folderUploader != nullptr && m_folderUploader->busy()) {
        return;
    }
    if (code == 0) {
        m_toast->showText(QString::fromUtf8("已新建文件夹「%1」").arg(name));
        refreshList();
        return;
    }
    if (code == 1003) {
        m_toast->showText(QString::fromUtf8("同名文件夹太多，换个名字"));
        return;
    }
    m_toast->showText(QString::fromUtf8("新建文件夹失败(code=%1)").arg(code));
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
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        if (info.isDir()) {
            // 拖文件夹进来 = 递归建目录后批量上传；一次只处理一个文件夹。
            if (m_folderUploader != nullptr && !m_folderUploader->busy()) {
                m_folderUploader->start(path, currentParent());
            }
        } else if (info.isFile()) {
            m_queue->enqueueUpload(path, info.fileName(), currentParent());
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
        case 0:
            text = QString::fromUtf8("● 连接中…");
            color = QString::fromUtf8("#F0A63A");
            break;
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

void MainWindow::onListFinished(quint32 code, const QVariantList& rows,
                                bool hasMore, const QString& nextKey,
                                quint64 nextId) {
    if (code != 0) {
        m_listHint->setText(QString::fromUtf8("加载失败 · code=%1").arg(code));
        return;
    }
    m_model->appendRows(rows);
    m_hasMore = hasMore;
    m_cursorKey = nextKey;
    m_cursorId = nextId;
    m_moreButton->setVisible(hasMore);

    const int total = m_model->rowCount();
    m_listHint->setText(QString::fromUtf8("%1 项%2")
                            .arg(total)
                            .arg(hasMore ? QString::fromUtf8("（还有更多）")
                                         : QString()));
    const bool empty = total == 0;
    m_table->setVisible(!empty);
    m_fileEmpty->setVisible(empty);
    statusBar()->showMessage(
        QString::fromUtf8("已登录 · %1").arg(m_net != nullptr ? m_net->userName()
                                                              : QString()));
}

void MainWindow::onRequestFailed(quint16 type, const QString& reason) {
    Q_UNUSED(type);
    m_listHint->setText(QString::fromUtf8("加载失败 · %1").arg(reason));
    statusBar()->showMessage(reason);
}

}  // namespace ui
}  // namespace cm
