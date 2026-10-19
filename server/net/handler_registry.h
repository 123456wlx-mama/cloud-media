#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "Callbacks.h"
#include "cm/base/error_code.h"
#include "cm/protocol/frame.h"

namespace cm {
class ThreadPool;
}  // namespace cm

namespace net {

// 类型 → 业务处理函数的注册表。
// 线程模型：
//  - registerHandler 只在启动阶段调用；
//  - dispatch 在连接所属 IO 线程调用，只做查表、准入与投递，不碰存储；
//  - handler 在业务线程池执行，回包通过 TcpConnection::send 自动投回 loop。
//
// workers 为 NULL 时 dispatch 在调用线程同步执行，仅用于单测；生产必须传业务池。
class HandlerRegistry {
 public:
  using Handler = std::function<void(const ConnectionPtr&,
                                     const cm::protocol::Frame&)>;
  // 每连接准入：在 IO 线程、投递前调用；返回 false 表示暂不处理。
  using Admission = std::function<bool(const ConnectionPtr&)>;
  // 处理结束（成功或失败）后在业务线程调用；与 Admission 成对出现。
  using Release = std::function<void(const ConnectionPtr&)>;
  // 未注册类型/准入失败的回包动作；由调用方决定编码细节。
  using ErrorResponder = std::function<void(const ConnectionPtr&,
                                            std::uint32_t seq,
                                            std::uint32_t wireCode,
                                            const std::string& message)>;

  HandlerRegistry(cm::ThreadPool* workers, ErrorResponder errorResponder);
  ~HandlerRegistry();

  HandlerRegistry(const HandlerRegistry&) = delete;
  HandlerRegistry& operator=(const HandlerRegistry&) = delete;

  // 重复注册、type==0、handler 为空都返回 InvalidArgument。
  cm::ErrorCode registerHandler(std::uint16_t type, Handler handler,
                                Admission admission = Admission(),
                                Release release = Release());

  // IO 线程入口：查表 → 准入 → 投业务池。
  // 未注册类型回 9003；准入失败/投递失败回 1006；成功返回 Ok。
  cm::ErrorCode dispatch(std::uint16_t type, const ConnectionPtr& conn,
                         const cm::protocol::Frame& frame);

  // 已在业务线程（例如 10006 CRC 校验完成后）直接执行，不再投递。
  cm::ErrorCode dispatchInline(std::uint16_t type, const ConnectionPtr& conn,
                               const cm::protocol::Frame& frame);

  bool hasHandler(std::uint16_t type) const;
  std::size_t size() const;

  // 9003：帧类型不在 v2 类型表，或类型合法但没有注册 handler。
  static std::uint32_t unregisteredTypeCode();
  // 1001：解码失败统一回业务码，不再落 9001/9003。
  static std::uint32_t decodeFailureCode();
  // BadCrc 仍回 9005（02 §2.2 硬规则）；其余解码失败回 1001。
  static std::uint32_t wireCodeForDecodeStatus(cm::protocol::DecodeStatus status);

 private:
  struct Entry {
    Handler handler;
    Admission admission;
    Release release;
  };

  bool find(std::uint16_t type, Entry* out) const;
  void respond(const ConnectionPtr& conn, std::uint32_t seq,
               std::uint32_t wireCode, const char* message) const;
  void runHandler(const Entry& entry, const ConnectionPtr& conn,
                  const cm::protocol::Frame& frame);

  mutable std::mutex mutex_;
  std::map<std::uint16_t, Entry> handlers_;
  cm::ThreadPool* workers_;
  ErrorResponder errorResponder_;
};

}  // namespace net
