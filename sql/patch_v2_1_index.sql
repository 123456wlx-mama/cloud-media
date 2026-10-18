-- v2 补丁 1：t_user_file 增加「按创建时间倒序」联合索引
-- 背景：idx_recent 的第三列是 uf_updated_at，业务上的「最近上传」按 uf_created_at 排序，
--       导致 Using filesort。补 idx_recent_created 后 key 顺序与 ORDER BY 完全一致。
-- 幂等：已存在同名索引则跳过，可重复执行。

SET @exists := (
  SELECT COUNT(*) FROM information_schema.statistics
   WHERE table_schema = DATABASE()
     AND table_name = 't_user_file'
     AND index_name = 'idx_recent_created'
);

SET @ddl := IF(@exists = 0,
  'ALTER TABLE t_user_file ADD KEY idx_recent_created (u_id, uf_deleted_at, uf_created_at)',
  'SELECT ''idx_recent_created already exists'' AS msg');

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
