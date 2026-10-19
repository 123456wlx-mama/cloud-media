#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cm/base/error_code.h"

namespace cm {
namespace logic {

// 下载块 CRC sidecar（<blobPath>.crc）：
// magic "CMBC1"(5) + version u8(1) + block_size u32 BE + block_count u32 BE
// + crc[block_count] u32 BE；总长 14 + 4n。block_size 固定 4194304。
constexpr std::uint32_t kBlockCrcBlockSize = 4194304;
constexpr std::size_t kBlockCrcHeaderSize = 14;

// 读 blob 边算每块 CRC32，写 <blobPath>.crc.tmp → fsync → rename。
// block_count 由 size 推导，不信任外部传入。
cm::ErrorCode writeBlockCrc(const std::string& blobPath, std::uint64_t size);

// 读取并校验 sidecar。缺失/损坏/长度不符统一返回 Ok + missing=true
// （crcs 已清空，由调用方重建）；真正不可能恢复的错误才返回非 Ok。
cm::ErrorCode readBlockCrc(const std::string& blobPath, std::uint64_t size,
                           std::vector<std::uint32_t>* crcs, bool* missing);

// 取某块 CRC：槽位有效且非 0 直接返回；否则读该块现算并 pwrite 到
// 14 + 4*index（sidecar 缺失/损坏时先建完整长度的空壳）。
// 写盘失败只影响下次命中，仍返回计算出的 CRC，不阻断下载。
// 调用方必须在 IO 池执行（本函数会阻塞读盘）。
cm::ErrorCode ensureBlockCrc(const std::string& blobPath, std::uint64_t size,
                             std::uint32_t blockIndex, std::uint32_t* crc);

}  // namespace logic
}  // namespace cm
