#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "cm/base/buffer.h"
#include "cm/protocol/frame.h"

namespace cm {
namespace cli {

// 退出码：0 成功 / 2 参数或鉴权 / 3 网络 / 4 服务端拒绝 / 9 主动中断。
const int kExitOk = 0;
const int kExitUsage = 2;
const int kExitNetwork = 3;
const int kExitRejected = 4;
const int kExitInterrupted = 9;

// 建立 TCP 连接并登录（10002/10003），成功返回 fd（>=0）；失败返回 -1。
// password 只用于组包，不进入任何日志。
// exitCode 区分：鉴权失败 kExitUsage，网络/协议失败 kExitNetwork。
int connectAndLogin(const std::string& host,
                    int port,
                    const std::string& tel,
                    const std::string& password,
                    std::string* error,
                    int* exitCode);

// 阻塞发一帧；写满或失败。
bool sendFrame(int fd,
               std::uint16_t type,
               std::uint32_t seq,
               const std::string& payload,
               std::string* error);

// 阻塞收一帧（内部循环直到 Codec::decode 成功）。超时/对端关闭/坏帧返回
// false；需连续收多帧时复用同一个 Buffer。
bool recvFrame(int fd,
               cm::Buffer* buffer,
               cm::protocol::Frame* frame,
               std::string* error);

// ERROR_RS（10044）payload = u32 code + string message。
bool decodeErrorRs(const cm::protocol::Frame& frame,
                   std::uint32_t* code,
                   std::string* message);

// 密码只从参数或 CM_CLI_PASSWORD 读取。
std::string passwordFromEnv();

bool parseU64(const std::string& text, std::uint64_t* value);
bool parseU32(const std::string& text, std::uint32_t* value);
bool parsePositiveInt(const std::string& text, int* value);

std::string baseName(const std::string& path);

// 位图（LSB-first）与 hex 文本互转；状态文件里存 hex。
std::string bitmapToHex(const std::string& bitmap);
bool bitmapFromHex(const std::string& hex, std::string* bitmap);
void bitmapSet(std::string* bitmap, std::uint32_t index);
bool bitmapTest(const std::string& bitmap, std::uint32_t index);

// 先写 <path>.tmp + fsync，再 rename。
bool writeFileAtomic(const std::string& path,
                     const std::string& data,
                     std::string* error);
bool readFileToString(const std::string& path, std::string* data);

// 流式 MD5（1MB 缓冲）。
bool md5File(const std::string& path, std::string* hex, std::string* error);

// 按偏移写满 / 读满。
bool pwriteAll(int fd, const char* data, std::size_t len, off_t offset,
               std::string* error);
bool preadExact(int fd, char* data, std::size_t len, off_t offset,
                std::string* error);

}  // namespace cli
}  // namespace cm
