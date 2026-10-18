#include "cm/config/ini_config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cm {
namespace {

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

void setLineError(std::string* err, int lineNo, const std::string& message) {
    if (err != NULL) {
        std::ostringstream os;
        os << "line " << lineNo << ": " << message;
        *err = os.str();
    }
}

bool parseInt(const std::string& text, int* value) {
    if (text.empty()) {
        return false;
    }
    char* end = NULL;
    errno = 0;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

}  // namespace

ErrorCode IniConfig::load(const std::string& path, std::string* err) {
    data_.clear();

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        if (err != NULL) {
            *err = "cannot open ini file: " + path;
        }
        return ErrorCode::NotFound;
    }

    std::string section;
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;

        if (lineNo == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }

        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            if (line.size() < 3 || line[line.size() - 1] != ']') {
                setLineError(err, lineNo, "invalid section header");
                return ErrorCode::InvalidArgument;
            }
            section = trim(line.substr(1, line.size() - 2));
            if (section.empty()) {
                setLineError(err, lineNo, "empty section name");
                return ErrorCode::InvalidArgument;
            }
            continue;
        }

        std::string::size_type equals = line.find('=');
        if (equals == std::string::npos) {
            setLineError(err, lineNo, "missing '='");
            return ErrorCode::InvalidArgument;
        }

        std::string key = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (key.empty()) {
            setLineError(err, lineNo, "empty key");
            return ErrorCode::InvalidArgument;
        }

        if (!value.empty() && (value[0] == '\'' || value[0] == '"')) {
            char quote = value[0];
            if (value.size() < 2 || value[value.size() - 1] != quote) {
                setLineError(err, lineNo, "unmatched quote");
                return ErrorCode::InvalidArgument;
            }
            value = value.substr(1, value.size() - 2);
        }

        data_[section][key] = value;
    }

    if (err != NULL) {
        err->clear();
    }
    return ErrorCode::Ok;
}

bool IniConfig::has(const std::string& section, const std::string& key) const {
    SectionMap::const_iterator sectionIt = data_.find(section);
    if (sectionIt == data_.end()) {
        return false;
    }
    return sectionIt->second.find(key) != sectionIt->second.end();
}

std::string IniConfig::getString(const std::string& section, const std::string& key,
                                 const std::string& defaultValue) const {
    SectionMap::const_iterator sectionIt = data_.find(section);
    if (sectionIt == data_.end()) {
        return defaultValue;
    }
    KeyMap::const_iterator keyIt = sectionIt->second.find(key);
    if (keyIt == sectionIt->second.end()) {
        return defaultValue;
    }
    return keyIt->second;
}

ErrorCode IniConfig::getInt(const std::string& section, const std::string& key,
                            int* value) const {
    if (value == NULL) {
        return ErrorCode::InvalidArgument;
    }
    if (!has(section, key)) {
        return ErrorCode::NotFound;
    }

    int parsed = 0;
    if (!parseInt(getString(section, key, ""), &parsed)) {
        return ErrorCode::InvalidArgument;
    }
    *value = parsed;
    return ErrorCode::Ok;
}

ErrorCode IniConfig::getBool(const std::string& section, const std::string& key,
                             bool* value) const {
    if (value == NULL) {
        return ErrorCode::InvalidArgument;
    }
    if (!has(section, key)) {
        return ErrorCode::NotFound;
    }

    std::string text = getString(section, key, "");
    for (std::size_t i = 0; i < text.size(); ++i) {
        text[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(text[i])));
    }

    if (text == "1" || text == "true" || text == "yes") {
        *value = true;
        return ErrorCode::Ok;
    }
    if (text == "0" || text == "false" || text == "no") {
        *value = false;
        return ErrorCode::Ok;
    }
    return ErrorCode::InvalidArgument;
}

}  // namespace cm
