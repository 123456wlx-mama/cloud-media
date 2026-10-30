#include "shareapi/ShareFlow.h"

#include <QDateTime>

#include <string>

#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/share.h"
#include "cm/protocol/serialize.h"
#include "netapi/NetClient.h"

namespace cm {
namespace shareapi {
namespace {

const QString kPendingText = QString::fromUtf8("处理中…");

quint16 t(protocol::MessageType type) {
    return static_cast<std::uint16_t>(type);
}

std::string toStd(const QByteArray& data) {
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

QByteArray toBytes(const std::string& data) {
    return QByteArray(data.data(), static_cast<int>(data.size()));
}

}  // namespace

ShareInfo::ShareInfo()
    : phase(SharePhase::Idle), code(0), accessCodeRequired(false),
      expireAtMs(0), ufId(0), size(0) {}

ShareFlow::ShareFlow(netapi::NetClient* net, QObject* parent)
    : QObject(parent), m_net(net), m_seq(0), m_op(OpNone) {
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(frame(quint16, quint32, QByteArray)), this,
                SLOT(onFrame(quint16, quint32, QByteArray)));
        connect(m_net, SIGNAL(requestTimeout(quint32, quint16)), this,
                SLOT(onTimeout(quint32, quint16)));
        connect(m_net, SIGNAL(requestAborted(quint32, quint16, QString)), this,
                SLOT(onAborted(quint32, quint16, QString)));
    }
}

QString ShareFlow::normalizeCode(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch.isSpace()) {
            continue;
        }
        out.append(ch.toUpper());
    }
    return out;
}

QString ShareFlow::errorText(quint32 code) {
    switch (code) {
        case 0:
            return QString::fromUtf8("成功");
        case 1001:
            return QString::fromUtf8("分享参数不合法");
        case 1002:
            return QString::fromUtf8("分享不存在或已被取消");
        case 2002:
            return QString::fromUtf8("登录已过期，请重新登录");
        case 2003:
            return QString::fromUtf8("分享已过期");  // 契约 §2.2
        case 2004:
            return QString::fromUtf8("提取码错误");  // 契约 §2.2
        case 4008:
            return QString::fromUtf8("服务暂不可用，请稍后重试");
        default:
            break;
    }
    return QString::fromUtf8("操作失败（code=%1）").arg(code);
}

QString ShareFlow::expiryText(quint64 expireAtMs) {
    if (expireAtMs == 0) {
        return QString::fromUtf8("永久有效");
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(expireAtMs))
        .toString(QString::fromUtf8("yyyy-MM-dd HH:mm"));
}

bool ShareFlow::busyOp() const {
    return m_info.phase == SharePhase::Creating ||
           m_info.phase == SharePhase::Opening || m_info.phase == SharePhase::Saving;
}

bool ShareFlow::busy() const { return busyOp(); }

void ShareFlow::reset() {
    m_info = ShareInfo();
    m_op = OpNone;
    m_seq = 0;
    emit changed();
}

void ShareFlow::create(quint64 ufId, quint8 expirePolicy, quint8 accessMode) {
    if (busyOp()) {
        return;
    }
    m_info = ShareInfo();
    m_info.ufId = ufId;

    protocol::ShareCreateRq rq;
    rq.ufId = ufId;
    rq.expirePolicy = expirePolicy;
    rq.accessMode = accessMode;
    rq.reqId = static_cast<std::uint32_t>(ufId & 0xFFFFFFFFULL);
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("分享参数不合法"));
        return;
    }
    setPhase(SharePhase::Creating, 0, kPendingText);
    m_op = OpCreate;
    m_seq = 0;
    if (m_net == nullptr) {
        return;  // 单测：停在 Creating，等 applyCreateRs
    }
    m_seq = m_net->sendRequest(t(protocol::MessageType::ShareCreateRq),
                               toBytes(payload));
}

void ShareFlow::openShare(const QString& shareCode, const QString& accessCode) {
    if (busyOp()) {
        return;
    }
    const QString code = normalizeCode(shareCode);
    if (code.isEmpty()) {
        failWith(0, QString::fromUtf8("请输入分享码"));
        return;
    }
    m_info = ShareInfo();
    m_info.shareCode = code;
    m_accessCode = accessCode.trimmed();

    protocol::ShareOpenRq rq;
    rq.shareCode = code.toStdString();
    rq.accessCode = accessCode.trimmed().toStdString();
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("分享码格式不合法"));
        return;
    }
    setPhase(SharePhase::Opening, 0, kPendingText);
    m_op = OpOpen;
    m_seq = 0;
    if (m_net == nullptr) {
        return;
    }
    m_seq = m_net->sendRequest(t(protocol::MessageType::ShareOpenRq),
                               toBytes(payload));
}

void ShareFlow::cancel(const QString& shareCode) {
    const QString code = normalizeCode(shareCode);
    if (code.isEmpty() || m_net == nullptr) {
        return;
    }
    m_info.shareCode = code;  // cancelled 信号带回被取消的码
    protocol::ShareCancelRq rq;
    rq.shareCode = code.toStdString();
    std::string payload;
    if (!rq.encode(&payload)) {
        return;
    }
    m_op = OpCancel;
    m_seq = m_net->sendRequest(t(protocol::MessageType::ShareCancelRq),
                               toBytes(payload));
}

void ShareFlow::save(quint64 parentId) {
    if (busyOp()) {
        return;
    }
    if (m_info.phase != SharePhase::Opened || m_info.shareCode.isEmpty()) {
        failWith(0, QString::fromUtf8("请先打开分享"));
        return;
    }
    protocol::ShareSaveRq rq;
    rq.shareCode = m_info.shareCode.toStdString();
    rq.accessCode = m_accessCode.toStdString();
    rq.parentId = parentId;
    // 幂等键：双击/超时重试不能建出两个节点。不能用 ufId（这里还没有），
    // 用毫秒时间戳保证每次"用户意图"唯一。
    rq.reqId = static_cast<std::uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0x7FFFFFFF);
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("转存参数不合法"));
        return;
    }
    setPhase(SharePhase::Saving, 0, QString::fromUtf8("正在保存…"));
    m_op = OpSave;
    m_seq = 0;
    if (m_net == nullptr) {
        return;  // 单测：停在 Saving，等 applySaveRs
    }
    m_seq = m_net->sendRequest(t(protocol::MessageType::ShareSaveRq),
                               toBytes(payload));
}

void ShareFlow::applySaveRs(const QByteArray& payload) {
    protocol::ShareSaveRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("转存响应解析失败"));
        return;
    }
    m_info.code = rs.code;
    if (rs.code != 0) {
        failWith(rs.code, errorText(rs.code));
        return;
    }
    m_info.ufId = rs.ufId;
    m_info.fileName = QString::fromStdString(rs.fileName);
    m_info.size = rs.size;
    m_info.phase = SharePhase::Opened;  // 分享仍然有效，可以继续下载
    m_info.message = QString::fromUtf8("已保存到我的网盘");
    emit changed();
    emit saved(rs.ufId, m_info.fileName, rs.reused != 0);
}

void ShareFlow::applyCreateRs(const QByteArray& payload) {
    protocol::ShareCreateRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("分享响应解析失败"));
        return;
    }
    m_info.code = rs.code;
    if (rs.code != 0) {
        failWith(rs.code, errorText(rs.code));
        return;
    }
    m_info.shareCode = QString::fromStdString(rs.shareCode);
    m_info.accessCode = QString::fromStdString(rs.accessCode);
    m_info.accessCodeRequired = rs.accessCodeRequired != 0;
    m_info.expireAtMs = rs.expireAtMs;
    m_info.phase = SharePhase::Created;
    m_info.message.clear();
    emit changed();
    emit created();
}

void ShareFlow::applyOpenRs(const QByteArray& payload) {
    protocol::ShareOpenRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("分享响应解析失败"));
        return;
    }
    m_info.code = rs.code;
    if (rs.code != 0) {
        failWith(rs.code, errorText(rs.code));
        return;
    }
    m_info.fileName = QString::fromStdString(rs.fileName);
    m_info.size = rs.size;
    m_info.expireAtMs = rs.expireAtMs;
    m_info.shareTicket = QString::fromStdString(rs.shareTicket);
    m_info.accessCodeRequired = rs.accessCodeRequired != 0;
    m_info.phase = SharePhase::Opened;
    m_info.message.clear();
    emit changed();
    emit opened();
}

void ShareFlow::applyCancelRs(const QByteArray& payload) {
    protocol::ShareCancelRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("取消分享响应解析失败"));
        return;
    }
    m_info.code = rs.code;
    if (rs.code != 0) {
        failWith(rs.code, errorText(rs.code));
        return;
    }
    m_info.phase = SharePhase::Cancelled;
    m_info.message = rs.already != 0 ? QString::fromUtf8("分享此前已取消")
                                     : QString::fromUtf8("分享已取消");
    const QString code = m_info.shareCode;
    emit changed();
    emit cancelled(code);
}

void ShareFlow::applyErrorRs(quint32 code, const QString& message) {
    Q_UNUSED(message);
    failWith(code, errorText(code));
}

void ShareFlow::setPhase(SharePhase phase, quint32 code, const QString& message) {
    m_info.phase = phase;
    m_info.code = code;
    m_info.message = message;
    emit changed();
}

void ShareFlow::failWith(quint32 code, const QString& text) {
    m_info.code = code;
    m_info.phase = SharePhase::Failed;
    m_info.message = text;
    emit changed();
    emit failed(code, text);
}

void ShareFlow::onFrame(quint16 type, quint32 seq, const QByteArray& payload) {
    if (m_net == nullptr || seq == 0 || seq != m_seq) {
        return;
    }
    if (type == t(protocol::MessageType::ShareCreateRs) && m_op == OpCreate) {
        applyCreateRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::ShareOpenRs) && m_op == OpOpen) {
        applyOpenRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::ShareCancelRs) && m_op == OpCancel) {
        applyCancelRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::ShareSaveRs) && m_op == OpSave) {
        applySaveRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::ErrorRs)) {
        const std::string body = toStd(payload);
        protocol::Reader reader(body);
        std::uint32_t code = 0;
        std::string message;
        if (!reader.readU32(&code) || !reader.readString(&message)) {
            code = 0;
        }
        m_info.code = code;
        failWith(code, errorText(code));
    }
}

void ShareFlow::onTimeout(quint32 seq, quint16 type) {
    Q_UNUSED(type);
    if (seq == 0 || seq != m_seq) {
        return;
    }
    failWith(0, QString::fromUtf8("分享请求超时，请重试"));
}

void ShareFlow::onAborted(quint32 seq, quint16 type, const QString& reason) {
    Q_UNUSED(type);
    if (seq == 0 || seq != m_seq) {
        return;
    }
    failWith(0, reason.isEmpty() ? QString::fromUtf8("连接已断开，请重试") : reason);
}

}  // namespace shareapi
}  // namespace cm
