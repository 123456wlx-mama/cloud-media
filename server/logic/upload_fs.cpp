#include "logic/upload_fs.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>

namespace cm {
namespace logic {

bool makeDirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    current.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/' || current.size() == 1) {
            continue;
        }
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

void removeTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != NULL) {
        struct dirent* entry = NULL;
        while ((entry = ::readdir(dir)) != NULL) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            removeTree(path + "/" + name);
        }
        ::closedir(dir);
    }
    ::remove(path.c_str());
}

}  // namespace logic
}  // namespace cm
