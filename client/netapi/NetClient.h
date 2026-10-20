#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

class QThread;

namespace cm {
namespace netapi {

class NetWorker;

// GUI 线程侧的门面：所有跨线程调用都是 queued，结果用信号回抛。
// 断线重连后如果之前登录过，会自动用缓存的凭据重登（对 UI 透明）。
class NetClient : public QObject {
    Q_OBJECT

public:
    explicit NetClient(QObject* parent = nullptr);
    ~NetClient() override;

    void connectTo(const QString& host, quint16 port);
    void shutdown();

    void login(const QString& tel, const QString& password);
    void listFiles(quint64 parentId, quint32 limit);

    quint64 uid() const { return m_uid; }
    QString userName() const { return m_userName; }

signals:
    void linkState(int state, const QString& detail);
    void loginFinished(quint32 code, quint64 uid, const QString& name);
    void listFinished(quint32 code, const QVariantList& rows);
    void requestFailed(quint16 type, const QString& reason);

private slots:
    void onLinkState(int state, const QString& detail);
    void onLoginFinished(quint32 seq, quint32 code, quint64 uid, const QString& name);
    void onListFinished(quint32 seq, quint32 code, const QVariantList& rows);
    void onRequestFailed(quint32 seq, quint16 type, const QString& reason);

private:
    quint32 nextSeq();

    QThread* m_thread;
    NetWorker* m_worker;
    quint32 m_seq;

    QString m_host;
    quint16 m_port;
    QString m_tel;
    QString m_password;
    bool m_haveCreds;
    quint64 m_uid;
    QString m_userName;
};

}  // namespace netapi
}  // namespace cm
