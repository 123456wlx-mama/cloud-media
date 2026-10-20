#include "transfer/TransferQueue.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaType>
#include <QTextStream>

#include <string>

#include "cm/base/md5.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/serialize.h"
#include "cm/protocol/messages/download.h"
#include "cm/protocol/messages/upload.h"
#include "netapi/NetClient.h"
#include "sqlapi/TaskStore.h"

namespace cm {
namespace transfer {
namespace {

const quint32 kBlockSize = 4194304;   // 4MB，与服务端 kChunkSize 一致
const int kMaxUploads = 2;            // 上传并发文件数
const int kMaxDownloads = 2;          // 下载并发文件数

quint16 t(protocol::MessageType type) { return static_cast<std::uint16_t>(type); }

template <typename Rq>
QByteArray encodePayload(const Rq& rq) {
    std::string payload;
    if (!rq.encode(&payload)) {
        return QByteArray();
    }
    return QByteArray(payload.data(), static_cast<int>(payload.size()));
}

std::string toStd(const QByteArray& data) {
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

QString hexOf(const QByteArray& data) { return QString::fromLatin1(data.toHex()); }

QByteArray fromHex(const QString& hex) { return QByteArray::fromHex(hex.toLatin1()); }

bool fileMd5(const QString& path, QString* hex) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    cm::Md5 md5;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) {
            break;
        }
        md5.update(chunk.constData(), static_cast<std::size_t>(chunk.size()));
    }
    *hex = QString::fromStdString(md5.hex());
    return true;
}

}  // namespace

TransferQueue::TransferQueue(netapi::NetClient* net, sqlapi::TaskStore* store,
                             QObject* parent)
    : QObject(parent), m_net(net), m_store(store), m_seq(0), m_nextId(1),
      m_runningUploads(0), m_runningDownloads(0), m_linkUp(false) {
    m_clock = []() { return QDateTime::currentMSecsSinceEpoch(); };
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(frame(quint16, quint32, QByteArray)), this,
                SLOT(onFrame(quint16, quint32, QByteArray)));
        connect(m_net, SIGNAL(requestTimeout(quint32, quint16)), this,
                SLOT(onTimeout(quint32, quint16)));
        connect(m_net, SIGNAL(requestAborted(quint32, quint16, QString)), this,
                SLOT(onAborted(quint32, quint16, QString)));
        connect(m_net, SIGNAL(linkState(int, QString)), this,
                SLOT(onLinkState(int, QString)));
    }
}

void TransferQueue::setClock(std::function<qint64()> clock) {
    if (clock) {
        m_clock = clock;
    }
}

int TransferQueue::nextTaskId() { return m_nextId++; }

TransferTask* TransferQueue::find(int id) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].id == id) {
            return &m_tasks[i];
        }
    }
    return nullptr;
}

void* TransferQueue::taskPtr(int taskId) { return find(taskId); }

void TransferQueue::touch(int taskId, bool persist) {
    TransferTask* task = find(taskId);
    if (task == nullptr) {
        return;
    }
    if (persist && m_store != nullptr) {
        m_store->saveTask(*task);
        m_store->saveBitmap(task->id, task->bitmap);
    }
    emit changed();
}

void TransferQueue::setState(TransferTask* task, TaskState state) {
    task->state = state;
    if (state != TaskState::Running) {
        task->speedBps = 0.0;
    } else {
        task->lastBytes = task->bytesDone;
        task->lastTickMs = m_clock();
    }
}

void TransferQueue::markSpeed(TransferTask* task) {
    const qint64 now = m_clock();
    const qint64 span = now - task->lastTickMs;
    if (span <= 0) {
        return;
    }
    const quint64 delta = task->bytesDone > task->lastBytes
                              ? task->bytesDone - task->lastBytes
                              : 0;
    task->speedBps = static_cast<double>(delta) * 1000.0 / static_cast<double>(span);
    task->lastBytes = task->bytesDone;
    task->lastTickMs = now;
}

void TransferQueue::onLinkState(int state, const QString& detail) {
    Q_UNUSED(detail);
    m_linkUp = (state == 1);
}

void TransferQueue::fail(TransferTask* task, quint32 code, const QString& message) {
    task->errCode = code;
    task->errMsg = message;
    setState(task, TaskState::Failed);
    if (m_runningUploads > 0 && task->type == TaskType::Upload) {
        --m_runningUploads;
    }
    if (m_runningDownloads > 0 && task->type == TaskType::Download) {
        --m_runningDownloads;
    }
    touch(task->id, true);
    emit notice(QString::fromUtf8("%1 失败：%2").arg(task->remoteName, message), true);
    pump();
}

// 断线不是失败：保留已下块（.part/.part.state），重连后从 bitmap 的第一个缺块继续。
void TransferQueue::pauseForLink(TransferTask* task, const QString& message) {
    if (task == nullptr || !task->isActive()) {
        return;
    }
    task->errCode = 0;
    task->errMsg = message;
    if (task->type == TaskType::Upload && m_runningUploads > 0) {
        --m_runningUploads;
    }
    if (task->type == TaskType::Download && m_runningDownloads > 0) {
        --m_runningDownloads;
    }
    setState(task, TaskState::Paused);
    touch(task->id, true);
    emit notice(QString::fromUtf8("%1 已暂停：%2").arg(task->remoteName, message), true);
    if (m_linkUp) {
        pump();  // 链路还在（例如服务端返回错误）才让排队任务补位
    }
}

void TransferQueue::finish(TransferTask* task) {
    if (!task->instant) {
        task->bytesDone = task->size;  // 秒传没有真实字节流动，保持 0（契约 §3）
    }
    setState(task, TaskState::Done);
    if (m_runningUploads > 0 && task->type == TaskType::Upload) {
        --m_runningUploads;
    }
    if (m_runningDownloads > 0 && task->type == TaskType::Download) {
        --m_runningDownloads;
    }
    if (m_store != nullptr) {
        m_store->saveTask(*task);
        m_store->deleteBitmap(task->id);
    }
    emit notice(task->instant ? QString::fromUtf8("%1 已秒传").arg(task->remoteName)
                              : QString::fromUtf8("%1 已完成").arg(task->remoteName),
                false);
    if (task->instant) {
        emit instantUploaded(task->remoteName);
    }
    touch(task->id, false);
    pump();
}

int TransferQueue::enqueueUpload(const QString& localPath, const QString& remoteName) {
    TransferTask task;
    task.id = nextTaskId();
    task.type = TaskType::Upload;
    task.localPath = localPath;
    task.remoteName = remoteName.isEmpty() ? QFileInfo(localPath).fileName()
                                           : remoteName;
    task.size = static_cast<quint64>(QFileInfo(localPath).size());
    task.state = TaskState::Queued;
    m_tasks.append(task);
    touch(task.id, true);
    pump();
    return task.id;
}

int TransferQueue::enqueuePlaybackDownload(quint64 ufId, const QString& remoteName,
                                           quint64 size) {
    const int id = enqueueDownload(ufId, remoteName, size);
    TransferTask* task = find(id);
    if (task != nullptr) {
        task->playback = true;
    }
    return id;
}

int TransferQueue::enqueueSharedPlayback(const QString& shareTicket,
                                         const QString& fileName, quint64 size) {
    const int id = enqueueSharedDownload(shareTicket, fileName, size);
    TransferTask* task = find(id);
    if (task != nullptr) {
        task->playback = true;
    }
    return id;
}

int TransferQueue::enqueueSharedDownload(const QString& shareTicket,
                                         const QString& fileName, quint64 size) {
    TransferTask task;
    task.id = nextTaskId();
    task.type = TaskType::Download;
    task.ufId = 0;
    task.shareTicket = shareTicket;
    task.remoteName = fileName.isEmpty() ? QString::fromUtf8("shared_file.bin")
                                         : fileName;
    task.size = size;
    task.localPath = QDir::tempPath() + QString::fromUtf8("/") + task.remoteName;
    task.state = TaskState::Queued;
    m_tasks.append(task);
    touch(task.id, true);
    pump();
    return task.id;
}

int TransferQueue::enqueueDownload(quint64 ufId, const QString& remoteName,
                                   quint64 size) {
    TransferTask task;
    task.id = nextTaskId();
    task.type = TaskType::Download;
    task.ufId = ufId;
    task.remoteName = remoteName;
    task.size = size;
    // 默认落到系统临时目录；UI 后续接 QFileDialog 时改成用户选择路径。
    task.localPath = QDir::tempPath() + QString::fromUtf8("/") + remoteName;
    task.state = TaskState::Queued;
    m_tasks.append(task);
    touch(task.id, true);
    pump();
    return task.id;
}

void TransferQueue::pause(int id) {
    TransferTask* task = find(id);
    if (task == nullptr || !task->isActive()) {
        return;
    }
    if (task->type == TaskType::Upload && m_runningUploads > 0) {
        --m_runningUploads;
    }
    if (task->type == TaskType::Download && m_runningDownloads > 0) {
        --m_runningDownloads;
    }
    setState(task, TaskState::Paused);
    touch(id, true);
    pump();
}

void TransferQueue::resume(int id) {
    TransferTask* task = find(id);
    if (task == nullptr) {
        return;
    }
    if (task->state != TaskState::Paused && task->state != TaskState::Failed) {
        return;
    }
    task->errCode = 0;
    task->errMsg.clear();
    setState(task, TaskState::Queued);
    touch(id, true);
    pump();
}

void TransferQueue::cancel(int id) {
    TransferTask* task = find(id);
    if (task == nullptr || task->isTerminal()) {
        return;
    }
    if (task->state == TaskState::Running) {
        if (task->type == TaskType::Upload && m_runningUploads > 0) {
            --m_runningUploads;
        }
        if (task->type == TaskType::Download && m_runningDownloads > 0) {
            --m_runningDownloads;
        }
    }
    setState(task, TaskState::Canceled);
    touch(id, true);
    pump();
}

void TransferQueue::retry(int id) { resume(id); }

void TransferQueue::onLinkLost() {
    bool changedAny = false;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].isActive()) {
            m_tasks[i].state = TaskState::Paused;
            m_tasks[i].speedBps = 0.0;
            if (m_store != nullptr) {
                m_store->saveTask(m_tasks[i]);
                m_store->saveBitmap(m_tasks[i].id, m_tasks[i].bitmap);
            }
            changedAny = true;
        }
    }
    m_runningUploads = 0;
    m_runningDownloads = 0;
    m_pending.clear();
    if (changedAny) {
        emit notice(QString::fromUtf8("连接已断开，任务已暂停"), true);
        emit changed();
    }
}

void TransferQueue::pump() {
    for (int i = 0; i < m_tasks.size(); ++i) {
        TransferTask* task = &m_tasks[i];
        if (task->state != TaskState::Queued) {
            continue;
        }
        if (task->type == TaskType::Upload && m_runningUploads >= kMaxUploads) {
            continue;
        }
        if (task->type == TaskType::Download && m_runningDownloads >= kMaxDownloads) {
            continue;
        }
        if (task->type == TaskType::Upload) {
            ++m_runningUploads;
            startUpload(task);
        } else {
            ++m_runningDownloads;
            startDownload(task);
        }
        break;  // 一次只启动一个，避免同一轮把并发顶满后无法再排队
    }
}

// ---------------------------------------------------------------- 上传

void TransferQueue::startUpload(TransferTask* task) {
    // 先占住调度位（单测无网络时任务停在 Running，用于验证并发上限）。
    setState(task, TaskState::Running);
    task->nextChunk = 0;
    task->bytesDone = 0;
    if (m_net == nullptr) {
        touch(task->id, false);
        return;
    }
    QString md5;
    if (!fileMd5(task->localPath, &md5)) {
        fail(task, 0, QString::fromUtf8("读取本地文件失败"));
        return;
    }
    task->fileMd5 = md5;

    protocol::UploadInitRq rq;
    rq.parentId = 0;
    rq.fileName = task->remoteName.toStdString();
    rq.totalSize = task->size;
    rq.fileMd5 = md5.toStdString();
    rq.chunkSize = kBlockSize;
    rq.totalChunks = static_cast<std::uint32_t>((task->size + kBlockSize - 1) / kBlockSize);
    rq.flags = 1;
    // reqId 是服务端幂等键：不能只用任务号（每次进程启动都从 1 开始，会撞历史请求，
    // 导致复用已完成的 taskId，分片上报直接 3005）。用毫秒时间戳保唯一。
    rq.reqId = static_cast<std::uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF);
    const QByteArray payload = encodePayload(rq);
    if (payload.isEmpty()) {
        fail(task, 0, QString::fromUtf8("组包失败"));
        return;
    }
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::UploadInitRq), payload);
    Pending pending;
    pending.taskId = task->id;
    pending.kind = ReqUploadInit;
    m_pending[seq] = pending;
    touch(task->id, true);
}

void TransferQueue::sendNextChunk(TransferTask* task) {
    if (m_net == nullptr || task->state != TaskState::Running) {
        return;
    }
    if (task->nextChunk >= task->totalChunks) {
        protocol::UploadCompleteRq rq;
        rq.taskId = task->taskId.toStdString();
        const QByteArray payload = encodePayload(rq);
        const quint32 seq = m_net->sendRequest(t(protocol::MessageType::UploadCompleteRq), payload);
        Pending pending;
        pending.taskId = task->id;
        pending.kind = ReqUploadComplete;
        m_pending[seq] = pending;
        return;
    }

    QFile file(task->localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(task, 0, QString::fromUtf8("打开本地文件失败"));
        return;
    }
    const quint64 offset = static_cast<quint64>(task->nextChunk) * kBlockSize;
    if (!file.seek(static_cast<qint64>(offset))) {
        fail(task, 0, QString::fromUtf8("定位文件失败"));
        return;
    }
    const quint64 remain = task->size - offset;
    const int len = static_cast<int>(remain < kBlockSize ? remain : kBlockSize);
    const QByteArray data = file.read(len);
    if (data.size() != len) {
        fail(task, 0, QString::fromUtf8("读取分片失败"));
        return;
    }

    protocol::UploadChunkRq rq;
    rq.taskId = task->taskId.toStdString();
    rq.chunkIndex = task->nextChunk;
    rq.offset = offset;
    rq.chunkMd5 = cm::md5Hex(data.constData(), static_cast<std::size_t>(data.size()));
    rq.data = toStd(data);
    const QByteArray payload = encodePayload(rq);
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::UploadChunkRq), payload);
    Pending pending;
    pending.taskId = task->id;
    pending.kind = ReqUploadChunk;
    m_pending[seq] = pending;
}

void TransferQueue::handleUploadInit(TransferTask* task, const QByteArray& payload) {
    protocol::UploadInitRs rs;
    if (!rs.decode(toStd(payload))) {
        fail(task, 0, QString::fromUtf8("初始化响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        fail(task, rs.code, QString::fromUtf8("初始化被拒(code=%1)").arg(rs.code));
        return;
    }
    task->taskId = QString::fromStdString(rs.taskId);
    if (rs.result == 1) {
        task->instant = true;
        finish(task);
        return;
    }
    task->chunkSize = rs.chunkSize;
    task->totalChunks = rs.totalChunks;
    task->nextChunk = 0;
    sendNextChunk(task);
}

void TransferQueue::handleUploadChunk(TransferTask* task, const QByteArray& payload) {
    protocol::UploadChunkRs rs;
    if (!rs.decode(toStd(payload))) {
        fail(task, 0, QString::fromUtf8("分片响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        fail(task, rs.code, QString::fromUtf8("分片被拒(code=%1)").arg(rs.code));
        return;
    }
    const quint64 offset = static_cast<quint64>(task->nextChunk) * kBlockSize;
    const quint64 remain = task->size - offset;
    task->bytesDone += (remain < kBlockSize ? remain : kBlockSize);
    ++task->nextChunk;
    markSpeed(task);
    touch(task->id, true);
    sendNextChunk(task);
}

void TransferQueue::handleUploadComplete(TransferTask* task, const QByteArray& payload) {
    protocol::UploadCompleteRs rs;
    if (!rs.decode(toStd(payload))) {
        fail(task, 0, QString::fromUtf8("完成响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        fail(task, rs.code, QString::fromUtf8("合并失败(code=%1)").arg(rs.code));
        return;
    }
    if (!rs.badIndexes.empty()) {
        // 只重传坏片：把 nextChunk 拉回第一个坏片，其余保持已确认。
        task->nextChunk = rs.badIndexes[0];
        setState(task, TaskState::Retrying);
        emit notice(QString::fromUtf8("%1 有 %2 个坏片，重传中")
                        .arg(task->remoteName)
                        .arg(rs.badIndexes.size()),
                    true);
        setState(task, TaskState::Running);
        sendNextChunk(task);
        return;
    }
    finish(task);
}

// ---------------------------------------------------------------- 下载

QString TransferQueue::partPath(const TransferTask& task) const {
    return task.localPath + QString::fromUtf8(".part");
}

QString TransferQueue::partStatePath(const TransferTask& task) const {
    return task.localPath + QString::fromUtf8(".part.state");
}

void TransferQueue::savePartState(const TransferTask& task) {
    QFile file(partStatePath(task));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    QTextStream out(&file);
    out << "version=1\n";
    out << "size=" << task.size << "\n";
    out << "md5=" << task.fileMd5 << "\n";
    out << "block_size=" << task.blockSize << "\n";
    out << "block_count=" << task.blockCount << "\n";
    out << "bitmap=" << hexOf(task.bitmap) << "\n";
}

bool TransferQueue::loadPartState(TransferTask* task) {
    QFile file(partStatePath(*task));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray text = file.readAll();
    QByteArray bitmapHex;
    quint64 size = 0;
    QString md5;
    const QList<QByteArray> lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray& line = lines.at(i);
        const int eq = line.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QByteArray key = line.left(eq);
        const QByteArray value = line.mid(eq + 1).trimmed();
        if (key == "size") {
            size = value.toULongLong();
        } else if (key == "md5") {
            md5 = QString::fromLatin1(value);
        } else if (key == "bitmap") {
            bitmapHex = value;
        }
    }
    if (size == 0 || bitmapHex.isEmpty()) {
        return false;
    }
    task->size = size;
    task->fileMd5 = md5;
    task->bitmap = fromHex(QString::fromLatin1(bitmapHex));
    return true;
}

void TransferQueue::startDownload(TransferTask* task) {
    setState(task, TaskState::Running);
    task->blocksFetched = 0;  // 本次启动实际取回的块数（续传取证）
    if (m_net == nullptr) {
        touch(task->id, false);
        return;
    }
    // 分享下载没有本账号的 ufId，DownloadStatRq 用不了：直接 open（票据里有 fId）。
    if (task->shareTicket.isEmpty() && QFile::exists(partStatePath(*task)) &&
        loadPartState(task)) {
        protocol::DownloadStatRq rq;
        rq.ufId = task->ufId;
        const QByteArray payload = encodePayload(rq);
        const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DownloadStatRq), payload);
        Pending pending;
        pending.taskId = task->id;
        pending.kind = ReqDownloadStat;
        m_pending[seq] = pending;
        return;
    }
    requestDownloadOpen(task);
}

void TransferQueue::requestDownloadOpen(TransferTask* task) {
    protocol::DownloadOpenRq rq;
    rq.ufId = task->ufId;
    rq.shareTicket = task->shareTicket.toStdString();  // 分享下载时用票据替代 ufId
    rq.startBlock = 0;
    const QByteArray payload = encodePayload(rq);
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DownloadOpenRq), payload);
    Pending pending;
    pending.taskId = task->id;
    pending.kind = ReqDownloadOpen;
    m_pending[seq] = pending;
}

void TransferQueue::handleDownloadStat(TransferTask* task, const QByteArray& payload) {
    protocol::DownloadStatRs rs;
    if (!rs.decode(toStd(payload)) || rs.code != 0) {
        // 统计失败不阻塞：直接从头下载。
        task->bitmap.clear();
        requestDownloadOpen(task);
        return;
    }
    const bool same = rs.size == task->size &&
                      QString::fromStdString(rs.fileMd5) == task->fileMd5;
    if (!same) {
        task->bitmap.clear();
        emit notice(QString::fromUtf8("%1 文件已变更，将重新下载").arg(task->remoteName),
                    true);
    }
    requestDownloadOpen(task);
}

void TransferQueue::handleDownloadOpen(TransferTask* task, const QByteArray& payload) {
    protocol::DownloadOpenRs rs;
    if (!rs.decode(toStd(payload))) {
        fail(task, 0, QString::fromUtf8("打开下载解析失败"));
        return;
    }
    if (rs.code != 0) {
        fail(task, rs.code, QString::fromUtf8("打开下载被拒(code=%1)").arg(rs.code));
        return;
    }
    task->ticket = QString::fromStdString(rs.ticket);
    task->fId = rs.fId;
    task->fileMd5 = QString::fromStdString(rs.fileMd5);
    task->blockSize = rs.blockSize;
    task->blockCount = rs.blockCount;
    task->size = rs.size;
    bitmapResize(&task->bitmap, task->blockCount);
    task->nextBlock = firstMissing(task->bitmap, task->blockCount);
    const quint32 have = bitmapCount(task->bitmap, task->blockCount);
    task->bytesDone = static_cast<quint64>(have) * task->blockSize;
    if (task->bytesDone > task->size) {
        task->bytesDone = task->size;
    }
    savePartState(*task);
    if (have > 0) {
        // 续传取证：告诉用户这次到底取几块，而不是只报"完成"。
        emit notice(QString::fromUtf8("%1 续传：已下 %2/%3 块（%4 字节），本次只取 %5 块")
                        .arg(task->remoteName)
                        .arg(have)
                        .arg(task->blockCount)
                        .arg(static_cast<qulonglong>(task->bytesDone))
                        .arg(task->blockCount - have),
                    false);
    }

    if (task->nextBlock >= task->blockCount) {
        protocol::DownloadDoneRq done;
        done.ticket = task->ticket.toStdString();
        done.fId = task->fId;
        done.size = task->size;
        done.fileMd5 = task->fileMd5.toStdString();
        done.verifyOk = 1;
        const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DownloadDoneRq),
                                               encodePayload(done));
        Pending pending;
        pending.taskId = task->id;
        pending.kind = ReqDownloadDone;
        m_pending[seq] = pending;
        return;
    }
    sendNextBlock(task);
}

void TransferQueue::sendNextBlock(TransferTask* task) {
    if (m_net == nullptr || task->state != TaskState::Running) {
        return;
    }
    const quint32 index = firstMissing(task->bitmap, task->blockCount);
    if (index >= task->blockCount) {
        protocol::DownloadDoneRq done;
        done.ticket = task->ticket.toStdString();
        done.fId = task->fId;
        done.size = task->size;
        done.fileMd5 = task->fileMd5.toStdString();
        done.verifyOk = 1;
        const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DownloadDoneRq),
                                               encodePayload(done));
        Pending pending;
        pending.taskId = task->id;
        pending.kind = ReqDownloadDone;
        m_pending[seq] = pending;
        return;
    }
    task->nextBlock = index;
    protocol::DownloadRangeRq rq;
    rq.ticket = task->ticket.toStdString();
    rq.fId = task->fId;
    rq.blockIndex = index;
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DownloadRangeRq),
                                           encodePayload(rq));
    Pending pending;
    pending.taskId = task->id;
    pending.kind = ReqDownloadRange;
    m_pending[seq] = pending;
}

void TransferQueue::writePart(TransferTask* task, quint32 blockIndex,
                              const QByteArray& data) {
    const QString path = partPath(*task);
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return;
    }
    const qint64 offset = static_cast<qint64>(blockIndex) * task->blockSize;
    if (file.size() < offset + data.size()) {
        file.resize(offset + data.size());
    }
    file.seek(offset);
    file.write(data);
    file.close();
}

void TransferQueue::handleDownloadData(TransferTask* task, const QByteArray& payload) {
    const quint32 index = task->nextBlock;
    if (index >= task->blockCount) {
        return;
    }
    writePart(task, index, payload);
    bitmapSet(&task->bitmap, index);
    ++task->blocksFetched;
    task->bytesDone += static_cast<quint64>(payload.size());
    if (task->bytesDone > task->size) {
        task->bytesDone = task->size;
    }
    markSpeed(task);
    savePartState(*task);
    touch(task->id, true);
    sendNextBlock(task);
}

void TransferQueue::handleDownloadDone(TransferTask* task, const QByteArray& payload) {
    protocol::DownloadDoneRs rs;
    if (!rs.decode(toStd(payload)) || rs.code != 0) {
        fail(task, rs.code, QString::fromUtf8("完成上报失败"));
        return;
    }
    if (task->playback) {
        // 播放器正开着 .part：不能改名，保留文件让播放自然播到结尾
        QFile::remove(partStatePath(*task));
        task->bitmap.clear();
        finish(task);
        return;
    }
    QFile::remove(task->localPath);
    if (!QFile::rename(partPath(*task), task->localPath)) {
        fail(task, 0, QString::fromUtf8("重命名 .part 失败"));
        return;
    }
    QFile::remove(partStatePath(*task));
    task->bitmap.clear();
    finish(task);
}

// ---------------------------------------------------------------- 帧分发

void TransferQueue::onFrame(quint16 type, quint32 seq, const QByteArray& payload) {
    const std::map<quint32, Pending>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        // 10011（DownloadDataRs）沿用请求的 seq，正常能命中；命中不了就丢弃。
        return;
    }
    const Pending pending = it->second;
    TransferTask* task = find(pending.taskId);
    if (task == nullptr) {
        m_pending.erase(it);
        return;
    }
    const protocol::MessageType mt = static_cast<protocol::MessageType>(type);
    if (seq != 0) {
        m_pending.erase(it);
    }
    switch (mt) {
        case protocol::MessageType::UploadInitRs:
            handleUploadInit(task, payload);
            break;
        case protocol::MessageType::UploadChunkRs:
            handleUploadChunk(task, payload);
            break;
        case protocol::MessageType::UploadCompleteRs:
            handleUploadComplete(task, payload);
            break;
        case protocol::MessageType::DownloadStatRs:
            handleDownloadStat(task, payload);
            break;
        case protocol::MessageType::DownloadOpenRs:
            handleDownloadOpen(task, payload);
            break;
        case protocol::MessageType::DownloadDataRs:
            handleDownloadData(task, payload);
            break;
        case protocol::MessageType::DownloadDoneRs:
            handleDownloadDone(task, payload);
            break;
        case protocol::MessageType::ErrorRs: {
            protocol::Reader reader(toStd(payload));
            std::uint32_t code = 0;
            std::string message;
            if (!reader.readU32(&code) || !reader.readString(&message)) {
                code = 0;
            }
            fail(task, code, QString::fromUtf8("服务端拒绝(code=%1)").arg(code));
            break;
        }
        default:
            break;
    }
}

void TransferQueue::onTimeout(quint32 seq, quint16 type) {
    Q_UNUSED(type);
    const std::map<quint32, Pending>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        return;  // 该 seq 已由响应/断线终结，同一 seq 只允许一条终态路径
    }
    TransferTask* task = find(it->second.taskId);
    m_pending.erase(it);
    if (task == nullptr) {
        return;
    }
    if (!m_linkUp) {
        // 断线/重连期间到达的超时：优先按"暂停等待重连"处理，不判失败（契约 §3）。
        pauseForLink(task, QString::fromUtf8("连接超时，等待重连"));
        return;
    }
    fail(task, 0, QString::fromUtf8("请求超时(5s)"));
}

void TransferQueue::onAborted(quint32 seq, quint16 type, const QString& reason) {
    Q_UNUSED(type);
    const std::map<quint32, Pending>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        return;
    }
    TransferTask* task = find(it->second.taskId);
    m_pending.erase(it);
    pauseForLink(task, reason.isEmpty() ? QString::fromUtf8("连接已断开") : reason);
}

void TransferQueue::restoreFromStore() {
    if (m_store == nullptr) {
        return;
    }
    const QVector<TransferTask> saved = m_store->loadTasks();
    for (int i = 0; i < saved.size(); ++i) {
        TransferTask task = saved.at(i);
        if (task.state == TaskState::Done || task.state == TaskState::Canceled) {
            continue;
        }
        if (task.id >= m_nextId) {
            m_nextId = task.id + 1;
        }
        task.state = TaskState::Paused;  // 恢复出来先不自动跑，等用户点继续
        task.speedBps = 0.0;
        task.bitmap = m_store->loadBitmap(task.id);
        m_tasks.append(task);
    }
    if (!m_tasks.isEmpty()) {
        emit changed();
    }
}

}  // namespace transfer
}  // namespace cm
