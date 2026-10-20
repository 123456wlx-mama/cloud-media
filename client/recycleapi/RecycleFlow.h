#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <map>

namespace cm {

namespace netapi {
class NetClient;
}

namespace recycleapi {

struct RecycleEntry {
    quint64 ufId;
    QString name;
    bool isDir;
    quint64 size;
    quint64 deletedAtMs;
    quint64 parentId;

    RecycleEntry();
};

// 删除 → Toast「撤销」→ 恢复 / 回收站列表 → 彻底删除。
// 只软删语义：没有保留期与定时物理清理（契约 §3、需求冻结）。
enum class DeletePhase {
    Idle = 0,
    Deleting,
    Deleted,
    Restoring,
    Restored,
    Purging,
    Purged,
    Failed
};

class RecycleFlow : public QObject {
    Q_OBJECT

public:
    explicit RecycleFlow(netapi::NetClient* net, QObject* parent = nullptr);

    void deleteFile(quint64 ufId, const QString& name);
    void undo();  // 撤销最近一次删除（RestoreRq）
    void restore(quint64 ufId);
    void purge(quint64 ufId);
    void refresh();

    const QVector<RecycleEntry>& entries() const { return m_entries; }
    DeletePhase phase() const { return m_phase; }
    bool canUndo() const { return m_canUndo; }
    quint64 lastUfId() const { return m_lastUfId; }
    QString lastDeletedName() const { return m_lastName; }
    QString message() const { return m_message; }

    static QString errorText(quint32 code);

    // 响应入口（onFrame 与单测共用）。
    void applyDeleteRs(const QByteArray& payload);
    void applyRestoreRs(const QByteArray& payload);
    void applyListRs(const QByteArray& payload);
    void applyPurgeRs(const QByteArray& payload);

signals:
    void changed();
    // 删除成功：UI 弹「已移入回收站 · 撤销」Toast
    void deleted(quint64 ufId, const QString& name);
    void restored(quint64 ufId, const QString& name);
    void purged(quint64 ufId);
    void listChanged();
    void failed(const QString& text);

private slots:
    void onFrame(quint16 type, quint32 seq, const QByteArray& payload);
    void onTimeout(quint32 seq, quint16 type);
    void onAborted(quint32 seq, quint16 type, const QString& reason);

private:
    enum Op { OpNone = 0, OpDelete, OpRestore, OpList, OpPurge };

    void failWith(quint32 code, const QString& text);
    void applyFailure(quint32 code, const QString& fallback);

    netapi::NetClient* m_net;
    QVector<RecycleEntry> m_entries;
    std::map<quint32, int> m_pending;  // seq → Op
    DeletePhase m_phase;
    bool m_canUndo;
    quint64 m_lastUfId;
    QString m_lastName;
    QString m_message;
};

}  // namespace recycleapi
}  // namespace cm
