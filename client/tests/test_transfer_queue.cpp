// TransferQueue 状态机单测：假网络（net=nullptr）+ 假时钟，不依赖真实 IO。
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include "netapi/NetClient.h"
#include "transfer/TransferQueue.h"

namespace {

class TransferQueueTest : public ::testing::Test {
protected:
    void SetUp() override { now = 1000; }

    cm::transfer::TransferQueue queue{nullptr, nullptr};
    qint64 now;
};

}  // namespace

TEST_F(TransferQueueTest, UploadAdmissionCapsAtTwo) {
    queue.setClock([this]() { return now; });
    const int a = queue.enqueueUpload(QString::fromUtf8("C:/tmp/a.bin"), "a.bin");
    const int b = queue.enqueueUpload(QString::fromUtf8("C:/tmp/b.bin"), "b.bin");
    const int c = queue.enqueueUpload(QString::fromUtf8("C:/tmp/c.bin"), "c.bin");

    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(a)->state);
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(b)->state);
    ASSERT_EQ(cm::transfer::TaskState::Queued, queue.find(c)->state);
}

TEST_F(TransferQueueTest, DownloadAdmissionCapsAtTwo) {
    queue.enqueueDownload(101, "x.bin", 10);
    queue.enqueueDownload(102, "y.bin", 10);
    const int third = queue.enqueueDownload(103, "z.bin", 10);
    EXPECT_EQ(cm::transfer::TaskState::Queued, queue.find(third)->state);
}

TEST_F(TransferQueueTest, PauseFreesSlotAndResumeRequeues) {
    const int a = queue.enqueueUpload(QString::fromUtf8("C:/tmp/a.bin"), "a.bin");
    const int b = queue.enqueueUpload(QString::fromUtf8("C:/tmp/b.bin"), "b.bin");
    const int c = queue.enqueueUpload(QString::fromUtf8("C:/tmp/c.bin"), "c.bin");
    ASSERT_EQ(cm::transfer::TaskState::Queued, queue.find(c)->state);

    queue.pause(a);
    EXPECT_EQ(cm::transfer::TaskState::Paused, queue.find(a)->state);
    // 空出的并发位应立即被排队任务补上
    EXPECT_EQ(cm::transfer::TaskState::Running, queue.find(c)->state);

    // a 恢复后仍要排队：并发位已被 b/c 占满（cap=2）
    queue.resume(a);
    EXPECT_EQ(cm::transfer::TaskState::Queued, queue.find(a)->state);
    // 再腾出一个位，a 才能起
    queue.pause(c);
    EXPECT_EQ(cm::transfer::TaskState::Running, queue.find(a)->state);
    EXPECT_EQ(cm::transfer::TaskState::Running, queue.find(b)->state);
}

TEST_F(TransferQueueTest, CancelIsTerminal) {
    const int a = queue.enqueueUpload(QString::fromUtf8("C:/tmp/a.bin"), "a.bin");
    queue.cancel(a);
    EXPECT_EQ(cm::transfer::TaskState::Canceled, queue.find(a)->state);
    EXPECT_TRUE(queue.find(a)->isTerminal());
}

TEST_F(TransferQueueTest, LinkLostPausesAllActive) {
    const int a = queue.enqueueUpload(QString::fromUtf8("C:/tmp/a.bin"), "a.bin");
    const int b = queue.enqueueDownload(7, "b.bin", 10);
    queue.onLinkLost();
    EXPECT_EQ(cm::transfer::TaskState::Paused, queue.find(a)->state);
    EXPECT_EQ(cm::transfer::TaskState::Paused, queue.find(b)->state);
}

// 同一 seq 上"断线中止"与"请求超时"两条终态信号先后到达：只允许生效一条，
// 且断线优先——任务必须是"已暂停"（等重连），不能钉成"失败"。
TEST(TransferQueueTerminal, AbortThenTimeoutOnSameSeqPausesOnce) {
    cm::netapi::NetClient net;  // 不连服务端：请求停在 worker 的 pending/outbox
    cm::transfer::TransferQueue queue(&net, nullptr);
    const int id = queue.enqueueDownload(42, QString::fromUtf8("resume.bin"),
                                        8 * 1024 * 1024);
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    int notices = 0;
    QObject::connect(&queue, &cm::transfer::TransferQueue::notice,
                     [&notices](const QString&, bool) { ++notices; });

    const quint32 seq = net.lastSeq();
    ASSERT_NE(0u, seq);
    const bool ok1 = QMetaObject::invokeMethod(
        &queue, "onAborted", Qt::DirectConnection, Q_ARG(quint32, seq),
        Q_ARG(quint16, 10008), Q_ARG(QString, QString::fromUtf8("连接已断开")));
    ASSERT_TRUE(ok1);
    const bool ok2 = QMetaObject::invokeMethod(
        &queue, "onTimeout", Qt::DirectConnection, Q_ARG(quint32, seq),
        Q_ARG(quint16, 10008));
    ASSERT_TRUE(ok2);

    EXPECT_EQ(cm::transfer::TaskState::Paused, queue.find(id)->state);
    EXPECT_EQ(1, notices);  // 第二条终态信号被 seq 去重吞掉
}

// 链路正常时服务端不响应：走失败（可重试），与断线路径区分开。
TEST(TransferQueueTerminal, TimeoutWithHealthyLinkFails) {
    cm::netapi::NetClient net;
    cm::transfer::TransferQueue queue(&net, nullptr);
    QMetaObject::invokeMethod(&queue, "onLinkState", Qt::DirectConnection,
                              Q_ARG(int, 1), Q_ARG(QString, QString::fromUtf8("已连接")));
    const int id = queue.enqueueDownload(43, QString::fromUtf8("stuck.bin"), 1024);
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    const quint32 seq = net.lastSeq();
    ASSERT_NE(0u, seq);
    const bool ok = QMetaObject::invokeMethod(
        &queue, "onTimeout", Qt::DirectConnection, Q_ARG(quint32, seq),
        Q_ARG(quint16, 10008));
    ASSERT_TRUE(ok);
    EXPECT_EQ(cm::transfer::TaskState::Failed, queue.find(id)->state);
}

TEST(TransferBitmap, SetTestCountAndFirstMissing) {
    QByteArray bitmap(1, '\0');
    EXPECT_FALSE(cm::transfer::bitmapTest(bitmap, 0));
    cm::transfer::bitmapSet(&bitmap, 0);
    cm::transfer::bitmapSet(&bitmap, 2);
    EXPECT_TRUE(cm::transfer::bitmapTest(bitmap, 0));
    EXPECT_FALSE(cm::transfer::bitmapTest(bitmap, 1));
    EXPECT_TRUE(cm::transfer::bitmapTest(bitmap, 2));
    EXPECT_EQ(2u, cm::transfer::bitmapCount(bitmap, 8));
    EXPECT_EQ(1u, cm::transfer::firstMissing(bitmap, 8));
}

TEST(TransferBitmap, ResizeKeepsExistingBits) {
    QByteArray bitmap(1, '\0');
    cm::transfer::bitmapSet(&bitmap, 3);
    cm::transfer::bitmapResize(&bitmap, 40);   // 5 字节
    EXPECT_EQ(5, bitmap.size());
    EXPECT_TRUE(cm::transfer::bitmapTest(bitmap, 3));
    EXPECT_FALSE(cm::transfer::bitmapTest(bitmap, 39));
}

TEST(TransferBitmap, AllDoneMeansNoMissing) {
    QByteArray bitmap(1, '\0');
    for (quint32 i = 0; i < 8; ++i) {
        cm::transfer::bitmapSet(&bitmap, i);
    }
    EXPECT_EQ(8u, cm::transfer::firstMissing(bitmap, 8));
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
