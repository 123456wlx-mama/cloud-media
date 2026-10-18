#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#ifndef CM_SOURCE_DIR
#error CM_SOURCE_DIR must be defined for schema contract tests
#endif

namespace {

std::string readSchema() {
    const std::string path =
        std::string(CM_SOURCE_DIR) + "/sql/schema_v2.sql";
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::size_t countLinesStartingWith(const std::string& text,
                                   const std::string& prefix) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::size_t length =
            end == std::string::npos ? text.size() - pos : end - pos;
        const std::string line = text.substr(pos, length);
        if (line.compare(0, prefix.size(), prefix) == 0) {
            ++count;
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return count;
}

std::string tableBlock(const std::string& text, const std::string& table) {
    const std::string marker = "CREATE TABLE IF NOT EXISTS " + table;
    const std::size_t start = text.find(marker);
    if (start == std::string::npos) {
        return std::string();
    }
    const std::size_t end = text.find(';', start);
    if (end == std::string::npos) {
        return std::string();
    }
    return text.substr(start, end - start + 1);
}

}  // namespace

TEST(SchemaContract, HasSevenTables) {
    const std::string schema = readSchema();
    ASSERT_FALSE(schema.empty()) << "cannot read sql/schema_v2.sql";
    EXPECT_EQ(std::size_t(7),
              countLinesStartingWith(schema, "CREATE TABLE IF NOT EXISTS"));
}

TEST(SchemaContract, UserFileHasRequiredKeys) {
    const std::string schema = readSchema();
    ASSERT_FALSE(schema.empty());
    const std::string block = tableBlock(schema, "t_user_file");
    ASSERT_FALSE(block.empty());
    EXPECT_NE(std::string::npos, block.find("uk_user_parent_name"));
    EXPECT_NE(std::string::npos, block.find("idx_recent_created"));
}

TEST(SchemaContract, HasNoForeignKeys) {
    const std::string schema = readSchema();
    ASSERT_FALSE(schema.empty());
    EXPECT_EQ(std::string::npos, schema.find("FOREIGN KEY"));
    EXPECT_EQ(std::string::npos, schema.find("REFERENCES"));
}

TEST(SchemaContract, HasNoTriggersOrViews) {
    const std::string schema = readSchema();
    ASSERT_FALSE(schema.empty());
    EXPECT_EQ(std::string::npos, schema.find("CREATE TRIGGER"));
    EXPECT_EQ(std::string::npos, schema.find("CREATE VIEW"));
}

TEST(SchemaContract, UsesDynamicRowFormat) {
    const std::string schema = readSchema();
    ASSERT_FALSE(schema.empty());
    EXPECT_EQ(std::size_t(7),
              countLinesStartingWith(schema,
                                     ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC"));
    EXPECT_NE(std::string::npos, schema.find("DEFAULT CHARSET=utf8mb4"));
}
