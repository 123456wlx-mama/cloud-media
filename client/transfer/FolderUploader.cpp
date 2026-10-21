#include "transfer/FolderUploader.h"

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QPair>

#include "netapi/NetClient.h"
#include "transfer/TransferQueue.h"

namespace cm {
namespace transfer {

FolderUploader::FolderUploader(netapi::NetClient* net, TransferQueue* queue,
                               QObject* parent)
    : QObject(parent), m_net(net), m_queue(queue), m_dirIndex(0), m_dirsDone(0),
      m_busy(false), m_waiting(false) {
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(addFolderFinished(quint32, quint64, QString)), this,
                SLOT(onAddFolderFinished(quint32, quint64, QString)));
        connect(m_net, SIGNAL(requestFailed(quint16, QString)), this,
                SLOT(onRequestFailed(quint16, QString)));
    }
}

bool FolderUploader::start(const QString& localDir, quint64 remoteParentId) {
    if (m_busy || m_net == nullptr || m_queue == nullptr) {
        return false;
    }
    QDir root(localDir);
    if (!root.exists()) {
        emit failed(QString::fromUtf8("本地文件夹不存在"));
        return false;
    }

    m_dirs.clear();
    m_files.clear();
    m_remoteIds.clear();
    m_remoteIds.insert(QString(), remoteParentId);
    m_dirIndex = 0;
    m_dirsDone = 0;
    m_busy = true;
    m_waiting = false;

    scan(root.absolutePath());
    if (m_dirs.isEmpty() && m_files.isEmpty()) {
        m_busy = false;
        emit failed(QString::fromUtf8("文件夹是空的"));
        return false;
    }
    nextDir();
    return true;
}

void FolderUploader::scan(const QString& localDir) {
    // BFS：保证 m_dirs 里父目录一定排在子目录前面（串行建目录依赖这个顺序）。
    QVector<QPair<QString, QString> > queue;  // (绝对路径, 相对路径)
    queue.append(qMakePair(localDir, QString()));

    int head = 0;
    while (head < queue.size()) {
        const QString abs = queue.at(head).first;
        const QString rel = queue.at(head).second;
        ++head;

        QDir dir(abs);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (int i = 0; i < entries.size(); ++i) {
            const QFileInfo& info = entries.at(i);
            const QString childRel =
                rel.isEmpty() ? info.fileName() : rel + QLatin1Char('/') + info.fileName();
            if (info.isDir()) {
                DirJob job;
                job.rel = childRel;
                job.parent = rel;
                job.name = info.fileName();
                m_dirs.append(job);
                queue.append(qMakePair(info.absoluteFilePath(), childRel));
            } else {
                FileJob file;
                file.localPath = info.absoluteFilePath();
                file.parent = rel;
                file.name = info.fileName();
                m_files.append(file);
            }
        }
    }
}

void FolderUploader::nextDir() {
    if (m_dirIndex >= m_dirs.size()) {
        enqueueFiles();
        return;
    }
    const DirJob& job = m_dirs.at(m_dirIndex);
    const quint64 parentId = m_remoteIds.value(job.parent, 0);
    m_waiting = true;
    m_net->addFolder(parentId, job.name);
}

void FolderUploader::onAddFolderFinished(quint32 code, quint64 ufId,
                                         const QString& name) {
    Q_UNUSED(name);
    if (!m_busy || !m_waiting) {
        return;  // 不是本 uploader 发起的（如用户手动新建文件夹）
    }
    m_waiting = false;
    if (code != 0) {
        abort(QString::fromUtf8("创建目录失败(code=%1)").arg(code));
        return;
    }
    m_remoteIds.insert(m_dirs.at(m_dirIndex).rel, ufId);
    ++m_dirIndex;
    ++m_dirsDone;
    nextDir();
}

void FolderUploader::enqueueFiles() {
    int queued = 0;
    for (int i = 0; i < m_files.size(); ++i) {
        const FileJob& file = m_files.at(i);
        const quint64 parentId = m_remoteIds.value(file.parent, 0);
        m_queue->enqueueUpload(file.localPath, file.name, parentId);
        ++queued;
    }
    const int dirs = m_dirsDone;
    m_busy = false;
    emit finished(dirs, queued);
}

void FolderUploader::abort(const QString& reason) {
    m_busy = false;
    m_waiting = false;
    emit failed(reason);
}

void FolderUploader::onRequestFailed(quint16 type, const QString& reason) {
    // 只关心建目录请求的失败（10015）；传输层自己的失败由 TransferQueue 处理。
    if (!m_busy || type != 10015) {
        return;
    }
    abort(QString::fromUtf8("创建目录失败：%1").arg(reason));
}

}  // namespace transfer
}  // namespace cm
