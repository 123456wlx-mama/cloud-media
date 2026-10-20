#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace cm {

namespace netapi {
class NetClient;
}

namespace shareapi {

// 分享状态机（契约 §2.1：创建 → 展示码 → 打开 → 下载）。
// 只做协议收发与状态流转；弹窗、剪贴板、Toast 都在 ui/。
// 单测：net 传 nullptr，直接喂 applyXxxRs 的 payload。
enum class SharePhase {
    Idle = 0,
    Creating,
    Created,
    Opening,
    Opened,
    Cancelled,
    Failed
};

struct ShareInfo {
    SharePhase phase;
    quint32 code;          // 最近一次响应码
    QString message;       // 失败文案 / 成功摘要
    QString shareCode;
    QString accessCode;    // 创建时服务端生成的提取码（可能为空）
    bool accessCodeRequired;
    quint64 expireAtMs;    // 0=永久
    quint64 ufId;
    quint64 size;
    QString fileName;
    QString shareTicket;   // ShareOpenRs 返回，交给传输区下载

    ShareInfo();
};

class ShareFlow : public QObject {
    Q_OBJECT

public:
    explicit ShareFlow(netapi::NetClient* net, QObject* parent = nullptr);

    // expirePolicy: 0=永久 1=1天 2=7天；accessMode: 0=无提取码 1=生成 4 位。
    void create(quint64 ufId, quint8 expirePolicy = 2, quint8 accessMode = 1);
    void openShare(const QString& shareCode, const QString& accessCode);
    void cancel(const QString& shareCode);
    void reset();

    const ShareInfo& info() const { return m_info; }
    bool busy() const;

    // 纯函数，便于单测与 UI 复用。
    static QString normalizeCode(const QString& text);   // 去空格 + 大写
    static QString errorText(quint32 code);              // 2003/2004 → 契约 §2.2 文案
    static QString expiryText(quint64 expireAtMs);

    // 响应入口（onFrame 与单测共用）。
    void applyCreateRs(const QByteArray& payload);
    void applyOpenRs(const QByteArray& payload);
    void applyCancelRs(const QByteArray& payload);
    void applyErrorRs(quint32 code, const QString& message);

signals:
    void changed();
    void created();
    void opened();
    void cancelled(const QString& shareCode);
    void failed(quint32 code, const QString& text);

private slots:
    void onFrame(quint16 type, quint32 seq, const QByteArray& payload);
    void onTimeout(quint32 seq, quint16 type);
    void onAborted(quint32 seq, quint16 type, const QString& reason);

private:
    enum Op { OpNone = 0, OpCreate, OpOpen, OpCancel };

    void setPhase(SharePhase phase, quint32 code, const QString& message);
    void failWith(quint32 code, const QString& text);
    bool busyOp() const;

    netapi::NetClient* m_net;
    ShareInfo m_info;
    quint32 m_seq;
    Op m_op;
};

}  // namespace shareapi
}  // namespace cm
