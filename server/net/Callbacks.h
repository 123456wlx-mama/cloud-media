#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "cm/base/buffer.h"

namespace net {

class TcpConnection;

// 统一使用单调时钟：系统时间被 NTP 校正时，超时/心跳不会跳变
using Timestamp = std::chrono::steady_clock::time_point;

using ConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionWeakPtr = std::weak_ptr<TcpConnection>;

using Functor = std::function<void()>;
using TimerCallback = std::function<void()>;
using MessageCallback = std::function<void(const ConnectionPtr&, cm::Buffer*, Timestamp)>;
using CloseCallback = std::function<void(const ConnectionPtr&)>;

}  // namespace net
