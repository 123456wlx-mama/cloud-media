-- 创建生产库（测试库由验收命令单独创建，避免测试数据进入生产库）
CREATE DATABASE IF NOT EXISTS cloud_media
  DEFAULT CHARACTER SET utf8mb4
  COLLATE utf8mb4_general_ci;
