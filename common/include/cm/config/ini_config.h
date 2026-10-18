#pragma once

#include <map>
#include <string>

#include "cm/base/error_code.h"

namespace cm {

class IniConfig {
public:
    ErrorCode load(const std::string& path, std::string* err);

    bool has(const std::string& section, const std::string& key) const;
    std::string getString(const std::string& section, const std::string& key,
                          const std::string& defaultValue) const;
    ErrorCode getInt(const std::string& section, const std::string& key,
                     int* value) const;
    ErrorCode getBool(const std::string& section, const std::string& key,
                      bool* value) const;

private:
    typedef std::map<std::string, std::string> KeyMap;
    typedef std::map<std::string, KeyMap> SectionMap;

    SectionMap data_;
};

}  // namespace cm
