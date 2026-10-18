#!/bin/bash
# Phase 4 数据库初始化：建库 + 授权（需 root/admin 权限执行）
# 用法：sudo bash scripts/sql/setup_dev_db.sh
set -e

SQL_DIR=$(cd "$(dirname "$0")/../../sql" && pwd)

mysql --defaults-file=/etc/mysql/debian.cnf < "${SQL_DIR}/create_database.sql"

mysql --defaults-file=/etc/mysql/debian.cnf -e "
GRANT ALL PRIVILEGES ON cloud_media.* TO 'netdisk'@'localhost';
GRANT ALL PRIVILEGES ON cloud_media.* TO 'netdisk'@'127.0.0.1';
FLUSH PRIVILEGES;"

echo "db_ready"
