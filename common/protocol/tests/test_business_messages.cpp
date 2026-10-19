#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"
#include "cm/protocol/message_type.h"
#include "cm/protocol/messages/download.h"
#include "cm/protocol/messages/list.h"
#include "cm/protocol/messages/play.h"
#include "cm/protocol/messages/recycle.h"
#include "cm/protocol/messages/share.h"
#include "cm/protocol/messages/upload.h"
#include "cm/protocol/serialize.h"

namespace {

const char* kMd5 = "0123456789abcdef0123456789abcdef";
const char* kTaskId = "fedcba9876543210fedcba9876543210";
const char* kTicket =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
const char* kShareCode = "AB23CD";

// encode 后立即 decode，返回反序列化结果；断言失败时 gtest 会记录。
template <typename T>
T roundTrip(const T& in) {
    std::string payload;
    EXPECT_TRUE(in.encode(&payload));
    EXPECT_FALSE(payload.empty());
    T out;
    EXPECT_TRUE(out.decode(payload));
    return out;
}

TEST(BusinessMessagesTest, UploadInitRoundTrip) {
    cm::protocol::UploadInitRq rq;
    rq.parentId = 42;
    rq.fileName = "phase5.bin";
    rq.totalSize = 52428800;
    rq.fileMd5 = kMd5;
    rq.chunkSize = 4194304;
    rq.totalChunks = 13;
    rq.flags = 1;
    rq.reqId = 1001;

    const cm::protocol::UploadInitRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.parentId, rqOut.parentId);
    EXPECT_EQ(rq.fileName, rqOut.fileName);
    EXPECT_EQ(rq.totalSize, rqOut.totalSize);
    EXPECT_EQ(rq.fileMd5, rqOut.fileMd5);
    EXPECT_EQ(rq.chunkSize, rqOut.chunkSize);
    EXPECT_EQ(rq.totalChunks, rqOut.totalChunks);
    EXPECT_EQ(1, static_cast<int>(rqOut.flags));
    EXPECT_EQ(rq.reqId, rqOut.reqId);

    cm::protocol::UploadInitRs rs;
    rs.code = 0;
    rs.result = 1;
    rs.taskId = kTaskId;
    rs.fileId = 7;
    rs.ufId = 88;
    rs.fileName = "phase5 (2).bin";
    rs.chunkSize = 4194304;
    rs.totalChunks = 13;
    rs.expireAtMs = 1760000000000ULL;

    const cm::protocol::UploadInitRs rsOut = roundTrip(rs);
    EXPECT_EQ(rs.code, rsOut.code);
    EXPECT_EQ(1, static_cast<int>(rsOut.result));
    EXPECT_EQ(rs.taskId, rsOut.taskId);
    EXPECT_EQ(rs.fileId, rsOut.fileId);
    EXPECT_EQ(rs.ufId, rsOut.ufId);
    EXPECT_EQ(rs.fileName, rsOut.fileName);
    EXPECT_EQ(rs.chunkSize, rsOut.chunkSize);
    EXPECT_EQ(rs.totalChunks, rsOut.totalChunks);
    EXPECT_EQ(rs.expireAtMs, rsOut.expireAtMs);
}

TEST(BusinessMessagesTest, UploadChunkRoundTrip) {
    cm::protocol::UploadChunkRq rq;
    rq.taskId = kTaskId;
    rq.chunkIndex = 5;
    rq.offset = 5ULL * 4194304ULL;
    rq.chunkMd5 = kMd5;
    rq.data = std::string(1024, 'x');

    const cm::protocol::UploadChunkRq out = roundTrip(rq);
    EXPECT_EQ(rq.taskId, out.taskId);
    EXPECT_EQ(rq.chunkIndex, out.chunkIndex);
    EXPECT_EQ(rq.offset, out.offset);
    EXPECT_EQ(rq.chunkMd5, out.chunkMd5);
    EXPECT_EQ(rq.data.size(), out.data.size());
    EXPECT_EQ(rq.data, out.data);

    cm::protocol::UploadChunkRs rs;
    rs.code = 0;
    rs.chunkIndex = 5;
    rs.duplicate = 0;
    rs.doneChunks = 6;
    rs.doneBytes = 6ULL * 4194304ULL;
    rs.nextHint = 6;

    const cm::protocol::UploadChunkRs rsOut = roundTrip(rs);
    EXPECT_EQ(rs.chunkIndex, rsOut.chunkIndex);
    EXPECT_EQ(0, static_cast<int>(rsOut.duplicate));
    EXPECT_EQ(rs.doneChunks, rsOut.doneChunks);
    EXPECT_EQ(rs.doneBytes, rsOut.doneBytes);
    EXPECT_EQ(rs.nextHint, rsOut.nextHint);
}

TEST(BusinessMessagesTest, ContinueUploadBitmapRoundTrip) {
    cm::protocol::ContinueUploadRq rq;
    rq.taskId = kTaskId;
    const cm::protocol::ContinueUploadRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.taskId, rqOut.taskId);

    cm::protocol::ContinueUploadRs rs;
    rs.code = 0;
    rs.taskId = kTaskId;
    rs.status = 1;
    rs.chunkSize = 4194304;
    rs.totalChunks = 13;  // ceil(13/8) = 2 字节
    rs.doneChunks = 6;
    rs.doneBytes = 6ULL * 4194304ULL;
    rs.bitmap.assign(2, '\0');
    rs.bitmap[0] = static_cast<char>(0x3F);  // 前 6 片完成（LSB-first）
    rs.expireAtMs = 1760000000000ULL;

    const cm::protocol::ContinueUploadRs out = roundTrip(rs);
    EXPECT_EQ(rs.taskId, out.taskId);
    EXPECT_EQ(1, static_cast<int>(out.status));
    EXPECT_EQ(rs.chunkSize, out.chunkSize);
    EXPECT_EQ(rs.totalChunks, out.totalChunks);
    EXPECT_EQ(rs.doneChunks, out.doneChunks);
    EXPECT_EQ(rs.doneBytes, out.doneBytes);
    ASSERT_EQ(std::size_t(2), out.bitmap.size());
    EXPECT_EQ(0x3F, static_cast<int>(static_cast<unsigned char>(out.bitmap[0])));
    EXPECT_EQ(0, static_cast<int>(static_cast<unsigned char>(out.bitmap[1])));
    EXPECT_EQ(rs.expireAtMs, out.expireAtMs);
}

TEST(BusinessMessagesTest, UploadCompleteBadIndexesRoundTrip) {
    cm::protocol::UploadCompleteRq rq;
    rq.taskId = kTaskId;
    const cm::protocol::UploadCompleteRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.taskId, rqOut.taskId);

    cm::protocol::UploadCompleteRs rs;
    rs.code = 3002;
    rs.fileId = 9;
    rs.ufId = 90;
    rs.fileName = "phase5.bin";
    rs.fileMd5 = kMd5;
    rs.size = 52428800;
    rs.badIndexes.push_back(1);
    rs.badIndexes.push_back(3);
    rs.badIndexes.push_back(7);

    const cm::protocol::UploadCompleteRs out = roundTrip(rs);
    EXPECT_EQ(rs.code, out.code);
    EXPECT_EQ(rs.fileId, out.fileId);
    EXPECT_EQ(rs.ufId, out.ufId);
    EXPECT_EQ(rs.fileName, out.fileName);
    EXPECT_EQ(rs.fileMd5, out.fileMd5);
    EXPECT_EQ(rs.size, out.size);
    ASSERT_EQ(std::size_t(3), out.badIndexes.size());
    EXPECT_EQ(1u, out.badIndexes[0]);
    EXPECT_EQ(3u, out.badIndexes[1]);
    EXPECT_EQ(7u, out.badIndexes[2]);

    // 超过 1024 截断：只写前 1024 条，服务层据此把 code 置 1001。
    cm::protocol::UploadCompleteRs big;
    big.code = 3002;
    for (std::uint32_t i = 0; i < 1100; ++i) {
        big.badIndexes.push_back(i);
    }
    std::string payload;
    ASSERT_TRUE(big.encode(&payload));
    cm::protocol::UploadCompleteRs bigOut;
    ASSERT_TRUE(bigOut.decode(payload));
    EXPECT_EQ(std::size_t(1024), bigOut.badIndexes.size());
    EXPECT_EQ(1023u, bigOut.badIndexes[1023]);
}

TEST(BusinessMessagesTest, QuickUploadRoundTrip) {
    cm::protocol::QuickUploadRq rq;
    rq.fileMd5 = kMd5;
    rq.totalSize = 52428800;
    const cm::protocol::QuickUploadRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.fileMd5, rqOut.fileMd5);
    EXPECT_EQ(rq.totalSize, rqOut.totalSize);

    cm::protocol::QuickUploadRs rs;
    rs.code = 0;
    rs.hit = 1;
    rs.fileId = 12;
    rs.size = 52428800;
    const cm::protocol::QuickUploadRs rsOut = roundTrip(rs);
    EXPECT_EQ(1, static_cast<int>(rsOut.hit));
    EXPECT_EQ(rs.fileId, rsOut.fileId);
    EXPECT_EQ(rs.size, rsOut.size);
}

TEST(BusinessMessagesTest, DownloadOpenRoundTrip) {
    cm::protocol::DownloadOpenRq rq;
    rq.ufId = 123;
    rq.shareTicket = "";
    rq.startBlock = 4;
    const cm::protocol::DownloadOpenRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);
    EXPECT_TRUE(rqOut.shareTicket.empty());
    EXPECT_EQ(rq.startBlock, rqOut.startBlock);

    cm::protocol::DownloadOpenRs rs;
    rs.code = 0;
    rs.fId = 55;
    rs.size = 52428800;
    rs.fileMd5 = kMd5;
    rs.blockSize = 4194304;
    rs.blockCount = 13;
    rs.ticket = kTicket;
    rs.ticketExpireMs = 1760000300000ULL;
    rs.startBlock = 4;

    const cm::protocol::DownloadOpenRs out = roundTrip(rs);
    EXPECT_EQ(rs.fId, out.fId);
    EXPECT_EQ(rs.size, out.size);
    EXPECT_EQ(rs.fileMd5, out.fileMd5);
    EXPECT_EQ(rs.blockSize, out.blockSize);
    EXPECT_EQ(rs.blockCount, out.blockCount);
    EXPECT_EQ(rs.ticket, out.ticket);
    EXPECT_EQ(rs.ticketExpireMs, out.ticketExpireMs);
    EXPECT_EQ(rs.startBlock, out.startBlock);
}

TEST(BusinessMessagesTest, DownloadStatRoundTrip) {
    cm::protocol::DownloadStatRq rq;
    rq.ufId = 321;
    const cm::protocol::DownloadStatRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);

    cm::protocol::DownloadStatRs rs;
    rs.code = 0;
    rs.fId = 55;
    rs.size = 52428800;
    rs.fileMd5 = kMd5;
    rs.blockSize = 4194304;
    rs.blockCount = 13;
    rs.state = 1;
    rs.updatedAtMs = 1760000000000ULL;

    const cm::protocol::DownloadStatRs out = roundTrip(rs);
    EXPECT_EQ(rs.fId, out.fId);
    EXPECT_EQ(rs.size, out.size);
    EXPECT_EQ(rs.fileMd5, out.fileMd5);
    EXPECT_EQ(rs.blockSize, out.blockSize);
    EXPECT_EQ(rs.blockCount, out.blockCount);
    EXPECT_EQ(1, static_cast<int>(out.state));
    EXPECT_EQ(rs.updatedAtMs, out.updatedAtMs);
}

TEST(BusinessMessagesTest, DownloadRangeAndDoneRoundTrip) {
    cm::protocol::DownloadRangeRq rq;
    rq.ticket = kTicket;
    rq.fId = 55;
    rq.blockIndex = 12;
    const cm::protocol::DownloadRangeRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ticket, rqOut.ticket);
    EXPECT_EQ(rq.fId, rqOut.fId);
    EXPECT_EQ(rq.blockIndex, rqOut.blockIndex);

    cm::protocol::DownloadDoneRq done;
    done.ticket = kTicket;
    done.fId = 55;
    done.size = 52428800;
    done.fileMd5 = kMd5;
    done.verifyOk = 1;
    const cm::protocol::DownloadDoneRq doneOut = roundTrip(done);
    EXPECT_EQ(done.ticket, doneOut.ticket);
    EXPECT_EQ(done.fId, doneOut.fId);
    EXPECT_EQ(done.fileMd5, doneOut.fileMd5);
    EXPECT_EQ(1, static_cast<int>(doneOut.verifyOk));

    cm::protocol::DownloadDoneRs rs;
    rs.code = 0;
    rs.accepted = 1;
    const cm::protocol::DownloadDoneRs rsOut = roundTrip(rs);
    EXPECT_EQ(1, static_cast<int>(rsOut.accepted));
}

TEST(BusinessMessagesTest, ShareCreateRoundTrip) {
    cm::protocol::ShareCreateRq rq;
    rq.ufId = 88;
    rq.expirePolicy = 2;
    rq.accessMode = 1;
    rq.reqId = 7;
    const cm::protocol::ShareCreateRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);
    EXPECT_EQ(2, static_cast<int>(rqOut.expirePolicy));
    EXPECT_EQ(1, static_cast<int>(rqOut.accessMode));
    EXPECT_EQ(rq.reqId, rqOut.reqId);

    cm::protocol::ShareCreateRs rs;
    rs.code = 0;
    rs.shareCode = kShareCode;
    rs.accessCode = "1234";
    rs.accessCodeRequired = 1;
    rs.expireAtMs = 1760000000000ULL;

    const cm::protocol::ShareCreateRs out = roundTrip(rs);
    EXPECT_EQ(rs.shareCode, out.shareCode);
    EXPECT_EQ(rs.accessCode, out.accessCode);
    EXPECT_EQ(1, static_cast<int>(out.accessCodeRequired));
    EXPECT_EQ(rs.expireAtMs, out.expireAtMs);
}

TEST(BusinessMessagesTest, ShareListRoundTrip) {
    cm::protocol::ShareListRq rq;
    rq.cursorCreatedMs = 1760000000000ULL;
    rq.cursorId = 3;
    rq.limit = 50;
    const cm::protocol::ShareListRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.cursorCreatedMs, rqOut.cursorCreatedMs);
    EXPECT_EQ(rq.cursorId, rqOut.cursorId);
    EXPECT_EQ(rq.limit, rqOut.limit);

    cm::protocol::ShareListRs rs;
    rs.code = 0;
    for (int i = 0; i < 2; ++i) {
        cm::protocol::ShareListItem item;
        item.shareCode = kShareCode;
        item.fileName = "video.mp4";
        item.size = 1024;
        item.accessCodeRequired = 1;
        item.status = 0;
        item.expireAtMs = 1760000000000ULL;
        item.createdAtMs = 1759990000000ULL;
        rs.items.push_back(item);
    }
    rs.hasMore = 1;
    rs.nextCreatedMs = 1759990000000ULL;
    rs.nextId = 9;

    const cm::protocol::ShareListRs out = roundTrip(rs);
    ASSERT_EQ(std::size_t(2), out.items.size());
    EXPECT_EQ(rs.items[0].shareCode, out.items[0].shareCode);
    EXPECT_EQ(rs.items[0].fileName, out.items[0].fileName);
    EXPECT_EQ(rs.items[0].size, out.items[0].size);
    EXPECT_EQ(1, static_cast<int>(out.items[0].accessCodeRequired));
    EXPECT_EQ(1, static_cast<int>(out.hasMore));
    EXPECT_EQ(rs.nextCreatedMs, out.nextCreatedMs);
    EXPECT_EQ(rs.nextId, out.nextId);
}

TEST(BusinessMessagesTest, ShareOpenRoundTrip) {
    cm::protocol::ShareOpenRq rq;
    rq.shareCode = kShareCode;
    rq.accessCode = "9876";
    const cm::protocol::ShareOpenRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.shareCode, rqOut.shareCode);
    EXPECT_EQ(rq.accessCode, rqOut.accessCode);

    cm::protocol::ShareOpenRs rs;
    rs.code = 0;
    rs.fId = 55;
    rs.fileName = "video.mp4";
    rs.size = 1024;
    rs.fileMd5 = kMd5;
    rs.isDir = 0;
    rs.expireAtMs = 1760000000000ULL;
    rs.shareTicket = kTicket;
    rs.accessCodeRequired = 1;

    const cm::protocol::ShareOpenRs out = roundTrip(rs);
    EXPECT_EQ(rs.fId, out.fId);
    EXPECT_EQ(rs.fileName, out.fileName);
    EXPECT_EQ(rs.size, out.size);
    EXPECT_EQ(rs.fileMd5, out.fileMd5);
    EXPECT_EQ(0, static_cast<int>(out.isDir));
    EXPECT_EQ(rs.expireAtMs, out.expireAtMs);
    EXPECT_EQ(rs.shareTicket, out.shareTicket);
    EXPECT_EQ(1, static_cast<int>(out.accessCodeRequired));
}

TEST(BusinessMessagesTest, ShareCancelRoundTrip) {
    cm::protocol::ShareCancelRq rq;
    rq.shareCode = kShareCode;
    const cm::protocol::ShareCancelRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.shareCode, rqOut.shareCode);

    cm::protocol::ShareCancelRs rs;
    rs.code = 0;
    rs.cancelled = 0;
    rs.already = 1;
    const cm::protocol::ShareCancelRs out = roundTrip(rs);
    EXPECT_EQ(0, static_cast<int>(out.cancelled));
    EXPECT_EQ(1, static_cast<int>(out.already));
}

TEST(BusinessMessagesTest, RecycleDeleteFileRoundTrip) {
    cm::protocol::DeleteFileRq rq;
    rq.ufId = 77;
    const cm::protocol::DeleteFileRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);

    cm::protocol::DeleteFileRs rs;
    rs.code = 0;
    rs.deleted = 1;
    rs.already = 0;
    const cm::protocol::DeleteFileRs out = roundTrip(rs);
    EXPECT_EQ(1, static_cast<int>(out.deleted));
    EXPECT_EQ(0, static_cast<int>(out.already));
}

TEST(BusinessMessagesTest, RecycleListRoundTrip) {
    cm::protocol::RecycleListRq rq;
    rq.limit = 50;
    rq.cursorDeletedMs = 1760000000000ULL;
    rq.cursorId = 4;
    const cm::protocol::RecycleListRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.limit, rqOut.limit);
    EXPECT_EQ(rq.cursorDeletedMs, rqOut.cursorDeletedMs);
    EXPECT_EQ(rq.cursorId, rqOut.cursorId);

    cm::protocol::RecycleListRs rs;
    rs.code = 0;
    cm::protocol::RecycleItem item;
    item.ufId = 7;
    item.name = "old.bin";
    item.isDir = 0;
    item.size = 2048;
    item.deletedAtMs = 1760000000000ULL;
    item.parentId = 0;
    rs.items.push_back(item);
    rs.hasMore = 0;
    rs.nextDeletedMs = 1760000000000ULL;
    rs.nextId = 7;

    const cm::protocol::RecycleListRs out = roundTrip(rs);
    ASSERT_EQ(std::size_t(1), out.items.size());
    EXPECT_EQ(item.ufId, out.items[0].ufId);
    EXPECT_EQ(item.name, out.items[0].name);
    EXPECT_EQ(0, static_cast<int>(out.items[0].isDir));
    EXPECT_EQ(item.size, out.items[0].size);
    EXPECT_EQ(item.deletedAtMs, out.items[0].deletedAtMs);
    EXPECT_EQ(0, static_cast<int>(out.hasMore));
    EXPECT_EQ(rs.nextId, out.nextId);
}

TEST(BusinessMessagesTest, RestoreRoundTrip) {
    cm::protocol::RestoreRq rq;
    rq.ufId = 77;
    rq.newName = "";
    rq.autoRename = 1;
    const cm::protocol::RestoreRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);
    EXPECT_TRUE(rqOut.newName.empty());
    EXPECT_EQ(1, static_cast<int>(rqOut.autoRename));

    cm::protocol::RestoreRs rs;
    rs.code = 0;
    rs.ufId = 77;
    rs.parentId = 3;
    rs.name = "old (2).bin";
    rs.renamed = 1;
    const cm::protocol::RestoreRs out = roundTrip(rs);
    EXPECT_EQ(rs.ufId, out.ufId);
    EXPECT_EQ(rs.parentId, out.parentId);
    EXPECT_EQ(rs.name, out.name);
    EXPECT_EQ(1, static_cast<int>(out.renamed));
}

TEST(BusinessMessagesTest, PurgeRoundTrip) {
    cm::protocol::PurgeRq rq;
    rq.ufId = 77;
    rq.confirm = 1;
    const cm::protocol::PurgeRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.ufId, rqOut.ufId);
    EXPECT_EQ(1, static_cast<int>(rqOut.confirm));

    // confirm 必须是 1，否则连序列化都拒绝。
    cm::protocol::PurgeRq bad;
    bad.ufId = 77;
    bad.confirm = 0;
    std::string payload;
    EXPECT_FALSE(bad.encode(&payload));

    cm::protocol::PurgeRs rs;
    rs.code = 0;
    rs.filesDeleted = 3;
    rs.dirsDeleted = 1;
    rs.blobsOrphaned = 2;
    const cm::protocol::PurgeRs out = roundTrip(rs);
    EXPECT_EQ(rs.filesDeleted, out.filesDeleted);
    EXPECT_EQ(rs.dirsDeleted, out.dirsDeleted);
    EXPECT_EQ(rs.blobsOrphaned, out.blobsOrphaned);
}

TEST(BusinessMessagesTest, ListFileRoundTrip) {
    cm::protocol::ListFileRq rq;
    rq.parentId = 0;
    rq.sort = 1;
    rq.limit = 50;
    rq.cursorKey = "1760000000000";
    rq.cursorId = 12;
    const cm::protocol::ListFileRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.parentId, rqOut.parentId);
    EXPECT_EQ(1, static_cast<int>(rqOut.sort));
    EXPECT_EQ(rq.limit, rqOut.limit);
    EXPECT_EQ(rq.cursorKey, rqOut.cursorKey);
    EXPECT_EQ(rq.cursorId, rqOut.cursorId);

    cm::protocol::ListFileRs rs;
    rs.code = 0;
    cm::protocol::FileItem file;
    file.ufId = 10;
    file.fId = 5;
    file.name = "a.bin";
    file.isDir = 0;
    file.size = 4096;
    file.md5 = kMd5;
    file.createdAtMs = 1760000000000ULL;
    file.updatedAtMs = 1760000000001ULL;
    rs.items.push_back(file);

    cm::protocol::FileItem dir;
    dir.ufId = 11;
    dir.fId = 0;
    dir.name = "movies";
    dir.isDir = 1;
    dir.size = 0;
    dir.md5 = "";
    dir.createdAtMs = 1760000000002ULL;
    dir.updatedAtMs = 1760000000003ULL;
    rs.items.push_back(dir);

    rs.hasMore = 1;
    rs.nextKey = "movies";
    rs.nextId = 11;
    rs.totalCount = 2;

    const cm::protocol::ListFileRs out = roundTrip(rs);
    ASSERT_EQ(std::size_t(2), out.items.size());
    EXPECT_EQ(file.ufId, out.items[0].ufId);
    EXPECT_EQ(file.fId, out.items[0].fId);
    EXPECT_EQ(file.name, out.items[0].name);
    EXPECT_EQ(file.md5, out.items[0].md5);
    EXPECT_EQ(dir.name, out.items[1].name);
    EXPECT_EQ(1, static_cast<int>(out.items[1].isDir));
    EXPECT_TRUE(out.items[1].md5.empty());
    EXPECT_EQ(1, static_cast<int>(out.hasMore));
    EXPECT_EQ(rs.nextKey, out.nextKey);
    EXPECT_EQ(rs.nextId, out.nextId);
    EXPECT_EQ(rs.totalCount, out.totalCount);
}

TEST(BusinessMessagesTest, PlayReportRoundTrip) {
    cm::protocol::PlayReportRq rq;
    rq.fId = 55;
    rq.positionMs = 30000;
    rq.durationMs = 600000;
    rq.finished = 0;
    const cm::protocol::PlayReportRq rqOut = roundTrip(rq);
    EXPECT_EQ(rq.fId, rqOut.fId);
    EXPECT_EQ(rq.positionMs, rqOut.positionMs);
    EXPECT_EQ(rq.durationMs, rqOut.durationMs);
    EXPECT_EQ(0, static_cast<int>(rqOut.finished));

    cm::protocol::PlayReportRs rs;
    rs.code = 0;
    rs.saved = 1;
    const cm::protocol::PlayReportRs out = roundTrip(rs);
    EXPECT_EQ(1, static_cast<int>(out.saved));
}

TEST(BusinessMessagesTest, BusinessMessageRejectsBadMd5) {
    cm::protocol::UploadInitRq rq;
    rq.fileName = "a.bin";
    rq.totalSize = 10;
    rq.fileMd5 = "0123456789abcdef0123456789abcde";  // 31 位
    std::string payload;
    EXPECT_FALSE(rq.encode(&payload));

    rq.fileMd5 = "0123456789abcdef0123456789abcdeZ";  // 非 hex
    EXPECT_FALSE(rq.encode(&payload));

    cm::protocol::QuickUploadRq quick;
    quick.fileMd5 = "zz";
    quick.totalSize = 10;
    EXPECT_FALSE(quick.encode(&payload));

    // 手工拼一个 md5 非法的 payload，解码侧也必须拒绝。
    cm::protocol::Writer writer;
    ASSERT_TRUE(writer.writeU64(0));
    ASSERT_TRUE(writer.writeString("a.bin"));
    ASSERT_TRUE(writer.writeU64(10));
    ASSERT_TRUE(writer.writeString("0123456789abcdef0123456789abcdeZ"));
    ASSERT_TRUE(writer.writeU32(4194304));
    ASSERT_TRUE(writer.writeU32(1));
    ASSERT_TRUE(writer.writeU8(0));
    ASSERT_TRUE(writer.writeU32(1));

    cm::protocol::UploadInitRq decoded;
    EXPECT_FALSE(decoded.decode(writer.data()));

    // chunk_md5 同样校验。
    cm::protocol::UploadChunkRq chunk;
    chunk.taskId = kTaskId;
    chunk.chunkMd5 = "bad";
    chunk.data = "x";
    EXPECT_FALSE(chunk.encode(&payload));
}

TEST(BusinessMessagesTest, BusinessMessageRejectsTruncated) {
    cm::protocol::DownloadOpenRs rs;
    rs.code = 0;
    rs.fId = 1;
    rs.size = 10;
    rs.fileMd5 = kMd5;
    rs.blockSize = 4194304;
    rs.blockCount = 1;
    rs.ticket = kTicket;
    rs.ticketExpireMs = 1;
    rs.startBlock = 0;

    std::string payload;
    ASSERT_TRUE(rs.encode(&payload));
    ASSERT_GT(payload.size(), std::size_t(1));

    cm::protocol::DownloadOpenRs out;
    EXPECT_FALSE(out.decode(payload.substr(0, payload.size() - 1)));
    EXPECT_FALSE(out.decode(""));
    EXPECT_FALSE(out.decode(payload.substr(0, 4)));
}

TEST(BusinessMessagesTest, BusinessMessageRejectsBitmapTooLong) {
    cm::protocol::ContinueUploadRs rs;
    rs.taskId = kTaskId;
    rs.status = 1;
    rs.chunkSize = 4194304;
    rs.totalChunks = 8;  // 期望 1 字节
    rs.doneChunks = 0;
    rs.doneBytes = 0;
    rs.bitmap.assign(2, '\0');  // 多了 1 字节
    rs.expireAtMs = 1;
    std::string payload;
    EXPECT_FALSE(rs.encode(&payload));

    // totalChunks 超过 524288 直接拒绝（位图会超过 64KB）。
    rs.totalChunks = cm::protocol::kMaxChunkCount + 1;
    rs.bitmap.assign(1, '\0');
    EXPECT_FALSE(rs.encode(&payload));

    // 手工拼一个 bitmap 长度不符的 payload，解码侧也必须拒绝。
    cm::protocol::Writer writer;
    ASSERT_TRUE(writer.writeU32(0));
    ASSERT_TRUE(writer.writeString(kTaskId));
    ASSERT_TRUE(writer.writeU8(1));
    ASSERT_TRUE(writer.writeU32(4194304));
    ASSERT_TRUE(writer.writeU32(8));
    ASSERT_TRUE(writer.writeU32(0));
    ASSERT_TRUE(writer.writeU64(0));
    ASSERT_TRUE(writer.writeU32(2));  // 期望 1 字节
    ASSERT_TRUE(writer.writeBytes("\0\0", 2));
    ASSERT_TRUE(writer.writeU64(1));

    cm::protocol::ContinueUploadRs decoded;
    EXPECT_FALSE(decoded.decode(writer.data()));
}

TEST(BusinessMessagesTest, MessageTypeTable) {
    using cm::protocol::MessageType;

    EXPECT_EQ(10011u, static_cast<std::uint32_t>(MessageType::DownloadDataRs));
    EXPECT_EQ(10017u, static_cast<std::uint32_t>(MessageType::QuickUploadRq));
    EXPECT_EQ(10024u, static_cast<std::uint32_t>(MessageType::QuickUploadRs));
    EXPECT_EQ(10030u, static_cast<std::uint32_t>(MessageType::UploadCompleteRq));
    EXPECT_EQ(10031u, static_cast<std::uint32_t>(MessageType::UploadCompleteRs));
    EXPECT_EQ(10032u, static_cast<std::uint32_t>(MessageType::PlayReportRq));
    EXPECT_EQ(10033u, static_cast<std::uint32_t>(MessageType::PlayReportRs));
    EXPECT_EQ(10034u, static_cast<std::uint32_t>(MessageType::RecycleListRq));
    EXPECT_EQ(10035u, static_cast<std::uint32_t>(MessageType::RecycleListRs));
    EXPECT_EQ(10036u, static_cast<std::uint32_t>(MessageType::RestoreRq));
    EXPECT_EQ(10037u, static_cast<std::uint32_t>(MessageType::RestoreRs));
    EXPECT_EQ(10038u, static_cast<std::uint32_t>(MessageType::PurgeRq));
    EXPECT_EQ(10039u, static_cast<std::uint32_t>(MessageType::PurgeRs));
    EXPECT_EQ(10040u, static_cast<std::uint32_t>(MessageType::DownloadDoneRq));
    EXPECT_EQ(10041u, static_cast<std::uint32_t>(MessageType::DownloadDoneRs));
    EXPECT_EQ(10042u, static_cast<std::uint32_t>(MessageType::ShareCancelRq));
    EXPECT_EQ(10043u, static_cast<std::uint32_t>(MessageType::ShareCancelRs));

    EXPECT_TRUE(cm::protocol::isKnownType(10000));
    EXPECT_TRUE(cm::protocol::isKnownType(10043));
    EXPECT_FALSE(cm::protocol::isKnownType(10044));
    EXPECT_FALSE(cm::protocol::isKnownType(19999));
}

TEST(BusinessMessagesTest, ErrorCodeNewValuesUnique) {
    EXPECT_EQ(3007, static_cast<int>(cm::ErrorCode::TaskExpired));
    EXPECT_EQ(3008, static_cast<int>(cm::ErrorCode::TooManyNodes));
    EXPECT_NE(cm::ErrorCode::TaskExpired, cm::ErrorCode::RateLimited);
    EXPECT_NE(cm::ErrorCode::TooManyNodes, cm::ErrorCode::TaskStateInvalid);

    EXPECT_EQ(std::string("TaskExpired"), cm::to_string(cm::ErrorCode::TaskExpired));
    EXPECT_EQ(std::string("TooManyNodes"), cm::to_string(cm::ErrorCode::TooManyNodes));

    // 3007 需要重新 init，3008 需要分批删除，都不是“原样重试”能解决的。
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::TaskExpired));
    EXPECT_FALSE(cm::is_retryable(cm::ErrorCode::TooManyNodes));
}

}  // namespace
