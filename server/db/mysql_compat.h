#pragma once

// MySQL 客户端 5.7 / 8.0 双兼容层。
//
// MySQL 8.0.1 起客户端头文件移除了 my_bool（5.7 里是 char），
// 同时 MYSQL_BIND.is_null / .error 的指针类型由 my_bool* 改为 bool*。
// 本项目 VM 用 5.7 客户端、CI(ubuntu-22.04) 用 8.0 客户端，两端都要能编译。
//
// 处理方式：
// - 8.0 下恢复 1 字节 my_bool 别名，仅用于标量与存储（如 std::vector<my_bool>）；
//   禁止退化成 std::vector<bool>——位压缩特化无法取元素地址；
// - mysql_options / mysql_stmt_attr_set 的值参数本身是 const void*，两端都不用改；
// - MYSQL_BIND.is_null / .error 通过 cmBoolPtr() 按当前客户端期望的类型转换。

#include <mysql.h>

#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80001
typedef char my_bool;  // 8.0 已移除：恢复 1 字节别名，语义与 5.7 一致
#endif

namespace cm {
namespace db {

#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80001
// 8.0：MYSQL_BIND.is_null/.error 期望 bool*
inline bool* cmBoolPtr(my_bool* p) { return reinterpret_cast<bool*>(p); }
inline const bool* cmBoolPtr(const my_bool* p) {
    return reinterpret_cast<const bool*>(p);
}
#else
// 5.7：my_bool* 即期望类型
inline my_bool* cmBoolPtr(my_bool* p) { return p; }
inline const my_bool* cmBoolPtr(const my_bool* p) { return p; }
#endif

}  // namespace db
}  // namespace cm
