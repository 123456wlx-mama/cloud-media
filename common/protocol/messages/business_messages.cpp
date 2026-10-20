#include "cm/protocol/messages/download.h"

#include <cstddef>

#include "cm/protocol/messages/auth.h"
#include "cm/protocol/messages/list.h"
#include "cm/protocol/messages/play.h"
#include "cm/protocol/messages/recycle.h"
#include "cm/protocol/messages/share.h"
#include "cm/protocol/messages/upload.h"
#include "cm/protocol/serialize.h"

namespace cm {
namespace protocol {
namespace {

const std::size_t kMaxNameBytes = 255;
const std::size_t kMaxTicketBytes = 128;
const std::size_t kMaxAccessCodeBytes = 4;
// 认证字段上限与 t_user 列宽一致（u_tel 20 / u_name 45）。
const std::size_t kMaxTelBytes = 20;
const std::size_t kMaxAuthPasswordBytes = 64;
const std::size_t kMaxUserNameBytes = 45;

bool isHexN(const std::string& value, std::size_t len) {
    if (value.size() != len) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        const bool upper = (c >= 'A' && c <= 'F');
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

bool isValidMd5(const std::string& value) {
    return isHexN(value, 32);
}

bool isValidShareCode(const std::string& value) {
    if (value.size() != 6) {
        return false;
    }
    const char* alphabet = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
    for (std::size_t i = 0; i < value.size(); ++i) {
        bool found = false;
        for (std::size_t j = 0; alphabet[j] != '\0'; ++j) {
            if (alphabet[j] == value[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// 提取码：空或 4 位数字（06 §4.11）。
bool isValidAccessCode(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    if (value.size() != kMaxAccessCodeBytes) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

bool isValidTicket(const std::string& value) {
    return value.size() <= kMaxTicketBytes && isHexN(value, value.size());
}

bool isNameOk(const std::string& value, bool allowEmpty) {
    if (value.empty()) {
        return allowEmpty;
    }
    return value.size() <= kMaxNameBytes;
}

bool isTelOk(const std::string& value) {
    return !value.empty() && value.size() <= kMaxTelBytes;
}

bool isAuthPasswordOk(const std::string& value) {
    return !value.empty() && value.size() <= kMaxAuthPasswordBytes;
}

bool isUserNameOk(const std::string& value, bool allowEmpty) {
    if (value.empty()) {
        return allowEmpty;
    }
    return value.size() <= kMaxUserNameBytes;
}

bool writeU8Max(Writer* writer, std::uint8_t value, std::uint8_t maxValue) {
    return value <= maxValue && writer->writeU8(value);
}

bool readU8Max(Reader* reader, std::uint8_t* value, std::uint8_t maxValue) {
    return reader->readU8(value) && *value <= maxValue;
}

bool writeLimit(Writer* writer, std::uint32_t limit) {
    return limit >= 1 && limit <= 200 && writer->writeU32(limit);
}

bool readLimit(Reader* reader, std::uint32_t* limit) {
    return reader->readU32(limit) && *limit >= 1 && *limit <= 200;
}

std::size_t bitmapBytes(std::uint32_t totalChunks) {
    return (static_cast<std::size_t>(totalChunks) + 7) / 8;
}

bool writeBytesField(Writer* writer, const std::string& value) {
    return writer->writeU32(static_cast<std::uint32_t>(value.size())) &&
           writer->writeBytes(value.data(), value.size());
}

bool readBytesField(Reader* reader, std::string* value) {
    std::uint32_t len = 0;
    if (!reader->readU32(&len)) {
        return false;
    }
    return reader->readBytes(value, len);
}

}  // namespace

// ---------------------------------------------------------------- upload ----

UploadInitRq::UploadInitRq()
    : parentId(0), totalSize(0), chunkSize(0), totalChunks(0), flags(0), reqId(0) {}

bool UploadInitRq::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(fileName, false) || totalSize == 0 ||
        !isValidMd5(fileMd5) || totalChunks > kMaxChunkCount) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(parentId) || !writer.writeString(fileName) ||
        !writer.writeU64(totalSize) || !writer.writeString(fileMd5) ||
        !writer.writeU32(chunkSize) || !writer.writeU32(totalChunks) ||
        !writer.writeU8(flags) || !writer.writeU32(reqId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool UploadInitRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&parentId) && reader.readString(&fileName) &&
           isNameOk(fileName, false) && reader.readU64(&totalSize) &&
           totalSize > 0 && reader.readString(&fileMd5) && isValidMd5(fileMd5) &&
           reader.readU32(&chunkSize) && reader.readU32(&totalChunks) &&
           totalChunks <= kMaxChunkCount && reader.readU8(&flags) &&
           reader.readU32(&reqId);
}

UploadInitRs::UploadInitRs()
    : code(0), result(0), fileId(0), ufId(0), chunkSize(0), totalChunks(0),
      expireAtMs(0) {}

bool UploadInitRs::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(fileName, true) || totalChunks > kMaxChunkCount) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, result, 1) ||
        !writer.writeString(taskId) || !writer.writeU64(fileId) ||
        !writer.writeU64(ufId) || !writer.writeString(fileName) ||
        !writer.writeU32(chunkSize) || !writer.writeU32(totalChunks) ||
        !writer.writeU64(expireAtMs)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool UploadInitRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &result, 1) &&
           reader.readString(&taskId) && reader.readU64(&fileId) &&
           reader.readU64(&ufId) && reader.readString(&fileName) &&
           isNameOk(fileName, true) && reader.readU32(&chunkSize) &&
           reader.readU32(&totalChunks) && totalChunks <= kMaxChunkCount &&
           reader.readU64(&expireAtMs);
}

UploadChunkRq::UploadChunkRq() : chunkIndex(0), offset(0) {}

bool UploadChunkRq::encode(std::string* out) const {
    if (out == nullptr || !isValidMd5(chunkMd5) || data.empty()) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(taskId) || !writer.writeU32(chunkIndex) ||
        !writer.writeU64(offset) || !writer.writeString(chunkMd5) ||
        !writeBytesField(&writer, data)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool UploadChunkRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&taskId) && reader.readU32(&chunkIndex) &&
           reader.readU64(&offset) && reader.readString(&chunkMd5) &&
           isValidMd5(chunkMd5) && readBytesField(&reader, &data) &&
           !data.empty();
}

UploadChunkRs::UploadChunkRs()
    : code(0), chunkIndex(0), duplicate(0), doneChunks(0), doneBytes(0),
      nextHint(0) {}

bool UploadChunkRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU32(chunkIndex) ||
        !writeU8Max(&writer, duplicate, 1) || !writer.writeU32(doneChunks) ||
        !writer.writeU64(doneBytes) || !writer.writeU32(nextHint)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool UploadChunkRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU32(&chunkIndex) &&
           readU8Max(&reader, &duplicate, 1) && reader.readU32(&doneChunks) &&
           reader.readU64(&doneBytes) && reader.readU32(&nextHint);
}

QuickUploadRq::QuickUploadRq() : totalSize(0) {}

bool QuickUploadRq::encode(std::string* out) const {
    if (out == nullptr || !isValidMd5(fileMd5) || totalSize == 0) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(fileMd5) || !writer.writeU64(totalSize)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool QuickUploadRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&fileMd5) && isValidMd5(fileMd5) &&
           reader.readU64(&totalSize) && totalSize > 0;
}

QuickUploadRs::QuickUploadRs() : code(0), hit(0), fileId(0), size(0) {}

bool QuickUploadRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, hit, 1) ||
        !writer.writeU64(fileId) || !writer.writeU64(size)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool QuickUploadRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &hit, 1) &&
           reader.readU64(&fileId) && reader.readU64(&size);
}

ContinueUploadRq::ContinueUploadRq() {}

bool ContinueUploadRq::encode(std::string* out) const {
    if (out == nullptr || taskId.empty()) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(taskId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ContinueUploadRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&taskId) && !taskId.empty();
}

ContinueUploadRs::ContinueUploadRs()
    : code(0), status(0), chunkSize(0), totalChunks(0), doneChunks(0),
      doneBytes(0), expireAtMs(0) {}

bool ContinueUploadRs::encode(std::string* out) const {
    if (out == nullptr || status > 7 || totalChunks == 0 ||
        totalChunks > kMaxChunkCount || doneChunks > totalChunks ||
        bitmap.size() != bitmapBytes(totalChunks)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeString(taskId) ||
        !writer.writeU8(status) || !writer.writeU32(chunkSize) ||
        !writer.writeU32(totalChunks) || !writer.writeU32(doneChunks) ||
        !writer.writeU64(doneBytes) || !writeBytesField(&writer, bitmap) ||
        !writer.writeU64(expireAtMs)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ContinueUploadRs::decode(const std::string& payload) {
    Reader reader(payload);
    if (!reader.readU32(&code) || !reader.readString(&taskId) ||
        !readU8Max(&reader, &status, 7) || !reader.readU32(&chunkSize) ||
        !reader.readU32(&totalChunks) || totalChunks == 0 ||
        totalChunks > kMaxChunkCount || !reader.readU32(&doneChunks) ||
        doneChunks > totalChunks || !reader.readU64(&doneBytes) ||
        !readBytesField(&reader, &bitmap)) {
        return false;
    }
    if (bitmap.size() != bitmapBytes(totalChunks)) {
        return false;
    }
    return reader.readU64(&expireAtMs);
}

UploadCompleteRq::UploadCompleteRq() {}

bool UploadCompleteRq::encode(std::string* out) const {
    if (out == nullptr || taskId.empty()) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(taskId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool UploadCompleteRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&taskId) && !taskId.empty();
}

UploadCompleteRs::UploadCompleteRs() : code(0), fileId(0), ufId(0), size(0) {}

bool UploadCompleteRs::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(fileName, true)) {
        return false;
    }
    // 超过 1024 截断；调用方据 badIndexes.size() > kMaxBadIndexes 自行置 code=1001。
    const std::size_t count =
        badIndexes.size() > kMaxBadIndexes ? kMaxBadIndexes : badIndexes.size();

    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(fileId) ||
        !writer.writeU64(ufId) || !writer.writeString(fileName) ||
        !writer.writeString(fileMd5) || !writer.writeU64(size) ||
        !writer.writeU32(static_cast<std::uint32_t>(count))) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!writer.writeU32(badIndexes[i])) {
            return false;
        }
    }
    *out = writer.data();
    return true;
}

bool UploadCompleteRs::decode(const std::string& payload) {
    Reader reader(payload);
    std::uint32_t count = 0;
    if (!reader.readU32(&code) || !reader.readU64(&fileId) ||
        !reader.readU64(&ufId) || !reader.readString(&fileName) ||
        !isNameOk(fileName, true) || !reader.readString(&fileMd5) ||
        !reader.readU64(&size) || !reader.readU32(&count) ||
        count > kMaxBadIndexes) {
        return false;
    }
    badIndexes.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t index = 0;
        if (!reader.readU32(&index)) {
            return false;
        }
        badIndexes.push_back(index);
    }
    return true;
}

// -------------------------------------------------------------- download ----

DownloadOpenRq::DownloadOpenRq() : ufId(0), startBlock(0) {}

bool DownloadOpenRq::encode(std::string* out) const {
    if (out == nullptr || !isValidTicket(shareTicket)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId) || !writer.writeString(shareTicket) ||
        !writer.writeU32(startBlock)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadOpenRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId) && reader.readString(&shareTicket) &&
           isValidTicket(shareTicket) && reader.readU32(&startBlock);
}

DownloadOpenRs::DownloadOpenRs()
    : code(0), fId(0), size(0), blockSize(0), blockCount(0),
      ticketExpireMs(0), startBlock(0), zeroCopy(0), hls(0) {}

bool DownloadOpenRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    // 错误包的 file_md5/ticket 允许为空，否则业务失败时无法编码 10012。
    if (!fileMd5.empty() && !isValidMd5(fileMd5)) {
        return false;
    }
    if (!ticket.empty() && !isValidTicket(ticket)) {
        return false;
    }
    if (code == 0 && (!isValidMd5(fileMd5) || !isValidTicket(ticket))) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(fId) ||
        !writer.writeU64(size) || !writer.writeString(fileMd5) ||
        !writer.writeU32(blockSize) || !writer.writeU32(blockCount) ||
        !writer.writeString(ticket) || !writer.writeU64(ticketExpireMs) ||
        !writer.writeU32(startBlock) || !writeU8Max(&writer, zeroCopy, 1) ||
        !writeU8Max(&writer, hls, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadOpenRs::decode(const std::string& payload) {
    Reader reader(payload);
    if (!reader.readU32(&code) || !reader.readU64(&fId) ||
        !reader.readU64(&size) || !reader.readString(&fileMd5) ||
        !reader.readU32(&blockSize) || !reader.readU32(&blockCount) ||
        !reader.readString(&ticket) || !reader.readU64(&ticketExpireMs) ||
        !reader.readU32(&startBlock) ||
        !readU8Max(&reader, &zeroCopy, 1) ||
        !readU8Max(&reader, &hls, 1)) {
        return false;
    }
    if (!fileMd5.empty() && !isValidMd5(fileMd5)) {
        return false;
    }
    if (!ticket.empty() && !isValidTicket(ticket)) {
        return false;
    }
    if (code == 0 && (!isValidMd5(fileMd5) || !isValidTicket(ticket))) {
        return false;
    }
    return true;
}

DownloadStatRq::DownloadStatRq() : ufId(0) {}

bool DownloadStatRq::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadStatRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId);
}

DownloadStatRs::DownloadStatRs()
    : code(0), fId(0), size(0), blockSize(0), blockCount(0), state(0),
      updatedAtMs(0) {}

bool DownloadStatRs::encode(std::string* out) const {
    if (out == nullptr || !isValidMd5(fileMd5) || state > 7) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(fId) ||
        !writer.writeU64(size) || !writer.writeString(fileMd5) ||
        !writer.writeU32(blockSize) || !writer.writeU32(blockCount) ||
        !writer.writeU8(state) || !writer.writeU64(updatedAtMs)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadStatRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU64(&fId) &&
           reader.readU64(&size) && reader.readString(&fileMd5) &&
           isValidMd5(fileMd5) && reader.readU32(&blockSize) &&
           reader.readU32(&blockCount) && readU8Max(&reader, &state, 7) &&
           reader.readU64(&updatedAtMs);
}

DownloadRangeRq::DownloadRangeRq() : fId(0), blockIndex(0) {}

bool DownloadRangeRq::encode(std::string* out) const {
    if (out == nullptr || !isValidTicket(ticket)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(ticket) || !writer.writeU64(fId) ||
        !writer.writeU32(blockIndex)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadRangeRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&ticket) && isValidTicket(ticket) &&
           reader.readU64(&fId) && reader.readU32(&blockIndex);
}

DownloadDoneRq::DownloadDoneRq() : fId(0), size(0), verifyOk(0) {}

bool DownloadDoneRq::encode(std::string* out) const {
    if (out == nullptr || !isValidTicket(ticket) || !isValidMd5(fileMd5)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(ticket) || !writer.writeU64(fId) ||
        !writer.writeU64(size) || !writer.writeString(fileMd5) ||
        !writeU8Max(&writer, verifyOk, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadDoneRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&ticket) && isValidTicket(ticket) &&
           reader.readU64(&fId) && reader.readU64(&size) &&
           reader.readString(&fileMd5) && isValidMd5(fileMd5) &&
           readU8Max(&reader, &verifyOk, 1);
}

DownloadDoneRs::DownloadDoneRs() : code(0), accepted(0) {}

bool DownloadDoneRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, accepted, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DownloadDoneRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &accepted, 1);
}

// ----------------------------------------------------------------- share ----

ShareCreateRq::ShareCreateRq() : ufId(0), expirePolicy(0), accessMode(0), reqId(0) {}

bool ShareCreateRq::encode(std::string* out) const {
    if (out == nullptr || expirePolicy > 2 || accessMode > 1) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId) || !writer.writeU8(expirePolicy) ||
        !writer.writeU8(accessMode) || !writer.writeU32(reqId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareCreateRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId) && readU8Max(&reader, &expirePolicy, 2) &&
           readU8Max(&reader, &accessMode, 1) && reader.readU32(&reqId);
}

ShareCreateRs::ShareCreateRs() : code(0), accessCodeRequired(0), expireAtMs(0) {}

bool ShareCreateRs::encode(std::string* out) const {
    if (out == nullptr || (!shareCode.empty() && !isValidShareCode(shareCode))) {
        return false;
    }
    if (!isValidAccessCode(accessCode)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeString(shareCode) ||
        !writer.writeString(accessCode) ||
        !writeU8Max(&writer, accessCodeRequired, 1) ||
        !writer.writeU64(expireAtMs)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareCreateRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readString(&shareCode) &&
           (shareCode.empty() || isValidShareCode(shareCode)) &&
           reader.readString(&accessCode) && isValidAccessCode(accessCode) &&
           readU8Max(&reader, &accessCodeRequired, 1) &&
           reader.readU64(&expireAtMs);
}

ShareListRq::ShareListRq() : cursorCreatedMs(0), cursorId(0), limit(0) {}

bool ShareListRq::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(cursorCreatedMs) || !writer.writeU64(cursorId) ||
        !writeLimit(&writer, limit)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareListRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&cursorCreatedMs) && reader.readU64(&cursorId) &&
           readLimit(&reader, &limit);
}

ShareListItem::ShareListItem()
    : size(0), accessCodeRequired(0), status(0), expireAtMs(0), createdAtMs(0) {}

ShareListRs::ShareListRs() : code(0), hasMore(0), nextCreatedMs(0), nextId(0) {}

bool ShareListRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) ||
        !writer.writeU32(static_cast<std::uint32_t>(items.size()))) {
        return false;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const ShareListItem& item = items[i];
        if (!isValidShareCode(item.shareCode) ||
            !isNameOk(item.fileName, true) ||
            !writer.writeString(item.shareCode) ||
            !writer.writeString(item.fileName) ||
            !writer.writeU64(item.size) ||
            !writeU8Max(&writer, item.accessCodeRequired, 1) ||
            !writeU8Max(&writer, item.status, 1) ||
            !writer.writeU64(item.expireAtMs) ||
            !writer.writeU64(item.createdAtMs)) {
            return false;
        }
    }
    if (!writeU8Max(&writer, hasMore, 1) || !writer.writeU64(nextCreatedMs) ||
        !writer.writeU64(nextId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareListRs::decode(const std::string& payload) {
    Reader reader(payload);
    std::uint32_t count = 0;
    if (!reader.readU32(&code) || !reader.readU32(&count) || count > 1000) {
        return false;
    }
    items.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        ShareListItem item;
        if (!reader.readString(&item.shareCode) ||
            !isValidShareCode(item.shareCode) ||
            !reader.readString(&item.fileName) ||
            !isNameOk(item.fileName, true) || !reader.readU64(&item.size) ||
            !readU8Max(&reader, &item.accessCodeRequired, 1) ||
            !readU8Max(&reader, &item.status, 1) ||
            !reader.readU64(&item.expireAtMs) ||
            !reader.readU64(&item.createdAtMs)) {
            return false;
        }
        items.push_back(item);
    }
    return readU8Max(&reader, &hasMore, 1) && reader.readU64(&nextCreatedMs) &&
           reader.readU64(&nextId);
}

ShareOpenRq::ShareOpenRq() {}

bool ShareOpenRq::encode(std::string* out) const {
    if (out == nullptr || !isValidShareCode(shareCode) ||
        !isValidAccessCode(accessCode)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(shareCode) || !writer.writeString(accessCode)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareOpenRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&shareCode) && isValidShareCode(shareCode) &&
           reader.readString(&accessCode) && isValidAccessCode(accessCode);
}

ShareOpenRs::ShareOpenRs()
    : code(0), fId(0), size(0), isDir(0), expireAtMs(0), accessCodeRequired(0) {}

bool ShareOpenRs::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(fileName, true) ||
        (!fileMd5.empty() && !isValidMd5(fileMd5)) ||
        !isValidTicket(shareTicket)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(fId) ||
        !writer.writeString(fileName) || !writer.writeU64(size) ||
        !writer.writeString(fileMd5) || !writeU8Max(&writer, isDir, 1) ||
        !writer.writeU64(expireAtMs) || !writer.writeString(shareTicket) ||
        !writeU8Max(&writer, accessCodeRequired, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareOpenRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU64(&fId) &&
           reader.readString(&fileName) && isNameOk(fileName, true) &&
           reader.readU64(&size) && reader.readString(&fileMd5) &&
           (fileMd5.empty() || isValidMd5(fileMd5)) &&
           readU8Max(&reader, &isDir, 1) && reader.readU64(&expireAtMs) &&
           reader.readString(&shareTicket) && isValidTicket(shareTicket) &&
           readU8Max(&reader, &accessCodeRequired, 1);
}

ShareCancelRq::ShareCancelRq() {}

bool ShareCancelRq::encode(std::string* out) const {
    if (out == nullptr || !isValidShareCode(shareCode)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(shareCode)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareCancelRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&shareCode) && isValidShareCode(shareCode);
}

ShareCancelRs::ShareCancelRs() : code(0), cancelled(0), already(0) {}

bool ShareCancelRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, cancelled, 1) ||
        !writeU8Max(&writer, already, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ShareCancelRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &cancelled, 1) &&
           readU8Max(&reader, &already, 1);
}

// --------------------------------------------------------------- recycle ----

DeleteFileRq::DeleteFileRq() : ufId(0) {}

bool DeleteFileRq::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DeleteFileRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId);
}

DeleteFileRs::DeleteFileRs() : code(0), deleted(0), already(0) {}

bool DeleteFileRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, deleted, 1) ||
        !writeU8Max(&writer, already, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool DeleteFileRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &deleted, 1) &&
           readU8Max(&reader, &already, 1);
}

RecycleListRq::RecycleListRq() : limit(0), cursorDeletedMs(0), cursorId(0) {}

bool RecycleListRq::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writeLimit(&writer, limit) || !writer.writeU64(cursorDeletedMs) ||
        !writer.writeU64(cursorId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RecycleListRq::decode(const std::string& payload) {
    Reader reader(payload);
    return readLimit(&reader, &limit) && reader.readU64(&cursorDeletedMs) &&
           reader.readU64(&cursorId);
}

RecycleItem::RecycleItem()
    : ufId(0), isDir(0), size(0), deletedAtMs(0), parentId(0) {}

RecycleListRs::RecycleListRs()
    : code(0), hasMore(0), nextDeletedMs(0), nextId(0) {}

bool RecycleListRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) ||
        !writer.writeU32(static_cast<std::uint32_t>(items.size()))) {
        return false;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const RecycleItem& item = items[i];
        if (!isNameOk(item.name, false) || !writer.writeU64(item.ufId) ||
            !writer.writeString(item.name) ||
            !writeU8Max(&writer, item.isDir, 1) ||
            !writer.writeU64(item.size) ||
            !writer.writeU64(item.deletedAtMs) ||
            !writer.writeU64(item.parentId)) {
            return false;
        }
    }
    if (!writeU8Max(&writer, hasMore, 1) || !writer.writeU64(nextDeletedMs) ||
        !writer.writeU64(nextId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RecycleListRs::decode(const std::string& payload) {
    Reader reader(payload);
    std::uint32_t count = 0;
    if (!reader.readU32(&code) || !reader.readU32(&count) || count > 1000) {
        return false;
    }
    items.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        RecycleItem item;
        if (!reader.readU64(&item.ufId) || !reader.readString(&item.name) ||
            !isNameOk(item.name, false) ||
            !readU8Max(&reader, &item.isDir, 1) ||
            !reader.readU64(&item.size) ||
            !reader.readU64(&item.deletedAtMs) ||
            !reader.readU64(&item.parentId)) {
            return false;
        }
        items.push_back(item);
    }
    return readU8Max(&reader, &hasMore, 1) && reader.readU64(&nextDeletedMs) &&
           reader.readU64(&nextId);
}

RestoreRq::RestoreRq() : ufId(0), autoRename(0) {}

bool RestoreRq::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(newName, true)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId) || !writer.writeString(newName) ||
        !writeU8Max(&writer, autoRename, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RestoreRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId) && reader.readString(&newName) &&
           isNameOk(newName, true) && readU8Max(&reader, &autoRename, 1);
}

RestoreRs::RestoreRs() : code(0), ufId(0), parentId(0), renamed(0) {}

bool RestoreRs::encode(std::string* out) const {
    if (out == nullptr || !isNameOk(name, false)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(ufId) ||
        !writer.writeU64(parentId) || !writer.writeString(name) ||
        !writeU8Max(&writer, renamed, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RestoreRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU64(&ufId) &&
           reader.readU64(&parentId) && reader.readString(&name) &&
           isNameOk(name, false) && readU8Max(&reader, &renamed, 1);
}

PurgeRq::PurgeRq() : ufId(0), confirm(0) {}

bool PurgeRq::encode(std::string* out) const {
    if (out == nullptr || confirm != 1) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(ufId) || !writer.writeU8(confirm)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool PurgeRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&ufId) && readU8Max(&reader, &confirm, 1) &&
           confirm == 1;
}

PurgeRs::PurgeRs()
    : code(0), filesDeleted(0), dirsDeleted(0), blobsOrphaned(0) {}

bool PurgeRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU32(filesDeleted) ||
        !writer.writeU32(dirsDeleted) || !writer.writeU32(blobsOrphaned)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool PurgeRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU32(&filesDeleted) &&
           reader.readU32(&dirsDeleted) && reader.readU32(&blobsOrphaned);
}

// ------------------------------------------------------------------ list ----

FileItem::FileItem()
    : ufId(0), fId(0), isDir(0), size(0), createdAtMs(0), updatedAtMs(0) {}

ListFileRq::ListFileRq() : parentId(0), sort(0), limit(0), cursorId(0) {}

bool ListFileRq::encode(std::string* out) const {
    if (out == nullptr || sort > 2) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(parentId) || !writer.writeU8(sort) ||
        !writeLimit(&writer, limit) || !writer.writeString(cursorKey) ||
        !writer.writeU64(cursorId)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ListFileRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU64(&parentId) && readU8Max(&reader, &sort, 2) &&
           readLimit(&reader, &limit) && reader.readString(&cursorKey) &&
           reader.readU64(&cursorId);
}

ListFileRs::ListFileRs()
    : code(0), hasMore(0), nextId(0), totalCount(0) {}

bool ListFileRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) ||
        !writer.writeU32(static_cast<std::uint32_t>(items.size()))) {
        return false;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const FileItem& item = items[i];
        if (!isNameOk(item.name, false) ||
            (!item.md5.empty() && !isValidMd5(item.md5)) ||
            !writer.writeU64(item.ufId) || !writer.writeU64(item.fId) ||
            !writer.writeString(item.name) ||
            !writeU8Max(&writer, item.isDir, 1) ||
            !writer.writeU64(item.size) || !writer.writeString(item.md5) ||
            !writer.writeU64(item.createdAtMs) ||
            !writer.writeU64(item.updatedAtMs)) {
            return false;
        }
    }
    if (!writeU8Max(&writer, hasMore, 1) || !writer.writeString(nextKey) ||
        !writer.writeU64(nextId) || !writer.writeU64(totalCount)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool ListFileRs::decode(const std::string& payload) {
    Reader reader(payload);
    std::uint32_t count = 0;
    if (!reader.readU32(&code) || !reader.readU32(&count) || count > 1000) {
        return false;
    }
    items.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        FileItem item;
        if (!reader.readU64(&item.ufId) || !reader.readU64(&item.fId) ||
            !reader.readString(&item.name) || !isNameOk(item.name, false) ||
            !readU8Max(&reader, &item.isDir, 1) ||
            !reader.readU64(&item.size) || !reader.readString(&item.md5) ||
            (!item.md5.empty() && !isValidMd5(item.md5)) ||
            !reader.readU64(&item.createdAtMs) ||
            !reader.readU64(&item.updatedAtMs)) {
            return false;
        }
        items.push_back(item);
    }
    return readU8Max(&reader, &hasMore, 1) && reader.readString(&nextKey) &&
           reader.readU64(&nextId) && reader.readU64(&totalCount);
}

// ------------------------------------------------------------------ play ----

PlayReportRq::PlayReportRq()
    : fId(0), positionMs(0), durationMs(0), finished(0) {}

bool PlayReportRq::encode(std::string* out) const {
    if (out == nullptr || (durationMs > 0 && positionMs > durationMs)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU64(fId) || !writer.writeU64(positionMs) ||
        !writer.writeU64(durationMs) || !writeU8Max(&writer, finished, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool PlayReportRq::decode(const std::string& payload) {
    Reader reader(payload);
    if (!reader.readU64(&fId) || !reader.readU64(&positionMs) ||
        !reader.readU64(&durationMs) || !readU8Max(&reader, &finished, 1)) {
        return false;
    }
    return !(durationMs > 0) || positionMs <= durationMs;
}

PlayReportRs::PlayReportRs() : code(0), saved(0) {}

bool PlayReportRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writeU8Max(&writer, saved, 1)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool PlayReportRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && readU8Max(&reader, &saved, 1);
}

// ------------------------------------------------------------------ auth ----

RegisterRq::RegisterRq() {
}

bool RegisterRq::encode(std::string* out) const {
    if (out == nullptr || !isTelOk(tel) || !isAuthPasswordOk(password) ||
        !isUserNameOk(name, false)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(tel) || !writer.writeString(password) ||
        !writer.writeString(name)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RegisterRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&tel) && isTelOk(tel) &&
           reader.readString(&password) && isAuthPasswordOk(password) &&
           reader.readString(&name) && isUserNameOk(name, false);
}

RegisterRs::RegisterRs() : code(0), uid(0) {
}

bool RegisterRs::encode(std::string* out) const {
    if (out == nullptr) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(uid)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool RegisterRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU64(&uid);
}

LoginRq::LoginRq() {
}

bool LoginRq::encode(std::string* out) const {
    if (out == nullptr || !isTelOk(tel) || !isAuthPasswordOk(password)) {
        return false;
    }
    Writer writer;
    if (!writer.writeString(tel) || !writer.writeString(password)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool LoginRq::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readString(&tel) && isTelOk(tel) &&
           reader.readString(&password) && isAuthPasswordOk(password);
}

LoginRs::LoginRs() : code(0), uid(0) {
}

bool LoginRs::encode(std::string* out) const {
    if (out == nullptr || !isUserNameOk(name, true)) {
        return false;
    }
    Writer writer;
    if (!writer.writeU32(code) || !writer.writeU64(uid) ||
        !writer.writeString(name)) {
        return false;
    }
    *out = writer.data();
    return true;
}

bool LoginRs::decode(const std::string& payload) {
    Reader reader(payload);
    return reader.readU32(&code) && reader.readU64(&uid) &&
           reader.readString(&name) && isUserNameOk(name, true);
}

}  // namespace protocol
}  // namespace cm
