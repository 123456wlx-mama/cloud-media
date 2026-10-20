// RecycleFlow 状态机单测：删除 → 撤销（恢复）/ 回收站列表 / 彻底删除。
#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QString>

#include <string>

#include "cm/protocol/messages/recycle.h"
#include "recycleapi/RecycleFlow.h"

namespace {

QByteArray bytesOf(const std::string& payload) {
    return QByteArray(payload.data(), static_cast<int>(payload.size()));
}

QByteArray deleteRs(quint32 code, bool deleted, bool already) {
    cm::protocol::DeleteFileRs rs;
    rs.code = code;
    rs.deleted = deleted ? 1 : 0;
    rs.already = already ? 1 : 0;
    std::string payload;
    rs.encode(&payload);
    return bytesOf(payload);
}

QByteArray restoreRs(quint32 code, quint64 ufId, const char* name, bool renamed) {
    cm::protocol::RestoreRs rs;
    rs.code = code;
    rs.ufId = ufId;
    rs.name = name;
    rs.renamed = renamed ? 1 : 0;
    std::string payload;
    rs.encode(&payload);
    return bytesOf(payload);
}

}  // namespace

TEST(RecycleFlowTest, DeleteThenUndoRestores) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    int deleted = 0;
    int restored = 0;
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::deleted,
                     [&deleted](quint64, const QString&) { ++deleted; });
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::restored,
                     [&restored](quint64, const QString&) { ++restored; });

    flow.deleteFile(30670, QString::fromUtf8("p82_big.bin"));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Deleting, flow.phase());

    flow.applyDeleteRs(deleteRs(0, true, false));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Deleted, flow.phase());
    EXPECT_TRUE(flow.canUndo());
    EXPECT_EQ(1, deleted);

    flow.undo();  // 撤销 = RestoreRq
    EXPECT_EQ(cm::recycleapi::DeletePhase::Restoring, flow.phase());
    flow.applyRestoreRs(restoreRs(0, 30670, "p82_big.bin", false));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Restored, flow.phase());
    EXPECT_FALSE(flow.canUndo());
    EXPECT_EQ(1, restored);
}

TEST(RecycleFlowTest, DeleteAlreadyGoneHasNoUndo) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    int deleted = 0;
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::deleted,
                     [&deleted](quint64, const QString&) { ++deleted; });
    flow.deleteFile(7, QString::fromUtf8("x.bin"));
    flow.applyDeleteRs(deleteRs(0, true, true));
    EXPECT_FALSE(flow.canUndo());
    EXPECT_EQ(0, deleted);
}

TEST(RecycleFlowTest, DeleteRejectedReportsFailureText) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    QString failedText;
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::failed,
                     [&failedText](const QString& text) { failedText = text; });
    flow.deleteFile(9, QString::fromUtf8("y.bin"));
    flow.applyDeleteRs(deleteRs(1002, false, false));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Failed, flow.phase());
    EXPECT_EQ(QString::fromUtf8("文件不存在或已删除"), failedText);
}

TEST(RecycleFlowTest, RestoreRenamedUpdatesMessage) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    flow.restore(9);
    flow.applyRestoreRs(restoreRs(0, 9, "y (1).bin", true));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Restored, flow.phase());
    EXPECT_TRUE(flow.message().contains(QString::fromUtf8("y (1).bin")));
}

TEST(RecycleFlowTest, ListParsesItems) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    cm::protocol::RecycleListRs rs;
    rs.code = 0;
    cm::protocol::RecycleItem a;
    a.ufId = 11;
    a.name = "a.bin";
    a.size = 1024;
    cm::protocol::RecycleItem b;
    b.ufId = 12;
    b.name = "b.bin";
    b.size = 2048;
    rs.items.push_back(a);
    rs.items.push_back(b);
    std::string payload;
    rs.encode(&payload);

    int changes = 0;
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::listChanged,
                     [&changes]() { ++changes; });
    flow.applyListRs(bytesOf(payload));
    ASSERT_EQ(2, flow.entries().size());
    EXPECT_EQ(11ULL, flow.entries().at(0).ufId);
    EXPECT_EQ(QString::fromUtf8("b.bin"), flow.entries().at(1).name);
    EXPECT_EQ(1, changes);
}

TEST(RecycleFlowTest, PurgeMovesToPurged) {
    cm::recycleapi::RecycleFlow flow(nullptr);
    quint64 purged = 0;
    QObject::connect(&flow, &cm::recycleapi::RecycleFlow::purged,
                     [&purged](quint64 ufId) { purged = ufId; });
    flow.deleteFile(11, QString::fromUtf8("a.bin"));
    flow.applyDeleteRs(deleteRs(0, true, false));
    flow.purge(11);
    EXPECT_EQ(cm::recycleapi::DeletePhase::Purging, flow.phase());

    cm::protocol::PurgeRs rs;
    rs.code = 0;
    rs.filesDeleted = 1;
    std::string payload;
    rs.encode(&payload);
    flow.applyPurgeRs(bytesOf(payload));
    EXPECT_EQ(cm::recycleapi::DeletePhase::Purged, flow.phase());
    EXPECT_EQ(1u, rs.filesDeleted);
    EXPECT_EQ(11ULL, purged);  // purged 信号带回被彻底删除的 ufId
}
