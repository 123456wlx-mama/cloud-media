#include "netapi/NetWorker.h"

#include <QDateTime>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>

#include <string>
#include <vector>

#include "cm/protocol/codec.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/auth.h"
#include "cm/protocol/messages/list.h"
#include "cm/protocol/serialize.h"

namespace cm {
namespace netapi {
namespace {

const int kRequestTimeoutMs = 5000;
const int kTickMs = 500;
const int kMaxReconnect = 5;
const int kTimeoutBeforeReconnect = 2;

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

std::uint16_t toType(protocol::MessageType type) {
    return static_cast<std::uint16_t>(type);
}

}  // namespace

NetWorker::NetWorker(QObject* parent)
    : QObject(parent), m_socket(nullptr), m_tick(nullptr), m_port(0),
      m_wantConnected(false), m_attempt(0), m_timeouts(0) {
    m_socket = new QTcpSocket(this);
    // 自研二进制协议绝不走系统代理：Windows 上系统代理/ PAC 会让 QTcpSocket 直接失败
    // （errorString: "The proxy type is invalid for this operation"）。
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);

    connect(m_socket, SIGNAL(connected()), this, SLOT(onConnected()));
    connect(m_socket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
    connect(m_socket, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)), this,
            SLOT(onSocketError(QAbstractSocket::SocketError)));
    connect(m_tick, SIGNAL(timeout()), this, SLOT(onTick()));
}

NetWorker::~NetWorker() {}

void NetWorker::setEndpoint(const QString& host, quint16 port) {
    m_host = host;
    m_port = port;
}

void NetWorker::start() {
    m_wantConnected = true;
    m_attempt = 0;
    m_timeouts = 0;
    emit linkState(0, QString::fromUtf8("连接中…"));
    m_socket->connectToHost(m_host, m_port);
    m_tick->start();
}

void NetWorker::stop() {
    m_wantConnected = false;
    m_tick->stop();
    if (m_socket != nullptr) {
        m_socket->abort();
    }
}

void NetWorker::onConnected() {
    m_attempt = 0;
    m_timeouts = 0;
    m_assembler.reset();
    emit linkState(1, QString::fromUtf8("已连接"));
    if (!m_outbox.isEmpty()) {
        m_socket->write(m_outbox);
        m_outbox.clear();
    }
}

void NetWorker::onDisconnected() {
    m_outbox.clear();
    failAll(QString::fromUtf8("连接已断开"));
    if (m_wantConnected) {
        scheduleReconnect();
    }
}

void NetWorker::onSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    if (!m_wantConnected) {
        return;
    }
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }
}

void NetWorker::scheduleReconnect() {
    if (!m_wantConnected) {
        return;
    }
    if (m_attempt >= kMaxReconnect) {
        emit linkState(3, QString::fromUtf8("连接失败，请检查网络后重试"));
        m_wantConnected = false;
        return;
    }
    ++m_attempt;
    int delaySec = 1 << (m_attempt - 1);
    if (delaySec > 8) {
        delaySec = 8;
    }
    emit linkState(2, QString::fromUtf8("重连中(%1/%2) %3")
                          .arg(m_attempt)
                          .arg(kMaxReconnect)
                          .arg(m_socket->errorString()));
    QTimer::singleShot(delaySec * 1000, this, SLOT(reconnectNow()));
}

void NetWorker::reconnectNow() {
    if (!m_wantConnected) {
        return;
    }
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_socket->connectToHost(m_host, m_port);
}

void NetWorker::onTick() {
    const qint64 now = nowMs();
    std::vector<quint32> expired;
    for (std::map<quint32, Pending>::const_iterator it = m_pending.begin();
         it != m_pending.end(); ++it) {
        if (now >= it->second.deadlineMs) {
            expired.push_back(it->first);
        }
    }
    for (std::size_t i = 0; i < expired.size(); ++i) {
        const quint32 seq = expired[i];
        const Pending pending = m_pending[seq];
        m_pending.erase(seq);
        emit requestFailed(seq, pending.type, QString::fromUtf8("请求超时(5s)"));
        ++m_timeouts;
    }
    // 连续超时说明链路已僵死（TCP 还没报 RST）：主动断开触发重连。
    if (m_timeouts >= kTimeoutBeforeReconnect) {
        m_timeouts = 0;
        m_socket->abort();
    }
}

void NetWorker::sendFrame(std::uint16_t type, std::uint32_t seq,
                          const std::string& payload) {
    std::string frame;
    if (protocol::Codec::encode(type, seq, payload, &frame) != ErrorCode::Ok) {
        emit requestFailed(seq, type, QString::fromUtf8("组帧失败"));
        return;
    }
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        if (m_socket->write(frame.data(), static_cast<qint64>(frame.size())) < 0) {
            emit requestFailed(seq, type, QString::fromUtf8("写入 socket 失败"));
            return;
        }
    } else {
        m_outbox.append(frame.data(), static_cast<int>(frame.size()));
    }
    Pending pending;
    pending.type = type;
    pending.deadlineMs = nowMs() + kRequestTimeoutMs;
    m_pending[seq] = pending;
}

void NetWorker::loginRequest(quint32 seq, const QString& tel,
                             const QString& password) {
    protocol::LoginRq rq;
    rq.tel = tel.toStdString();
    rq.password = password.toStdString();
    std::string payload;
    if (!rq.encode(&payload)) {
        emit requestFailed(seq, toType(protocol::MessageType::LoginRq),
                           QString::fromUtf8("手机号/密码格式不合法"));
        return;
    }
    sendFrame(toType(protocol::MessageType::LoginRq), seq, payload);
}

void NetWorker::listRequest(quint32 seq, quint64 parentId, quint32 limit) {
    protocol::ListFileRq rq;
    rq.parentId = parentId;
    rq.sort = 1;  // 创建时间倒序，与契约 §2.1 的"最近上传"一致
    rq.limit = limit;
    std::string payload;
    if (!rq.encode(&payload)) {
        emit requestFailed(seq, toType(protocol::MessageType::ListFileRq),
                           QString::fromUtf8("列表参数不合法"));
        return;
    }
    sendFrame(toType(protocol::MessageType::ListFileRq), seq, payload);
}

void NetWorker::onReadyRead() {
    const QByteArray chunk = m_socket->readAll();
    std::vector<protocol::Frame> frames;
    if (!m_assembler.feed(chunk.constData(),
                          static_cast<std::size_t>(chunk.size()), &frames)) {
        emit requestFailed(0, 0, QString::fromUtf8("收到坏帧，断开重连"));
        m_socket->abort();
        return;
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        dispatch(frames[i]);
    }
}

void NetWorker::dispatch(const protocol::Frame& frame) {
    if (frame.type == toType(protocol::MessageType::ErrorRs)) {
        protocol::Reader reader(frame.payload);
        std::uint32_t code = 0;
        std::string message;
        if (!reader.readU32(&code) || !reader.readString(&message)) {
            emit requestFailed(frame.seq, 0, QString::fromUtf8("错误帧解析失败"));
            return;
        }
        const std::map<quint32, Pending>::iterator it = m_pending.find(frame.seq);
        const std::uint16_t origType = (it == m_pending.end()) ? 0 : it->second.type;
        if (it != m_pending.end()) {
            m_pending.erase(it);
        }
        if (origType == toType(protocol::MessageType::LoginRq)) {
            emit loginFinished(frame.seq, code, 0, QString());
            return;
        }
        if (origType == toType(protocol::MessageType::ListFileRq)) {
            emit listFinished(frame.seq, code, QVariantList());
            return;
        }
        emit requestFailed(frame.seq, origType,
                           QString::fromStdString(message).isEmpty()
                               ? QString::fromUtf8("服务端拒绝")
                               : QString::fromStdString(message));
        return;
    }

    if (frame.type == toType(protocol::MessageType::LoginRs)) {
        protocol::LoginRs rs;
        if (!rs.decode(frame.payload)) {
            emit requestFailed(frame.seq, frame.type, QString::fromUtf8("登录响应解析失败"));
            return;
        }
        m_pending.erase(frame.seq);
        emit loginFinished(frame.seq, rs.code, rs.uid,
                           QString::fromStdString(rs.name));
        return;
    }

    if (frame.type == toType(protocol::MessageType::ListFileRs)) {
        protocol::ListFileRs rs;
        if (!rs.decode(frame.payload)) {
            emit requestFailed(frame.seq, frame.type, QString::fromUtf8("列表响应解析失败"));
            return;
        }
        m_pending.erase(frame.seq);
        QVariantList rows;
        for (std::size_t i = 0; i < rs.items.size(); ++i) {
            const protocol::FileItem& item = rs.items[i];
            QVariantMap row;
            row.insert(QString::fromUtf8("ufId"),
                       QVariant::fromValue<qulonglong>(item.ufId));
            row.insert(QString::fromUtf8("name"),
                       QString::fromStdString(item.name));
            row.insert(QString::fromUtf8("isDir"), item.isDir != 0);
            row.insert(QString::fromUtf8("size"),
                       QVariant::fromValue<qulonglong>(item.size));
            row.insert(QString::fromUtf8("updatedAtMs"),
                       QVariant::fromValue<qulonglong>(item.updatedAtMs));
            rows.append(row);
        }
        emit listFinished(frame.seq, rs.code, rows);
        return;
    }

    // 本阶段没订阅的帧（心跳等）：直接丢弃，不当作错误。
    m_pending.erase(frame.seq);
}

void NetWorker::failAll(const QString& reason) {
    for (std::map<quint32, Pending>::const_iterator it = m_pending.begin();
         it != m_pending.end(); ++it) {
        emit requestFailed(it->first, it->second.type, reason);
    }
    m_pending.clear();
}

}  // namespace netapi
}  // namespace cm
