#pragma once

#include <memory>
#include <utility>

namespace cm {

// C++11 没有 std::make_unique（那是 C++14）。
// 项目规则要求「禁止裸 new 管理资源」，所有独占所有权都经由这里创建。
template <typename T, typename... Args>
std::unique_ptr<T> makeUnique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

}  // namespace cm
