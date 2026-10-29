// TransferQueue 状态机单测：假网络（net=nullptr）+ 假时钟，不依赖真实 IO。
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QString>

#include <string>

#include "cm/protocol/messages/download.h"
#include "netapi/NetClient.h"
#include "transfer/TransferQueue.h"

namespace {

QByteArray fromStd(const std::string& data) {
    return QByteArray(data.data(), static_cast<int>(data.size()));
}

// 用例中途 ASSERT_ 失败会提前 return，普通清理语句跑不到——用析构兜底，
// 免得每跑一次失败的用例就在 %TEMP% 里留一份 .part 垃圾。
struct TempPartGuard {
    QString part;
    QString finalPath;
    QString statePath;

    ~TempPartGuard() {
        QFile::remove(part);
        QFile::remove(finalPath);
        QFile::remove(statePath);
    }
};

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

namespace {

// 把 10012 响应塞回队列（模拟服务端），把任务推进到"开始取块"。
bool deliverOpen(cm::transfer::TransferQueue* queue, cm::netapi::NetClient* net) {
    cm::protocol::DownloadOpenRs rs;
    rs.code = 0;
    rs.fId = 1;
    rs.size = 1024;
    rs.fileMd5 = std::string(32, 'a');
    rs.blockSize = 1024;
    rs.blockCount = 1;
    rs.ticket = std::string(32, 'b');
    rs.ticketExpireMs = 0;
    rs.startBlock = 0;
    rs.zeroCopy = 0;
    rs.hls = 0;
    std::string payload;
    if (!rs.encode(&payload)) {
        return false;
    }
    return QMetaObject::invokeMethod(
        queue, "onFrame", Qt::DirectConnection, Q_ARG(quint16, 10012),
        Q_ARG(quint32, net->lastSeq()),
        Q_ARG(QByteArray, QByteArray(payload.data(), static_cast<int>(payload.size()))));
}

// 把 10011 的数据块塞回队列；payload 就是裸块字节。
bool deliverBlock(cm::transfer::TransferQueue* queue, cm::netapi::NetClient* net,
                  const QByteArray& data) {
    return QMetaObject::invokeMethod(queue, "onFrame", Qt::DirectConnection,
                                     Q_ARG(quint16, 10011),
                                     Q_ARG(quint32, net->lastSeq()),
                                     Q_ARG(QByteArray, data));
}

}  // namespace

// 回归：写盘失败绝不能把块标记为完成。
// 修复前 writePart 的返回值没人看，open/write 失败照样 bitmapSet，
// 续传时跳过这块，最终得到一个"大小正确、内容零填充"的损坏文件。
TEST(TransferQueueWrite, WriteFailureDoesNotMarkBlockDone) {
    cm::netapi::NetClient net;
    cm::transfer::TransferQueue queue(&net, nullptr);

    // 用同名目录占住 .part 路径：QFile::open 在任何平台上都会失败
    const QString name =
        QString::fromUtf8("cm_wf_%1.bin").arg(QCoreApplication::applicationPid());
    const QString partPath =
        QDir::tempPath() + QString::fromUtf8("/") + name + QString::fromUtf8(".part");
    QFile::remove(partPath);
    QDir(partPath).removeRecursively();
    ASSERT_TRUE(QDir().mkpath(partPath));
    // 清掉上次可能残留的 .part.state，否则任务会走 DownloadStat 分支而不是直接 open
    QFile::remove(partPath + QString::fromUtf8(".state"));

    const int id = queue.enqueueDownload(9001, name, 1024, true);
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    ASSERT_TRUE(deliverOpen(&queue, &net));

    const QByteArray block(1024, 'x');
    ASSERT_TRUE(deliverBlock(&queue, &net, block));

    cm::transfer::TransferTask* task = queue.find(id);
    ASSERT_NE(nullptr, task);
    EXPECT_EQ(0u, cm::transfer::bitmapCount(task->bitmap, task->blockCount))
        << "写盘失败的块被误标为已完成，续传会跳过它";
    EXPECT_EQ(quint64(0), task->bytesDone);
    EXPECT_EQ(0u, task->blocksFetched);
    EXPECT_EQ(1u, task->writeFailCount);
    EXPECT_EQ(cm::transfer::TaskState::Running, task->state);

    // 连续失败达到上限后必须判失败，不能无限重取同一块把服务端刷爆
    ASSERT_TRUE(deliverBlock(&queue, &net, block));
    ASSERT_TRUE(deliverBlock(&queue, &net, block));
    EXPECT_EQ(cm::transfer::TaskState::Failed, queue.find(id)->state);

    QDir(partPath).removeRecursively();
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

// 回归 E-1：服务端同名文件被换成更小的新版本后，旧 .part 比新文件长。
// 所有块写完后必须把多余尾部截掉并正常完成，而不是报「本地文件大小不符」
// 然后永远重试（writePart 只 seek+write，从不 resize）。
TEST(TransferQueueDownloadDone, TruncatesStalePartTail) {
    const QString name =
        QString::fromUtf8("cm_e1_part_%1.bin")
            .arg(static_cast<qulonglong>(QCoreApplication::applicationPid()));
    const QString part = QDir::tempPath() + "/" + name + ".part";
    const QString finalPath = QDir::tempPath() + "/" + name;
    const QString statePath = part + ".state";
    QFile::remove(part);
    QFile::remove(finalPath);
    QFile::remove(statePath);
    const TempPartGuard guard = {part, finalPath, statePath};
    (void)guard;

    // 上一版本留下的 .part：8192 字节；新文件只有 4096 字节
    {
        QFile stale(part);
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        ASSERT_EQ(8192, stale.write(QByteArray(8192, 'x')));
    }

    cm::netapi::NetClient net;
    cm::transfer::TransferQueue queue(&net, nullptr);
    const int id = queue.enqueueDownload(66, name, 4096, true);
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    // 1) 打开下载：单块、块大小 4096
    cm::protocol::DownloadOpenRs openRs;
    openRs.code = 0;
    openRs.fId = 66;
    openRs.size = 4096;
    openRs.fileMd5 = std::string(32, 'a');
    openRs.blockSize = 4096;
    openRs.blockCount = 1;
    openRs.ticket = std::string(32, 'b');  // 协议要求 32 位 hex 形式的票据
    std::string openBody;
    ASSERT_TRUE(openRs.encode(&openBody));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &queue, "onFrame", Qt::DirectConnection, Q_ARG(quint16, 10012),
        Q_ARG(quint32, net.lastSeq()), Q_ARG(QByteArray, fromStd(openBody))));
    ASSERT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    // 2) 取回第 0 块：4KB raw 数据（写入偏移 0，不会缩短文件）
    const quint32 rangeSeq = net.lastSeq();
    ASSERT_NE(0u, rangeSeq);
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &queue, "onFrame", Qt::DirectConnection, Q_ARG(quint16, 10011),
        Q_ARG(quint32, rangeSeq), Q_ARG(QByteArray, QByteArray(4096, 'n'))));
    EXPECT_EQ(cm::transfer::TaskState::Running, queue.find(id)->state);

    // 3) 完成上报：此时 .part 仍是 8192 字节，必须截到 4096 才算完成
    cm::protocol::DownloadDoneRs doneRs;
    doneRs.code = 0;
    doneRs.accepted = 1;
    std::string doneBody;
    ASSERT_TRUE(doneRs.encode(&doneBody));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &queue, "onFrame", Qt::DirectConnection, Q_ARG(quint16, 10041),
        Q_ARG(quint32, net.lastSeq()), Q_ARG(QByteArray, fromStd(doneBody))));

    EXPECT_EQ(cm::transfer::TaskState::Done, queue.find(id)->state);

    QFile out(finalPath);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(4096, out.size());  // 旧尾部已截掉
    out.close();
    EXPECT_FALSE(QFile::exists(part));  // 已重命名为正式文件
    QFile::remove(finalPath);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
