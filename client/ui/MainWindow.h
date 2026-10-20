#pragma once

#include <QMainWindow>
#include <QPair>
#include <QString>
#include <QVariantList>
#include <QVector>

class QSplitter;
class QTableView;
class QLabel;
class QPushButton;

namespace cm {

namespace netapi {
class NetClient;
}

namespace transfer {
class TransferQueue;
}

namespace shareapi {
class ShareFlow;
}

namespace recycleapi {
class RecycleFlow;
}

namespace ui {

class FileTableModel;
class TransferPane;
class PlayerPane;
class Toast;

// 三段式主窗口：文件区 / 传输区 / 播放区。
// P8-3：文件区分页 + 面包屑、右键下载/分享/删除、秒传与删除 Toast、打开分享、回收站。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(netapi::NetClient* net, transfer::TransferQueue* queue,
               QWidget* parent = nullptr, bool forcePainter = false);
    ~MainWindow() override;

public slots:
    void refreshList();
    void playLocalFile(const QString& path);
    void playRemoteByUfId(quint64 ufId);  // 联调/命令行入口：按 ufId 起播
    void shareByUfId(quint64 ufId);       // 联调/截图入口：按 ufId 弹分享窗

private slots:
    void onLoginSucceeded(quint32 code, quint64 uid, const QString& name);
    void onLinkState(int state, const QString& detail);
    void onListFinished(quint32 code, const QVariantList& rows, bool hasMore,
                        const QString& nextKey, quint64 nextId);
    void onRequestFailed(quint16 type, const QString& reason);

    void onDownloadSelected();
    void onUploadClicked();
    void onShareSelected();
    void onDeleteSelected();
    void onEnterFolder();
    void onGoUp();
    void onLoadMore();
    void onOpenShare();
    void onOpenRecycle();
    void onShareCreated();
    void onShareOpened();
    void onDeleted(quint64 ufId, const QString& name);
    void startPlayback(int row);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    QWidget* buildPane(const QString& title, const QString& hint,
                       const QString& objectName);
    QWidget* buildFilePane();
    QWidget* buildTransferPane();
    QWidget* buildPlayerPane();
    void setLinkBadge(int state, const QString& detail);

    int currentRow() const;
    bool hasActiveTransferFor(const QString& name) const;
    void loadPage(bool reset);
    void updateBreadcrumb();
    quint64 currentParent() const;

    netapi::NetClient* m_net;
    transfer::TransferQueue* m_queue;
    shareapi::ShareFlow* m_share;
    recycleapi::RecycleFlow* m_recycle;
    TransferPane* m_transferPane;
    PlayerPane* m_playerPane;
    QSplitter* m_splitter;
    FileTableModel* m_model;
    QTableView* m_table;
    QLabel* m_fileEmpty;
    QLabel* m_linkBadge;
    QLabel* m_listHint;
    QLabel* m_pathLabel;
    QPushButton* m_upButton;
    QPushButton* m_moreButton;
    Toast* m_toast;
    bool m_forcePainter;

    // 面包屑栈：(ufId, 显示名)，首项固定为 (0, 全部文件)
    QVector<QPair<quint64, QString> > m_path;
    QString m_cursorKey;
    quint64 m_cursorId;
    bool m_hasMore;
};

}  // namespace ui
}  // namespace cm
