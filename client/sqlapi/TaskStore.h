#pragma once

#include <QByteArray>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "transfer/TransferTask.h"

namespace cm {
namespace sqlapi {

// 传输任务持久化（SQLite，QtSql）。只给 transfer 层用，UI 不碰 SQL。
// 所有语句都是预编译 + bindValue，禁止拼串（契约 §6）。
class TaskStore : public QObject {
    Q_OBJECT

public:
    explicit TaskStore(QObject* parent = nullptr);
    ~TaskStore() override;

    // path 传 ":memory:" 即内存库（单测）。
    bool open(const QString& path);
    void close();

    bool saveTask(const transfer::TransferTask& task);
    bool deleteTask(int id);
    bool saveBitmap(int taskId, const QByteArray& bitmap);
    bool deleteBitmap(int taskId);

    // 只返回该账号的任务：本地缓存必须按 u_id 隔离，
    // 否则换账号登录会把上一个用户的任务列出来（taskId/ticket 在服务端不属于他）。
    QVector<transfer::TransferTask> loadTasks(quint64 uid);
    QByteArray loadBitmap(int taskId);

    QString lastError() const { return m_lastError; }

private:
    bool ensureSchema();
    // 老库升级：t_task 早期没有 u_id 列，这里补上（默认 0，旧任务不归属任何账号）。
    bool ensureUserIdColumn();

    QSqlDatabase m_db;
    QString m_connection;
    QString m_lastError;
};

}  // namespace sqlapi
}  // namespace cm
