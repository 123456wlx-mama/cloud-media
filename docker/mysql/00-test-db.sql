-- 测试库与测试账号（供 build/test 服务使用；生产不用）
CREATE DATABASE IF NOT EXISTS cloud_media_test
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

CREATE USER IF NOT EXISTS 'cm_test'@'%'
  IDENTIFIED WITH mysql_native_password BY 'cm_test_pw';
GRANT ALL PRIVILEGES ON cloud_media_test.* TO 'cm_test'@'%';
FLUSH PRIVILEGES;
