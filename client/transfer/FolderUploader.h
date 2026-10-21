#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace cm {

namespace netapi {
class NetClient;
}

namespace transfer {

class TransferQueue;

// 上传整个本地文件夹：
//   1) 本地递归扫描（BFS），得到目录树与文件清单；
//   2) 按 BFS 顺序逐级创建远端目录（串行等 AddFolderRs，父目录 ufId 已知后才建子目录）；
//   3) 全部目录建好后，把文件按所属目录 parentId 入队到 TransferQueue。
// 只发 AddFolderRq / 入队上传，不自己碰 socket 与文件内容。
class FolderUploader : public QObject {
    Q_OBJECT

public:
    FolderUploader(netapi::NetClient* net, TransferQueue* queue,
                   QObject* parent = nullptr);

    bool start(const QString& localDir, quint64 remoteParentId);
    bool busy() const { return m_busy; }

signals:
    void finished(int dirs, int files);
    void failed(const QString& reason);

private slots:
    void onAddFolderFinished(quint32 code, quint64 ufId, const QString& name);
    void onRequestFailed(quint16 type, const QString& reason);

private:
    struct DirJob {
        QString rel;     // 相对根目录的路径（根为 ""）
        QString parent;  // 父目录相对路径
        QString name;    // 目录名
    };
    struct FileJob {
        QString localPath;
        QString parent;  // 所属目录相对路径
        QString name;
    };

    void scan(const QString& localDir);
    void nextDir();
    void enqueueFiles();
    void abort(const QString& reason);

    netapi::NetClient* m_net;
    TransferQueue* m_queue;
    QVector<DirJob> m_dirs;
    QVector<FileJob> m_files;
    QHash<QString, quint64> m_remoteIds;  // 相对路径 → 远端 ufId
    int m_dirIndex;
    int m_dirsDone;
    bool m_busy;
    bool m_waiting;
};

}  // namespace transfer
}  // namespace cm
