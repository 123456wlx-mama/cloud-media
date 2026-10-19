#pragma once

#include <string>

namespace cm {
namespace logic {

// 只创建目录；已存在视为成功。空路径返回 false。
bool makeDirs(const std::string& path);

// 递归删除目录（含普通文件）；不存在时静默返回。
void removeTree(const std::string& path);

}  // namespace logic
}  // namespace cm
