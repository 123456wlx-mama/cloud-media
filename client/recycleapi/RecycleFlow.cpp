#include "recycleapi/RecycleFlow.h"

#include <string>

#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/recycle.h"
#include "cm/protocol/serialize.h"
#include "netapi/NetClient.h"

namespace cm {
namespace recycleapi {
namespace {

quint16 t(protocol::MessageType type) {
    return static_cast<std::uint16_t>(type);
}

std::string toStd(const QByteArray& data) {
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

QByteArray toBytes(const std::string& data) {
    return QByteArray(data.data(), static_cast<int>(data.size()));
}

const quint32 kRecyclePageSize = 50;

}  // namespace

RecycleEntry::RecycleEntry()
    : ufId(0), isDir(false), size(0), deletedAtMs(0), parentId(0) {}

RecycleFlow::RecycleFlow(netapi::NetClient* net, QObject* parent)
    : QObject(parent), m_net(net), m_phase(DeletePhase::Idle), m_canUndo(false),
      m_lastUfId(0) {
    if (m_net != nullptr) {
        connect(m_net, SIGNAL(frame(quint16, quint32, QByteArray)), this,
                SLOT(onFrame(quint16, quint32, QByteArray)));
        connect(m_net, SIGNAL(requestTimeout(quint32, quint16)), this,
                SLOT(onTimeout(quint32, quint16)));
        connect(m_net, SIGNAL(requestAborted(quint32, quint16, QString)), this,
                SLOT(onAborted(quint32, quint16, QString)));
    }
}

QString RecycleFlow::errorText(quint32 code) {
    switch (code) {
        case 0:
            return QString::fromUtf8("成功");
        case 1002:
            return QString::fromUtf8("文件不存在或已删除");
        case 1003:
            return QString::fromUtf8("同名文件过多，请先整理");
        case 2002:
            return QString::fromUtf8("登录已过期，请重新登录");
        case 4008:
            return QString::fromUtf8("服务暂不可用，请稍后重试");
        default:
            break;
    }
    return QString::fromUtf8("操作失败（code=%1）").arg(code);
}

void RecycleFlow::failWith(quint32 code, const QString& text) {
    Q_UNUSED(code);
    m_phase = DeletePhase::Failed;
    m_message = text;
    emit changed();
    emit failed(text);
}

void RecycleFlow::applyFailure(quint32 code, const QString& fallback) {
    failWith(code, code == 0 ? fallback : errorText(code));
}

void RecycleFlow::deleteFile(quint64 ufId, const QString& name) {
    if (ufId == 0) {
        return;
    }
    m_lastUfId = ufId;
    m_lastName = name;
    m_canUndo = false;
    protocol::DeleteFileRq rq;
    rq.ufId = ufId;
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("删除参数不合法"));
        return;
    }
    m_phase = DeletePhase::Deleting;
    emit changed();
    if (m_net == nullptr) {
        return;  // 单测：等 applyDeleteRs
    }
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::DeleteFileRq),
                                           toBytes(payload));
    m_pending[seq] = OpDelete;
}

void RecycleFlow::undo() {
    if (!m_canUndo || m_lastUfId == 0) {
        return;
    }
    restore(m_lastUfId);
}

void RecycleFlow::restore(quint64 ufId) {
    if (ufId == 0) {
        return;
    }
    protocol::RestoreRq rq;
    rq.ufId = ufId;
    rq.autoRename = 1;  // 同名冲突时自动改名（协议 1003 的兜底）
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("恢复参数不合法"));
        return;
    }
    m_phase = DeletePhase::Restoring;
    emit changed();
    if (m_net == nullptr) {
        return;
    }
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::RestoreRq),
                                           toBytes(payload));
    m_pending[seq] = OpRestore;
}

void RecycleFlow::purge(quint64 ufId) {
    if (ufId == 0) {
        return;
    }
    protocol::PurgeRq rq;
    rq.ufId = ufId;
    rq.confirm = 1;  // 协议要求必须=1
    std::string payload;
    if (!rq.encode(&payload)) {
        failWith(0, QString::fromUtf8("彻底删除参数不合法"));
        return;
    }
    m_phase = DeletePhase::Purging;
    emit changed();
    if (m_net == nullptr) {
        return;
    }
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::PurgeRq),
                                           toBytes(payload));
    m_pending[seq] = OpPurge;
}

void RecycleFlow::refresh() {
    if (m_net == nullptr) {
        return;
    }
    protocol::RecycleListRq rq;
    rq.limit = kRecyclePageSize;
    rq.cursorDeletedMs = 0;
    rq.cursorId = 0;
    std::string payload;
    if (!rq.encode(&payload)) {
        return;
    }
    const quint32 seq = m_net->sendRequest(t(protocol::MessageType::RecycleListRq),
                                           toBytes(payload));
    m_pending[seq] = OpList;
}

void RecycleFlow::applyDeleteRs(const QByteArray& payload) {
    protocol::DeleteFileRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("删除响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        applyFailure(rs.code, QString::fromUtf8("删除失败"));
        return;
    }
    m_phase = DeletePhase::Deleted;
    m_canUndo = rs.already == 0;  // 已经删过的没有"撤销"语义
    m_message = m_canUndo ? QString::fromUtf8("已移入回收站")
                          : QString::fromUtf8("该文件已在回收站");
    emit changed();
    if (m_canUndo) {
        emit deleted(m_lastUfId, m_lastName);
    }
}

void RecycleFlow::applyRestoreRs(const QByteArray& payload) {
    protocol::RestoreRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("恢复响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        applyFailure(rs.code, QString::fromUtf8("恢复失败"));
        return;
    }
    m_phase = DeletePhase::Restored;
    m_canUndo = false;
    m_message = rs.renamed != 0
                    ? QString::fromUtf8("已恢复到「%1」")
                          .arg(QString::fromStdString(rs.name))
                    : QString::fromUtf8("已恢复");
    emit changed();
    emit restored(rs.ufId, QString::fromStdString(rs.name));
}

void RecycleFlow::applyListRs(const QByteArray& payload) {
    protocol::RecycleListRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("回收站响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        applyFailure(rs.code, QString::fromUtf8("加载回收站失败"));
        return;
    }
    m_entries.clear();
    for (std::size_t i = 0; i < rs.items.size(); ++i) {
        const protocol::RecycleItem& item = rs.items[i];
        RecycleEntry entry;
        entry.ufId = item.ufId;
        entry.name = QString::fromStdString(item.name);
        entry.isDir = item.isDir != 0;
        entry.size = item.size;
        entry.deletedAtMs = item.deletedAtMs;
        entry.parentId = item.parentId;
        m_entries.append(entry);
    }
    emit listChanged();
}

void RecycleFlow::applyPurgeRs(const QByteArray& payload) {
    protocol::PurgeRs rs;
    if (!rs.decode(toStd(payload))) {
        failWith(0, QString::fromUtf8("彻底删除响应解析失败"));
        return;
    }
    if (rs.code != 0) {
        applyFailure(rs.code, QString::fromUtf8("彻底删除失败"));
        return;
    }
    m_phase = DeletePhase::Purged;
    m_message = QString::fromUtf8("已彻底删除 %1 个文件、%2 个目录")
                    .arg(rs.filesDeleted)
                    .arg(rs.dirsDeleted);
    emit changed();
    emit purged(m_lastUfId);
}

void RecycleFlow::onFrame(quint16 type, quint32 seq, const QByteArray& payload) {
    const std::map<quint32, int>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        return;
    }
    const int op = it->second;
    m_pending.erase(it);
    if (type == t(protocol::MessageType::DeleteFileRs) && op == OpDelete) {
        applyDeleteRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::RestoreRs) && op == OpRestore) {
        applyRestoreRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::RecycleListRs) && op == OpList) {
        applyListRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::PurgeRs) && op == OpPurge) {
        applyPurgeRs(payload);
        return;
    }
    if (type == t(protocol::MessageType::ErrorRs)) {
        protocol::Reader reader(toStd(payload));
        std::uint32_t code = 0;
        std::string message;
        if (!reader.readU32(&code) || !reader.readString(&message)) {
            code = 0;
        }
        failWith(code, errorText(code));
    }
}

void RecycleFlow::onTimeout(quint32 seq, quint16 type) {
    Q_UNUSED(type);
    const std::map<quint32, int>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        return;
    }
    m_pending.erase(it);
    failWith(0, QString::fromUtf8("请求超时，请重试"));
}

void RecycleFlow::onAborted(quint32 seq, quint16 type, const QString& reason) {
    Q_UNUSED(type);
    const std::map<quint32, int>::iterator it = m_pending.find(seq);
    if (it == m_pending.end()) {
        return;
    }
    m_pending.erase(it);
    failWith(0, reason.isEmpty() ? QString::fromUtf8("连接已断开") : reason);
}

}  // namespace recycleapi
}  // namespace cm
