// TaskStore 单测：内存 SQLite，验证持久化与 bitmap 往返。
#include <gtest/gtest.h>

#include <QCoreApplication>

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
    ASSERT_TRUE(store.saveTask(task)) << store.lastError().toStdString();

    const QVector<cm::transfer::TransferTask> loaded = store.loadTasks();
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
    ASSERT_TRUE(store.saveTask(task));
    task.remoteName = QString::fromUtf8("v2");
    task.state = cm::transfer::TaskState::Done;
    ASSERT_TRUE(store.saveTask(task));

    const QVector<cm::transfer::TransferTask> loaded = store.loadTasks();
    ASSERT_EQ(1, loaded.size());
    EXPECT_EQ(QString::fromUtf8("v2"), loaded[0].remoteName);
    EXPECT_EQ(cm::transfer::TaskState::Done, loaded[0].state);
}

TEST(TaskStore, DeleteRemovesTaskAndBitmap) {
    cm::sqlapi::TaskStore store;
    ASSERT_TRUE(store.open(QString::fromUtf8(":memory:")));
    cm::transfer::TransferTask task;
    task.id = 5;
    ASSERT_TRUE(store.saveTask(task));
    ASSERT_TRUE(store.saveBitmap(5, QByteArray(1, '\x01')));
    ASSERT_TRUE(store.deleteTask(5));
    EXPECT_TRUE(store.loadTasks().isEmpty());
    EXPECT_TRUE(store.loadBitmap(5).isEmpty());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
