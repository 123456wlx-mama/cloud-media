#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <map>

#include "netapi/FrameAssembler.h"

class QTcpSocket;
class QTimer;

namespace cm {
namespace netapi {

// 网络线程对象：持有 QTcpSocket，只在本线程做 IO。
// GUI 线程通过 NetClient 的 queued 调用进入，结果全部用信号回抛。
class NetWorker : public QObject {
    Q_OBJECT

public:
    explicit NetWorker(QObject* parent = nullptr);
    ~NetWorker() override;

public slots:
    // 必须在 moveToThread 之后、start 之前通过 queued 调用设置。
    void setEndpoint(const QString& host, quint16 port);
    void start();
    void stop();
    void loginRequest(quint32 seq, const QString& tel, const QString& password);
    void registerRequest(quint32 seq, const QString& tel, const QString& password,
                         const QString& name);
    // 目录分页：cursorKey/cursorId 为空/0 表示首页（ListFileRs 透传）。
    void listRequest(quint32 seq, quint64 parentId, quint32 limit,
                     const QString& cursorKey, quint64 cursorId);
    void addFolderRequest(quint32 seq, quint64 parentId, const QString& name);
    // 通用请求：transfer 层自己组 payload（复用 common/protocol 的结构体），netapi 只负责收发。
    void sendRequest(quint16 type, quint32 seq, QByteArray payload);

signals:
    // state: 0=连接中 1=已连接 2=重连中 3=失败
    void linkState(int state, const QString& detail);
    void loginFinished(quint32 seq, quint32 code, quint64 uid, const QString& name);
    void registerFinished(quint32 seq, quint32 code, quint64 uid);
    void listFinished(quint32 seq, quint32 code, const QVariantList& rows,
                      bool hasMore, const QString& nextKey, quint64 nextId);
    void addFolderFinished(quint32 seq, quint32 code, quint64 ufId,
                           const QString& name);
    // 链路正常但服务端未在 5s 内响应（可重试）。
    void requestTimeout(quint32 seq, quint16 type);
    // 连接断开导致在途请求中止（不是失败：契约 §3 断网→任务暂停）。
    void requestAborted(quint32 seq, quint16 type, const QString& reason);
    // 本地组包/写 socket 失败等，与被断开的请求无关。
    void requestFailed(quint32 seq, quint16 type, const QString& reason);
    // 未被 Login/List 消费的帧（10004/10005/10006/10007/10012/10013/10014/10040/10041…）。
    void frameReceived(quint16 type, quint32 seq, QByteArray payload);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onTick();
    void reconnectNow();

private:
    struct Pending {
        std::uint16_t type;
        qint64 deadlineMs;
        bool terminated;  // 同一 seq 只允许走一条终态路径
    };

    // 取出并标记终态；已终结/不存在返回 false，调用方直接 return。
    bool takePending(quint32 seq, Pending* out);
    void sendFrame(std::uint16_t type, std::uint32_t seq,
                   const std::string& payload);
    void failAll(const QString& reason);
    void scheduleReconnect();
    void dispatch(const protocol::Frame& frame);

    QTcpSocket* m_socket;
    QTimer* m_tick;
    // 未连接时先把已组好的帧缓存下来，连上后一次 flush（登录往往在 Connecting 阶段就发出）。
    QByteArray m_outbox;
    FrameAssembler m_assembler;
    std::map<quint32, Pending> m_pending;

    QString m_host;
    quint16 m_port;
    bool m_wantConnected;
    int m_attempt;
    int m_timeouts;
};

}  // namespace netapi
}  // namespace cm
