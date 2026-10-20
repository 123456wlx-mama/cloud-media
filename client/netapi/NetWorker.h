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
    void listRequest(quint32 seq, quint64 parentId, quint32 limit);

signals:
    // state: 0=连接中 1=已连接 2=重连中 3=失败
    void linkState(int state, const QString& detail);
    void loginFinished(quint32 seq, quint32 code, quint64 uid, const QString& name);
    void listFinished(quint32 seq, quint32 code, const QVariantList& rows);
    void requestFailed(quint32 seq, quint16 type, const QString& reason);

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
    };

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
