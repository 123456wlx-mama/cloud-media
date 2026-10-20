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

    QVector<transfer::TransferTask> loadTasks();
    QByteArray loadBitmap(int taskId);

    QString lastError() const { return m_lastError; }

private:
    bool ensureSchema();

    QSqlDatabase m_db;
    QString m_connection;
    QString m_lastError;
};

}  // namespace sqlapi
}  // namespace cm
