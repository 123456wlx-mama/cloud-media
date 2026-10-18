#!/bin/bash
# Phase 4 集成测试库初始化（独立于 cloud_media 迁移库，避免测试数据污染）
# 用法：sudo bash scripts/sql/setup_test_db.sh
set -e

SQL_DIR=$(cd "$(dirname "$0")/../../sql" && pwd)

mysql --defaults-file=/etc/mysql/debian.cnf -e "
CREATE DATABASE IF NOT EXISTS cloud_media_test DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;
GRANT ALL PRIVILEGES ON cloud_media_test.* TO 'netdisk'@'localhost';
GRANT ALL PRIVILEGES ON cloud_media_test.* TO 'netdisk'@'127.0.0.1';
FLUSH PRIVILEGES;"

mysql -unetdisk -pnetdisk123 cloud_media_test < "${SQL_DIR}/schema_v2.sql" 2>/dev/null

echo "test_db_ready"
