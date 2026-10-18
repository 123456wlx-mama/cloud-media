-- Phase 4 开发环境数据库账号（仅本机开发用；生产请用最小权限账号）
-- 执行：sudo mysql --defaults-file=/etc/mysql/debian.cnf < setup_dev_user.sql

CREATE USER IF NOT EXISTS 'netdisk'@'localhost' IDENTIFIED BY 'netdisk123';
CREATE USER IF NOT EXISTS 'netdisk'@'127.0.0.1' IDENTIFIED BY 'netdisk123';

GRANT ALL PRIVILEGES ON NetDisk.* TO 'netdisk'@'localhost';
GRANT ALL PRIVILEGES ON NetDisk.* TO 'netdisk'@'127.0.0.1';

FLUSH PRIVILEGES;
