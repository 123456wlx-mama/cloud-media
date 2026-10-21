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
    void registerAccount(const QString& tel, const QString& password,
                         const QString& name);
    // cursorKey/cursorId 传空/0 取首页；hasMore 时用返回的 nextKey/nextId 取下一页。
    void listFiles(quint64 parentId, quint32 limit,
                   const QString& cursorKey = QString(), quint64 cursorId = 0);
    void addFolder(quint64 parentId, const QString& name);

    // 通用请求：返回本次分配的 seq，响应通过 frame(type, seq, payload) 回来。
    quint32 sendRequest(quint16 type, const QByteArray& payload);

    quint64 uid() const { return m_uid; }
    QString userName() const { return m_userName; }
    // 最近一次链路状态（0连接中 1已连接 2重连中 3失败）。
    // 界面晚于 connectTo 创建时，用它补一次初始状态，避免徽标一直停在"连接中"。
    int linkStateValue() const { return m_linkState; }
    // 最近一次分配的 seq（诊断/测试用）。
    quint32 lastSeq() const { return m_seq; }

signals:
    void linkState(int state, const QString& detail);
    void loginFinished(quint32 code, quint64 uid, const QString& name);
    void registerFinished(quint32 code, quint64 uid);
    void listFinished(quint32 code, const QVariantList& rows, bool hasMore,
                      const QString& nextKey, quint64 nextId);
    void addFolderFinished(quint32 code, quint64 ufId, const QString& name);
    void requestFailed(quint16 type, const QString& reason);
    void frame(quint16 type, quint32 seq, const QByteArray& payload);
    // 链路正常但 5s 无响应：由上层决定是否重试。
    void requestTimeout(quint32 seq, quint16 type);
    // 连接断开中止在途请求：上层应把任务转"暂停"，不是"失败"。
    void requestAborted(quint32 seq, quint16 type, const QString& reason);

private slots:
    void onLinkState(int state, const QString& detail);
    void onFrame(quint16 type, quint32 seq, QByteArray payload);
    void onLoginFinished(quint32 seq, quint32 code, quint64 uid, const QString& name);
    void onRegisterFinished(quint32 seq, quint32 code, quint64 uid);
    void onListFinished(quint32 seq, quint32 code, const QVariantList& rows,
                        bool hasMore, const QString& nextKey, quint64 nextId);
    void onAddFolderFinished(quint32 seq, quint32 code, quint64 ufId,
                             const QString& name);
    void onRequestTimeout(quint32 seq, quint16 type);
    void onRequestAborted(quint32 seq, quint16 type, const QString& reason);
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
    int m_linkState;
};

}  // namespace netapi
}  // namespace cm
