#include "logic/sendfile_block_sender.h"

#if defined(CM_DOWNLOAD_ZEROCOPY) && (CM_DOWNLOAD_ZEROCOPY)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "db/file_dao.h"
#include "db/mysql_pool.h"
#include "logic/block_crc.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kChunkSize = 4194304;

// 组 19B 帧头：CRC = crc32Combine(crc32(header11), blockCrc, len)。
// header11 = version+type+len+seq 共 11 字节；payload 用 sidecar 块 CRC 合成，不读 payload。
void makeHeader19(std::uint32_t seq, std::uint32_t len, std::uint32_t blockCrc,
                  std::string* header) {
    header->assign(cm::protocol::kHeaderSize, '\0');
    std::uint8_t* h = reinterpret_cast<std::uint8_t*>(&(*header)[0]);
    cm::writeU32BE(h, cm::protocol::kMagic);
    h[4] = static_cast<std::uint8_t>(cm::protocol::kVersion);
    cm::writeU16BE(
        h + 5,
        static_cast<std::uint16_t>(cm::protocol::MessageType::DownloadDataRs));
    cm::writeU32BE(h + 7, len);
    cm::writeU32BE(h + 11, seq);
    // CRC 字段（offset 15~18）最后写；只覆盖 header[4..14] + payload。
    const std::uint32_t header11Crc = cm::crc32(h + 4, 11);
    const std::uint32_t frameCrc = cm::crc32Combine(header11Crc, blockCrc, len);
    cm::writeU32BE(h + 15, frameCrc);
}

}  // namespace

SendfileBlockSender::SendfileBlockSender(cm::db::MysqlPool& mysql,
                                         cm::cache::RedisPool& redis,
                                         Executor executor,
                                         const std::string& dataDir)
    : m_mysql(mysql),
      m_ticket(redis),
      m_executor(std::move(executor)),
      m_dataDir(dataDir) {
}

void SendfileBlockSender::sendBlock(std::uint64_t uid,
                                    const std::string& ticket,
                                    std::uint64_t fid,
                                    std::uint32_t blockIndex,
                                    std::uint32_t seq,
                                    Deliver deliver,
                                    Fallback fallback,
                                    DoneCallback done) {
    if (!done) {
        return;
    }

    // 1. 票据校验 + 滑动续期（Redis，业务线程）。uid/fid 不符 → 1004。
    DownloadTicketInfo info;
    ErrorCode rc = m_ticket.touch(ticket, uid, fid, &info);
    if (rc != ErrorCode::Ok) {
        done(rc);
        return;
    }

    // 2. 查 blob（MySQL，业务线程）。
    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        rc = (m_mysql.lastError() == ErrorCode::Ok) ? ErrorCode::DbPoolTimeout
                                                    : m_mysql.lastError();
        done(rc);
        return;
    }
    cm::db::FileDao dao(*conn);
    cm::db::BlobRow blob;
    rc = dao.getBlobById(fid, &blob);
    if (rc == ErrorCode::NotFound) {
        done(ErrorCode::FileGone);
        return;
    }
    if (rc != ErrorCode::Ok) {
        done(rc);
        return;
    }
    if (blob.state != 1 || blob.size == 0) {
        done(ErrorCode::FileGone);
        return;
    }

    // 3. 块范围校验：offset = index*4MB，越界回 1001。
    const std::uint64_t blockCount =
        (blob.size + kChunkSize - 1) / kChunkSize;
    if (static_cast<std::uint64_t>(blockIndex) >= blockCount) {
        done(ErrorCode::InvalidArgument);
        return;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(blockIndex) * kChunkSize;
    const std::uint32_t len = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(kChunkSize, blob.size - offset));

    WorkItem item;
    item.blobPath = m_dataDir + "/blobs/" + blob.path;
    item.size = blob.size;
    item.offset = offset;
    item.len = len;
    item.blockIndex = blockIndex;
    item.seq = seq;
    item.uid = uid;
    item.ticket = ticket;
    item.fid = fid;
    item.deliver = std::move(deliver);
    item.fallback = std::move(fallback);
    item.done = std::move(done);

    // 4. 查 sidecar 块 CRC（业务线程，小读）。
    std::vector<std::uint32_t> crcs;
    bool missing = false;
    rc = readBlockCrc(item.blobPath, item.size, &crcs, &missing);
    if (rc != ErrorCode::Ok) {
        item.done(rc);
        return;
    }
    if (!missing && blockIndex < crcs.size() && crcs[blockIndex] != 0) {
        // 已知：open + fstat + 组头 + deliver（同步，业务线程）。
        openAndDeliver(item, crcs[blockIndex]);
        return;
    }

    // 5. 缺失/为 0：投 IO 池懒填充（读块算 CRC pwrite），失败回退缓冲。
    if (!m_executor([this, item]() { fillAndDeliver(item); })) {
        item.done(ErrorCode::Timeout);
    }
}

void SendfileBlockSender::openAndDeliver(const WorkItem& item,
                                         std::uint32_t blockCrc) {
    const int fd = ::open(item.blobPath.c_str(), O_RDONLY);
    if (fd < 0) {
        item.done(ErrorCode::FileGone);
        return;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        static_cast<std::uint64_t>(st.st_size) != item.size) {
        ::close(fd);
        item.done(ErrorCode::FileGone);
        return;
    }

    std::string header;
    makeHeader19(item.seq, item.len, blockCrc, &header);

    bool accepted = false;
    if (item.deliver) {
        accepted = item.deliver(header, fd,
                                static_cast<std::int64_t>(item.offset),
                                item.len);
    }
    if (accepted) {
        item.done(ErrorCode::Ok);  // 连接接管 fd
    } else {
        ::close(fd);
        item.done(ErrorCode::NetworkError);
    }
}

void SendfileBlockSender::fillAndDeliver(const WorkItem& item) {
    std::uint32_t blockCrc = 0;
    const ErrorCode rc =
        ensureBlockCrc(item.blobPath, item.size, item.blockIndex, &blockCrc);
    if (rc != ErrorCode::Ok) {
        // 懒填充失败：回退缓冲路径，不阻断下载。fd 尚未打开，无需 close。
        if (item.fallback) {
            item.fallback(item.uid, item.ticket, item.fid, item.blockIndex,
                          item.seq);
        }
        return;
    }
    openAndDeliver(item, blockCrc);
}

}  // namespace logic
}  // namespace cm

#endif  // CM_DOWNLOAD_ZEROCOPY
