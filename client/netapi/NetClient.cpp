#include "netapi/NetClient.h"

#include <QMetaObject>
#include <QThread>

#include "netapi/NetWorker.h"

namespace cm {
namespace netapi {

NetClient::NetClient(QObject* parent)
    : QObject(parent), m_thread(nullptr), m_worker(nullptr), m_seq(0),
      m_port(0), m_haveCreds(false), m_uid(0) {
    m_thread = new QThread(this);
    m_worker = new NetWorker();
    m_worker->moveToThread(m_thread);

    connect(m_thread, SIGNAL(finished()), m_worker, SLOT(deleteLater()));
    connect(m_worker, SIGNAL(linkState(int, QString)), this,
            SLOT(onLinkState(int, QString)));
    connect(m_worker, SIGNAL(loginFinished(quint32, quint32, quint64, QString)),
            this, SLOT(onLoginFinished(quint32, quint32, quint64, QString)));
    connect(m_worker, SIGNAL(listFinished(quint32, quint32, QVariantList)), this,
            SLOT(onListFinished(quint32, quint32, QVariantList)));
    connect(m_worker, SIGNAL(requestTimeout(quint32, quint16)), this,
            SLOT(onRequestTimeout(quint32, quint16)));
    connect(m_worker, SIGNAL(requestAborted(quint32, quint16, QString)), this,
            SLOT(onRequestAborted(quint32, quint16, QString)));
    connect(m_worker, SIGNAL(requestFailed(quint32, quint16, QString)), this,
            SLOT(onRequestFailed(quint32, quint16, QString)));
    connect(m_worker, SIGNAL(frameReceived(quint16, quint32, QByteArray)), this,
            SLOT(onFrame(quint16, quint32, QByteArray)));

    m_thread->start();
}

NetClient::~NetClient() {
    shutdown();
    m_thread->quit();
    m_thread->wait(3000);
}

void NetClient::shutdown() {
    QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
}

void NetClient::connectTo(const QString& host, quint16 port) {
    m_host = host;
    m_port = port;
    QMetaObject::invokeMethod(m_worker, "setEndpoint", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, host), Q_ARG(quint16, port));
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

quint32 NetClient::nextSeq() {
    ++m_seq;
    if (m_seq == 0) {
        ++m_seq;
    }
    return m_seq;
}

void NetClient::login(const QString& tel, const QString& password) {
    m_tel = tel;
    m_password = password;
    m_haveCreds = true;
    QMetaObject::invokeMethod(m_worker, "loginRequest", Qt::QueuedConnection,
                              Q_ARG(quint32, nextSeq()), Q_ARG(QString, tel),
                              Q_ARG(QString, password));
}

void NetClient::listFiles(quint64 parentId, quint32 limit) {
    QMetaObject::invokeMethod(m_worker, "listRequest", Qt::QueuedConnection,
                              Q_ARG(quint32, nextSeq()),
                              Q_ARG(quint64, static_cast<quint64>(parentId)),
                              Q_ARG(quint32, limit));
}

quint32 NetClient::sendRequest(quint16 type, const QByteArray& payload) {
    const quint32 seq = nextSeq();
    QMetaObject::invokeMethod(m_worker, "sendRequest", Qt::QueuedConnection,
                              Q_ARG(quint16, type), Q_ARG(quint32, seq),
                              Q_ARG(QByteArray, payload));
    return seq;
}

void NetClient::onLinkState(int state, const QString& detail) {
    emit linkState(state, detail);
    // 重连成功且之前登录过：自动重登，UI 无感（契约 §3 断线重连）。
    if (state == 1 && m_haveCreds && m_uid != 0) {
        QMetaObject::invokeMethod(m_worker, "loginRequest", Qt::QueuedConnection,
                                  Q_ARG(quint32, nextSeq()), Q_ARG(QString, m_tel),
                                  Q_ARG(QString, m_password));
    }
}

void NetClient::onLoginFinished(quint32 seq, quint32 code, quint64 uid,
                                const QString& name) {
    Q_UNUSED(seq);
    if (code == 0) {
        m_uid = uid;
        m_userName = name;
    }
    emit loginFinished(code, uid, name);
}

void NetClient::onListFinished(quint32 seq, quint32 code,
                               const QVariantList& rows) {
    Q_UNUSED(seq);
    emit listFinished(code, rows);
}

void NetClient::onRequestTimeout(quint32 seq, quint16 type) {
    emit requestTimeout(seq, type);
}

void NetClient::onRequestAborted(quint32 seq, quint16 type,
                                 const QString& reason) {
    emit requestAborted(seq, type, reason);
}

void NetClient::onRequestFailed(quint32 seq, quint16 type,
                                const QString& reason) {
    Q_UNUSED(seq);
    // 同一 seq 已由 timeout/aborted 终结过的不再补发失败（每个 seq 只有一条终态路径）。
    emit requestFailed(type, reason);
}

void NetClient::onFrame(quint16 type, quint32 seq, QByteArray payload) {
    emit frame(type, seq, payload);
}

}  // namespace netapi
}  // namespace cm
