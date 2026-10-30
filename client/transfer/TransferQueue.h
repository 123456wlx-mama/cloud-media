#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <map>

#include "transfer/TransferTask.h"

namespace cm {

namespace netapi {
class NetClient;
}

namespace sqlapi {
class TaskStore;
}

namespace transfer {

// 边下边播的缓存目录：%TEMP%/cloud-media-cache（不存在则创建）。
// 单独成目录是为了让清理策略只碰自己的文件。
QString playbackCacheRoot();

// 清理播放缓存：超过 maxAgeDays 天的先删，总量仍超过 maxBytes 时按最旧的继续删。
// 返回删除的缓存文件数（.part 计一个，配套 .state 跟随删除不计）。
int prunePlaybackCache(const QString& root, int maxAgeDays, qint64 maxBytes);

// 传输队列：上传/下载的调度与状态机。
// 只通过 NetClient 发协议帧（不自己开 socket），响应按 seq 关联回任务。
// 单测：net 可传 nullptr（任务停在队列里），时钟可注入。
class TransferQueue : public QObject {
    Q_OBJECT

public:
    TransferQueue(netapi::NetClient* net, sqlapi::TaskStore* store,
                  QObject* parent = nullptr);

    // 单测用：替换时钟（默认 QDateTime::currentMSecsSinceEpoch）。
    void setClock(std::function<qint64()> clock);

    // parentId：上传目标目录（0 = 根）。上传文件夹时由 FolderUploader 解析出每级目录 ufId。
    int enqueueUpload(const QString& localPath, const QString& remoteName,
                      quint64 parentId = 0);
    // transient=true：播放用的临时下载，落系统临时目录（不污染用户「下载」文件夹）。
    int enqueueDownload(quint64 ufId, const QString& remoteName, quint64 size,
                        bool transient = false);
    // 分享下载：先 ShareOpenRq 拿 shareTicket，再走同一套下载链路（ufId=0）。
    int enqueueSharedDownload(const QString& shareTicket, const QString& fileName,
                              quint64 size, bool transient = false);
    // 分享 + 边下边播：完成后保留 .part 供播放器继续读。
    int enqueueSharedPlayback(const QString& shareTicket, const QString& fileName,
                              quint64 size);
    // 边下边播：下到 <localPath>.part，完成后不重命名（播放器还开着这个文件）。
    int enqueuePlaybackDownload(quint64 ufId, const QString& remoteName,
                                quint64 size);

    void pause(int id);
    void resume(int id);
    void cancel(int id);
    void retry(int id);

    const QVector<TransferTask>& tasks() const { return m_tasks; }
    TransferTask* find(int id);
    // 该目录下还没结束（排队/传输/暂停/重试）的上传任务数。
    // 删文件夹前用它提示「里面还有 N 个文件正在上传」，避免用户误删导致任务失败。
    int activeUploadsIn(quint64 parentId) const;

    // 登录成功后调用：切换当前账号。会把内存里上一个账号的任务全部丢弃，
    // 再按 uid 从 SQLite 恢复该账号未完成的任务。uid 相同时是 no-op。
    void setCurrentUser(quint64 uid);

    // 按当前账号从 SQLite 恢复未完成任务（Paused/Queued/Failed）。
    // 未设置账号（uid==0，例如登录前）时不做任何事。
    void restoreFromStore();

    // 断线：所有运行中任务转 Paused（等重连后用户点继续）。
    void onLinkLost();

signals:
    void changed();
    void notice(const QString& text, bool error);
    // 秒传命中：UI 弹 Toast「已秒传 · 0 字节」（契约 §3 必须显式提示）
    void instantUploaded(const QString& remoteName);

private slots:
    // 后台线程算完整文件 MD5 后回到主线程继续（任务可能已被取消/暂停）
    void onHashReady(int taskId, bool ok, const QString& hex);

    void onFrame(quint16 type, quint32 seq, const QByteArray& payload);
    // 链路正常但服务端 5s 未响应：失败（可重试）。
    void onTimeout(quint32 seq, quint16 type);
    // 连接断开导致在途请求中止：转暂停，等重连后继续（契约 §3）。
    void onAborted(quint32 seq, quint16 type, const QString& reason);
    void onLinkState(int state, const QString& detail);

private:
    enum RequestKind {
        ReqUploadInit = 1,
        ReqContinueUpload,
        ReqUploadChunk,
        ReqUploadComplete,
        ReqDownloadStat,
        ReqDownloadOpen,
        ReqDownloadRange,
        ReqDownloadDone
    };
    struct Pending {
        int taskId;
        RequestKind kind;
    };

    int nextTaskId();
    void pump();
    // 摘掉队列里第 index 条任务（连同 SQLite 残留），只用于播放任务的去重
    void dropTask(int index);
    // 下载任务的唯一构造入口：playback 必须在启动前定好（startDownload 靠它走缓存快判）
    int addDownloadTask(quint64 ufId, const QString& shareTicket,
                        const QString& remoteName, quint64 size, bool transient,
                        bool playback);
    void touch(int taskId, bool persist);
    void setState(TransferTask* task, TaskState state);
    void markSpeed(TransferTask* task);
    void fail(TransferTask* task, quint32 code, const QString& message);
    void pauseForLink(TransferTask* task, const QString& message);
    // notify=false：命中本地完整缓存时静默完成，不弹「已完成」提示
    void finish(TransferTask* task, bool notify = true);

    void startUpload(TransferTask* task);
    // MD5 就绪后组 UploadInitRq 并发出
    void sendUploadInit(TransferTask* task);
    // 暂停后继续：先问服务端「已经收到哪些分片」（10028/10029），再补缺口。
    // 不能重新 UploadInit —— 服务端每次 init 都会新建任务，等于从第 0 片重来。
    void sendContinueUpload(TransferTask* task);
    void sendNextChunk(TransferTask* task);
    void startDownload(TransferTask* task);
    void requestDownloadOpen(TransferTask* task);
    void sendNextBlock(TransferTask* task);
    // 返回 false 表示这一块没有真正落盘（打开/扩容/seek/短写失败）。
    // 调用方绝不能在这种情况下置位图，否则续传会跳过坏块。
    bool writePart(TransferTask* task, quint32 blockIndex, const QByteArray& data);

    void* taskPtr(int taskId);
    void handleUploadInit(TransferTask* task, const QByteArray& payload);
    void handleContinueUpload(TransferTask* task, const QByteArray& payload);
    void handleUploadChunk(TransferTask* task, const QByteArray& payload);
    void handleUploadComplete(TransferTask* task, const QByteArray& payload);
    void handleDownloadStat(TransferTask* task, const QByteArray& payload);
    void handleDownloadOpen(TransferTask* task, const QByteArray& payload);
    void handleDownloadData(TransferTask* task, const QByteArray& payload);
    void handleDownloadDone(TransferTask* task, const QByteArray& payload);

    QString partPath(const TransferTask& task) const;
    QString partStatePath(const TransferTask& task) const;
    void savePartState(const TransferTask& task);
    bool loadPartState(TransferTask* task);

    netapi::NetClient* m_net;
    sqlapi::TaskStore* m_store;
    quint64 m_uid;  // 当前登录账号；本地任务缓存按它隔离
    QVector<TransferTask> m_tasks;
    std::map<quint32, Pending> m_pending;
    int m_seq;
    int m_nextId;
    int m_runningUploads;
    int m_runningDownloads;
    bool m_linkUp;
    std::function<qint64()> m_clock;
};

}  // namespace transfer
}  // namespace cm
