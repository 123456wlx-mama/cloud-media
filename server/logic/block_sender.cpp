#include "logic/block_sender.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <utility>

#include "cm/base/byte_order.h"
#include "cm/base/crc32.h"
#include "cm/protocol/frame.h"
#include "cm/protocol/message_type.h"
#include "db/file_dao.h"
#include "db/mysql_pool.h"

namespace cm {
namespace logic {
namespace {

const std::uint32_t kChunkSize = 4194304;
const std::size_t kMaxInFlight = 16;

}  // namespace

BlockSender::BlockSender(cm::db::MysqlPool& mysql,
                         cm::cache::RedisPool& redis,
                         Executor executor,
                         const std::string& dataDir)
    : m_mysql(mysql),
      m_ticket(redis),
      m_executor(std::move(executor)),
      m_dataDir(dataDir),
      m_inFlight(0) {
}

void BlockSender::sendBlock(std::uint64_t uid,
                            const std::string& ticket,
                            std::uint64_t fid,
                            std::uint32_t blockIndex,
                            std::uint32_t seq,
                            DoneCallback done) {
    if (!done) {
        return;
    }

    // 1. 票据校验 + 滑动续期（Redis，业务线程）。uid/fid 不符 → 1004。
    DownloadTicketInfo info;
    ErrorCode rc = m_ticket.touch(ticket, uid, fid, &info);
    if (rc != ErrorCode::Ok) {
        done(rc, std::string());
        return;
    }

    // 2. 查 blob（MySQL，业务线程）。
    std::unique_ptr<cm::db::MysqlConn> conn = m_mysql.acquire();
    if (!conn) {
        rc = (m_mysql.lastError() == ErrorCode::Ok) ? ErrorCode::DbPoolTimeout
                                                    : m_mysql.lastError();
        done(rc, std::string());
        return;
    }
    cm::db::FileDao dao(*conn);
    cm::db::BlobRow blob;
    rc = dao.getBlobById(fid, &blob);
    if (rc == ErrorCode::NotFound) {
        done(ErrorCode::FileGone, std::string());
        return;
    }
    if (rc != ErrorCode::Ok) {
        done(rc, std::string());
        return;
    }
    if (blob.state != 1 || blob.size == 0) {
        done(ErrorCode::FileGone, std::string());
        return;
    }

    // 3. 块范围校验：offset = index*4MB，越界回 1001。
    const std::uint64_t blockCount =
        (blob.size + kChunkSize - 1) / kChunkSize;
    if (static_cast<std::uint64_t>(blockIndex) >= blockCount) {
        done(ErrorCode::InvalidArgument, std::string());
        return;
    }

    WorkItem item;
    item.path = blob.path;
    item.size = blob.size;
    item.blockIndex = blockIndex;
    item.seq = seq;
    item.done = std::move(done);
    enqueue(std::move(item));
}

void BlockSender::enqueue(WorkItem item) {
    bool dispatch = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight < kMaxInFlight) {
            ++m_inFlight;
            dispatch = true;
        } else {
            m_queue.push_back(std::move(item));
        }
    }
    if (dispatch) {
        dispatchToPool(std::move(item));
    }
}

void BlockSender::dispatchToPool(WorkItem item) {
    if (m_executor([this, item]() { runBlock(item); })) {
        return;
    }
    // IO 池已停止：失败当前项并归还名额；剩余排队项随 BlockSender 析构释放。
    item.done(ErrorCode::Timeout, std::string());
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_inFlight > 0) {
        --m_inFlight;
    }
}

void BlockSender::runBlock(const WorkItem& item) {
    std::string frame;
    const ErrorCode rc = readBlock(item, &frame);
    item.done(rc, std::move(frame));
    onBlockDone();
}

void BlockSender::onBlockDone() {
    WorkItem next;
    bool hasNext = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight > 0) {
            --m_inFlight;
        }
        if (!m_queue.empty()) {
            next = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_inFlight;
            hasNext = true;
        }
    }
    if (hasNext) {
        dispatchToPool(std::move(next));
    }
}

cm::ErrorCode BlockSender::readBlock(const WorkItem& item,
                                     std::string* frame) const {
    const std::string blobPath = m_dataDir + "/blobs/" + item.path;
    const int fd = ::open(blobPath.c_str(), O_RDONLY);
    if (fd < 0) {
        return ErrorCode::FileGone;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return ErrorCode::FileGone;
    }
    if (static_cast<std::uint64_t>(st.st_size) != item.size) {
        ::close(fd);
        return ErrorCode::FileGone;
    }

    const std::uint64_t offset =
        static_cast<std::uint64_t>(item.blockIndex) * kChunkSize;
    const std::uint64_t len =
        std::min<std::uint64_t>(kChunkSize, item.size - offset);

    // 单缓冲：帧 = 19B 头 + payload，块直接读进帧的 payload 区（峰值 4MB/块）。
    frame->assign(static_cast<std::size_t>(cm::protocol::kHeaderSize + len),
                  '\0');
    std::uint8_t* header =
        reinterpret_cast<std::uint8_t*>(&(*frame)[0]);

    cm::writeU32BE(header, cm::protocol::kMagic);
    header[4] = static_cast<std::uint8_t>(cm::protocol::kVersion);
    cm::writeU16BE(
        header + 5,
        static_cast<std::uint16_t>(cm::protocol::MessageType::DownloadDataRs));
    cm::writeU32BE(header + 7, static_cast<std::uint32_t>(len));
    cm::writeU32BE(header + 11, item.seq);
    // CRC 字段（offset 15~18）暂不填，读完 payload 后写入。

    char* payload = &(*frame)[cm::protocol::kHeaderSize];

    // 边读边算 CRC：只覆盖 version+type+len+seq 共 11 字节 + payload，
    // 与 Codec::encode 的规则一致，绝不把 CRC 字段本身算进去。
    cm::Crc32 checksum;
    checksum.update(header + 4, 11);

    std::size_t total = 0;
    while (total < static_cast<std::size_t>(len)) {
        const ssize_t n =
            ::pread(fd, payload + total, static_cast<std::size_t>(len) - total,
                    static_cast<off_t>(offset + total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return ErrorCode::FileGone;
        }
        if (n == 0) {
            break;  // 提前 EOF：短读
        }
        checksum.update(payload + total, static_cast<std::size_t>(n));
        total += static_cast<std::size_t>(n);
    }
    ::close(fd);

    if (total != static_cast<std::size_t>(len)) {
        return ErrorCode::FileGone;
    }

    cm::writeU32BE(header + 15, checksum.value());
    return ErrorCode::Ok;
}

std::size_t BlockSender::inFlight() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight;
}

std::size_t BlockSender::pending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

}  // namespace logic
}  // namespace cm
