#include "sqlapi/TaskStore.h"

#include <QAtomicInt>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace cm {
namespace sqlapi {
namespace {

QAtomicInt g_connectionSeq(0);

int stateToInt(transfer::TaskState state) { return static_cast<int>(state); }

transfer::TaskState intToState(int value) {
    if (value < 0 || value > static_cast<int>(transfer::TaskState::Canceled)) {
        return transfer::TaskState::Paused;
    }
    return static_cast<transfer::TaskState>(value);
}

}  // namespace

TaskStore::TaskStore(QObject* parent) : QObject(parent) {
    m_connection = QString::fromUtf8("cm_taskstore_%1")
                       .arg(g_connectionSeq.fetchAndAddOrdered(1));
}

TaskStore::~TaskStore() { close(); }

bool TaskStore::open(const QString& path) {
    m_db = QSqlDatabase::addDatabase(QString::fromUtf8("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return ensureSchema();
}

void TaskStore::close() {
    if (m_db.isOpen()) {
        m_db.close();
    }
    if (QSqlDatabase::contains(m_connection)) {
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool TaskStore::ensureSchema() {
    QSqlQuery query(m_db);
    const char* taskTable =
        "CREATE TABLE IF NOT EXISTS t_task ("
        " id INTEGER PRIMARY KEY,"
        " type INTEGER NOT NULL,"
        " local_path TEXT NOT NULL,"
        " remote_name TEXT NOT NULL,"
        " size INTEGER NOT NULL,"
        " state INTEGER NOT NULL,"
        " bytes_done INTEGER NOT NULL,"
        " err_code INTEGER NOT NULL,"
        " err_msg TEXT,"
        " instant INTEGER NOT NULL,"
        " task_id TEXT,"
        " file_md5 TEXT,"
        " chunk_size INTEGER NOT NULL,"
        " total_chunks INTEGER NOT NULL,"
        " next_chunk INTEGER NOT NULL,"
        " uf_id INTEGER NOT NULL,"
        " f_id INTEGER NOT NULL,"
        " ticket TEXT,"
        " block_size INTEGER NOT NULL,"
        " block_count INTEGER NOT NULL,"
        " next_block INTEGER NOT NULL,"
        " u_id INTEGER NOT NULL DEFAULT 0"
        ")";
    if (!query.exec(QString::fromUtf8(taskTable))) {
        m_lastError = query.lastError().text();
        return false;
    }
    if (!ensureUserIdColumn()) {
        return false;
    }
    if (!query.exec(QString::fromUtf8(
            "CREATE TABLE IF NOT EXISTS t_task_bitmap ("
            " task_id INTEGER PRIMARY KEY,"
            " bitmap BLOB NOT NULL"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool TaskStore::ensureUserIdColumn() {
    QSqlQuery info(m_db);
    if (!info.exec(QString::fromUtf8("PRAGMA table_info(t_task)"))) {
        m_lastError = info.lastError().text();
        return false;
    }
    while (info.next()) {
        if (info.value(1).toString() == QString::fromUtf8("u_id")) {
            return true;
        }
    }
    QSqlQuery alter(m_db);
    if (!alter.exec(QString::fromUtf8(
            "ALTER TABLE t_task ADD COLUMN u_id INTEGER NOT NULL DEFAULT 0"))) {
        m_lastError = alter.lastError().text();
        return false;
    }
    return true;
}

bool TaskStore::saveTask(const transfer::TransferTask& task) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8(
        "INSERT OR REPLACE INTO t_task (id, type, local_path, remote_name, size,"
        " state, bytes_done, err_code, err_msg, instant, task_id, file_md5,"
        " chunk_size, total_chunks, next_chunk, uf_id, f_id, ticket, block_size,"
        " block_count, next_block, u_id)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(task.id);
    query.addBindValue(static_cast<int>(task.type));
    // NOT NULL 列必须绑空串而不是 null QString（null 会绑定成 SQL NULL）。
    query.addBindValue(task.localPath.isNull() ? QString::fromUtf8("") : task.localPath);
    query.addBindValue(task.remoteName.isNull() ? QString::fromUtf8("") : task.remoteName);
    query.addBindValue(static_cast<qint64>(task.size));
    query.addBindValue(stateToInt(task.state));
    query.addBindValue(static_cast<qint64>(task.bytesDone));
    query.addBindValue(static_cast<uint>(task.errCode));
    query.addBindValue(task.errMsg);
    query.addBindValue(task.instant ? 1 : 0);
    query.addBindValue(task.taskId);
    query.addBindValue(task.fileMd5);
    query.addBindValue(static_cast<uint>(task.chunkSize));
    query.addBindValue(static_cast<uint>(task.totalChunks));
    query.addBindValue(static_cast<uint>(task.nextChunk));
    query.addBindValue(static_cast<qint64>(task.ufId));
    query.addBindValue(static_cast<qint64>(task.fId));
    query.addBindValue(task.ticket);
    query.addBindValue(static_cast<uint>(task.blockSize));
    query.addBindValue(static_cast<uint>(task.blockCount));
    query.addBindValue(static_cast<uint>(task.nextBlock));
    query.addBindValue(static_cast<qint64>(task.uid));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool TaskStore::deleteTask(int id) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8("DELETE FROM t_task WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return deleteBitmap(id);
}

bool TaskStore::saveBitmap(int taskId, const QByteArray& bitmap) {
    if (taskId <= 0) {
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8(
        "INSERT OR REPLACE INTO t_task_bitmap (task_id, bitmap) VALUES (?, ?)"));
    query.addBindValue(taskId);
    query.addBindValue(bitmap);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool TaskStore::deleteBitmap(int taskId) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8("DELETE FROM t_task_bitmap WHERE task_id = ?"));
    query.addBindValue(taskId);
    return query.exec();
}

QVector<transfer::TransferTask> TaskStore::loadTasks(quint64 uid) {
    QVector<transfer::TransferTask> tasks;
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8(
        "SELECT id, type, local_path, remote_name, size, state, bytes_done,"
        " err_code, err_msg, instant, task_id, file_md5, chunk_size,"
        " total_chunks, next_chunk, uf_id, f_id, ticket, block_size,"
        " block_count, next_block FROM t_task WHERE u_id = ? ORDER BY id"));
    query.addBindValue(static_cast<qint64>(uid));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return tasks;
    }
    while (query.next()) {
        transfer::TransferTask task;
        task.uid = uid;
        task.id = query.value(0).toInt();
        task.type = query.value(1).toInt() == 0 ? transfer::TaskType::Upload
                                                : transfer::TaskType::Download;
        task.localPath = query.value(2).toString();
        task.remoteName = query.value(3).toString();
        task.size = query.value(4).toLongLong();
        task.state = intToState(query.value(5).toInt());
        task.bytesDone = query.value(6).toLongLong();
        task.errCode = query.value(7).toUInt();
        task.errMsg = query.value(8).toString();
        task.instant = query.value(9).toInt() != 0;
        task.taskId = query.value(10).toString();
        task.fileMd5 = query.value(11).toString();
        task.chunkSize = query.value(12).toUInt();
        task.totalChunks = query.value(13).toUInt();
        task.nextChunk = query.value(14).toUInt();
        task.ufId = query.value(15).toULongLong();
        task.fId = query.value(16).toULongLong();
        task.ticket = query.value(17).toString();
        task.blockSize = query.value(18).toUInt();
        task.blockCount = query.value(19).toUInt();
        task.nextBlock = query.value(20).toUInt();
        tasks.append(task);
    }
    return tasks;
}

QByteArray TaskStore::loadBitmap(int taskId) {
    QSqlQuery query(m_db);
    query.prepare(QString::fromUtf8("SELECT bitmap FROM t_task_bitmap WHERE task_id = ?"));
    query.addBindValue(taskId);
    if (!query.exec() || !query.next()) {
        return QByteArray();
    }
    return query.value(0).toByteArray();
}

}  // namespace sqlapi
}  // namespace cm
