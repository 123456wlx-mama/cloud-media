// TaskStore 单测：内存 SQLite，验证持久化与 bitmap 往返。
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "sqlapi/TaskStore.h"

TEST(TaskStore, SaveAndLoadRoundTrip) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:"))) << store.lastError().toStdString();

    cm::transfer::TransferTask task;
    task.id = 7;
    task.type = cm::transfer::TaskType::Download;
    task.localPath = QString::fromUtf8("C:/tmp/x.bin");
    task.remoteName = QString::fromUtf8("x.bin");
    task.size = 10485760;
    task.state = cm::transfer::TaskState::Paused;
    task.bytesDone = 4194304;
    task.ufId = 30668;
    task.fileMd5 = QString::fromUtf8("abc");
    task.blockSize = 4194304;
    task.blockCount = 3;
    task.uid = 1001;
    ASSERT_TRUE(store.saveTask(task)) << store.lastError().toStdString();

    const QVector<cm::transfer::TransferTask> loaded = store.loadTasks(1001);
    ASSERT_EQ(1, loaded.size());
    EXPECT_EQ(7, loaded[0].id);
    EXPECT_EQ(cm::transfer::TaskType::Download, loaded[0].type);
    EXPECT_EQ(cm::transfer::TaskState::Paused, loaded[0].state);
    EXPECT_EQ(4194304ull, loaded[0].bytesDone);
    EXPECT_EQ(30668ull, loaded[0].ufId);
    EXPECT_EQ(QString::fromUtf8("C:/tmp/x.bin"), loaded[0].localPath);
}

TEST(TaskStore, BitmapRoundTrip) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:")));

    QByteArray bitmap(2, '\0');
    cm::transfer::bitmapSet(&bitmap, 0);
    cm::transfer::bitmapSet(&bitmap, 9);
    ASSERT_TRUE(store.saveBitmap(3, bitmap));

    const QByteArray loaded = store.loadBitmap(3);
    EXPECT_EQ(bitmap, loaded);
    EXPECT_TRUE(cm::transfer::bitmapTest(loaded, 9));

    ASSERT_TRUE(store.deleteBitmap(3));
    EXPECT_TRUE(store.loadBitmap(3).isEmpty());
}

TEST(TaskStore, SavingTwiceReplaces) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:")));

    cm::transfer::TransferTask task;
    task.id = 11;
    task.remoteName = QString::fromUtf8("v1");
    task.uid = 1001;
    ASSERT_TRUE(store.saveTask(task));
    task.remoteName = QString::fromUtf8("v2");
    task.state = cm::transfer::TaskState::Done;
    ASSERT_TRUE(store.saveTask(task));

    const QVector<cm::transfer::TransferTask> loaded = store.loadTasks(1001);
    ASSERT_EQ(1, loaded.size());
    EXPECT_EQ(QString::fromUtf8("v2"), loaded[0].remoteName);
    EXPECT_EQ(cm::transfer::TaskState::Done, loaded[0].state);
}

TEST(TaskStore, DeleteRemovesTaskAndBitmap) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:")));
    cm::transfer::TransferTask task;
    task.id = 5;
    task.uid = 1001;
    ASSERT_TRUE(store.saveTask(task));
    ASSERT_TRUE(store.saveBitmap(5, QByteArray(1, '\x01')));
    ASSERT_TRUE(store.deleteTask(5));
    EXPECT_TRUE(store.loadTasks(1001).isEmpty());
    EXPECT_TRUE(store.loadBitmap(5).isEmpty());
}

// 回归：本地任务缓存必须按账号隔离。
// 修复前 loadTasks() 不带 WHERE，换账号登录会把上一个用户的 100 条上传记录
// 原样列出来，而那些任务的 taskId/ticket 在服务端属于别人。
TEST(TaskStore, TasksAreIsolatedPerUser) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:"))) << store.lastError().toStdString();

    cm::transfer::TransferTask alice;
    alice.id = 1;
    alice.remoteName = QString::fromUtf8("alice.bin");
    alice.uid = 1001;
    ASSERT_TRUE(store.saveTask(alice));

    cm::transfer::TransferTask bob;
    bob.id = 2;
    bob.remoteName = QString::fromUtf8("bob.bin");
    bob.uid = 1002;
    ASSERT_TRUE(store.saveTask(bob));

    const QVector<cm::transfer::TransferTask> aliceTasks = store.loadTasks(1001);
    ASSERT_EQ(1, aliceTasks.size());
    EXPECT_EQ(QString::fromUtf8("alice.bin"), aliceTasks[0].remoteName);

    const QVector<cm::transfer::TransferTask> bobTasks = store.loadTasks(1002);
    ASSERT_EQ(1, bobTasks.size());
    EXPECT_EQ(QString::fromUtf8("bob.bin"), bobTasks[0].remoteName);

    // 新注册的账号看不到任何历史任务
    EXPECT_TRUE(store.loadTasks(1003).isEmpty());
}

// 回归：老用户的 cm-tasks.db 里 t_task 没有 u_id 列，open() 必须自动补列，
// 否则 saveTask 会一直报 "no such column"，任务静默丢失。
TEST(TaskStore, MigratesLegacyDatabaseWithoutUserId) {
    const QString path =
        QDir::tempPath() + QString::fromUtf8("/cm_taskstore_migrate_%1.db")
                               .arg(static_cast<qulonglong>(QCoreApplication::applicationPid()));
    QFile::remove(path);

    const QString legacyConn =
        QString::fromUtf8("cm_legacy_%1").arg(QCoreApplication::applicationPid());
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QString::fromUtf8("QSQLITE"), legacyConn);
        db.setDatabaseName(path);
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        // 老版本表结构：没有 u_id
        ASSERT_TRUE(q.exec(QString::fromUtf8(
            "CREATE TABLE t_task (id INTEGER PRIMARY KEY, type INTEGER NOT NULL,"
            " local_path TEXT NOT NULL, remote_name TEXT NOT NULL, size INTEGER NOT NULL,"
            " state INTEGER NOT NULL, bytes_done INTEGER NOT NULL, err_code INTEGER NOT NULL,"
            " err_msg TEXT, instant INTEGER NOT NULL, task_id TEXT, file_md5 TEXT,"
            " chunk_size INTEGER NOT NULL, total_chunks INTEGER NOT NULL,"
            " next_chunk INTEGER NOT NULL, uf_id INTEGER NOT NULL, f_id INTEGER NOT NULL,"
            " ticket TEXT, block_size INTEGER NOT NULL, block_count INTEGER NOT NULL,"
            " next_block INTEGER NOT NULL)")));
        ASSERT_TRUE(q.exec(QString::fromUtf8(
            "INSERT INTO t_task VALUES (1,1,'p','old.bin',10,2,0,0,NULL,0,NULL,NULL,"
            "0,0,0,0,0,NULL,0,0,0)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(legacyConn);

    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(path)) << store.lastError().toStdString();

    // 迁移后新代码能正常写：失败的写法会返回 "no such column: u_id"
    cm::transfer::TransferTask task;
    task.id = 9;
    task.remoteName = QString::fromUtf8("new.bin");
    task.uid = 1001;
    ASSERT_TRUE(store.saveTask(task)) << store.lastError().toStdString();

    const QVector<cm::transfer::TransferTask> loaded = store.loadTasks(1001);
    ASSERT_EQ(1, loaded.size());
    EXPECT_EQ(QString::fromUtf8("new.bin"), loaded[0].remoteName);

    // 老行 u_id 默认为 0，不归属任何账号，因此不会被任何用户看到
    EXPECT_TRUE(store.loadTasks(0).size() <= 1);

    store.close();
    QFile::remove(path);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
