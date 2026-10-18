-- Phase 5 schema patch v2_2
-- 目标：MySQL 5.7 / InnoDB / utf8mb4
-- 约束：不建库、不 USE；执行前先选择目标库。
-- 幂等：每段先查 information_schema，已满足则执行 SELECT 1，不重复 DDL。
-- 注意：若 idx_deleted 已存在但列不对，本脚本会因重名失败；此时需人工 DROP 后重跑。

SET NAMES utf8mb4;
SET @schema_name = DATABASE();

-- ---------------------------------------------------------------------------
-- 1. t_share.s_code：9 位数字 -> 6 位分享码（31 字符表）
-- ---------------------------------------------------------------------------
SET @need_share_code = (
    SELECT COUNT(*)
      FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = @schema_name
       AND TABLE_NAME = 't_share'
       AND COLUMN_NAME = 's_code'
       AND (CHARACTER_MAXIMUM_LENGTH <> 6
            OR CHARACTER_SET_NAME <> 'ascii'
            OR IFNULL(COLUMN_COMMENT, '') <> '6位分享码，31字符表')
);

-- 1.0 数据先行：旧库遗留 9 位纯数字分享码，直接收窄列宽会在严格模式下报
--     ERROR 1265 Data truncated。这里先把超长码重算为 6 位合法码。
--     字符表 = 23456789ABCDEFGHJKMNPQRSTUVWXYZ（去掉 0/O/1/I/L）。
--     做法：取 MD5('cm-share-'||s_id) 前 6 位，把十六进制里的 0/1 映射成 Z/Y，
--     结果只含 [2-9A-F]，天然落在合法字符表内；按 s_id 派生，可重复执行且结果稳定。
UPDATE t_share
   SET s_code = UPPER(REPLACE(REPLACE(
           SUBSTRING(MD5(CONCAT('cm-share-', s_id)), 1, 6), '0', 'Z'), '1', 'Y'))
 WHERE CHAR_LENGTH(s_code) <> 6;

-- 1.1 结构收窄：CHAR(9) -> CHAR(6)，幂等
SET @sql = IF(@need_share_code > 0,
    'ALTER TABLE t_share MODIFY COLUMN s_code CHAR(6) CHARACTER SET ascii NOT NULL COMMENT ''6位分享码，31字符表''',
    'SELECT 1');
PREPARE cm_patch_stmt FROM @sql;
EXECUTE cm_patch_stmt;
DEALLOCATE PREPARE cm_patch_stmt;

-- ---------------------------------------------------------------------------
-- 2. t_user_file：回收站游标索引 idx_deleted (u_id, uf_deleted_at)
-- ---------------------------------------------------------------------------
SET @idx_column_count = (
    SELECT COUNT(*)
      FROM information_schema.STATISTICS
     WHERE TABLE_SCHEMA = @schema_name
       AND TABLE_NAME = 't_user_file'
       AND INDEX_NAME = 'idx_deleted'
);

SET @idx_order_ok = (
    SELECT COUNT(*)
      FROM information_schema.STATISTICS
     WHERE TABLE_SCHEMA = @schema_name
       AND TABLE_NAME = 't_user_file'
       AND INDEX_NAME = 'idx_deleted'
       AND ((SEQ_IN_INDEX = 1 AND COLUMN_NAME = 'u_id')
            OR (SEQ_IN_INDEX = 2 AND COLUMN_NAME = 'uf_deleted_at'))
);

SET @need_idx_deleted = IF(@idx_column_count <> 2 OR @idx_order_ok <> 2, 1, 0);

SET @sql = IF(@need_idx_deleted = 1,
    'ALTER TABLE t_user_file ADD KEY idx_deleted (u_id, uf_deleted_at)',
    'SELECT 1');
PREPARE cm_patch_stmt FROM @sql;
EXECUTE cm_patch_stmt;
DEALLOCATE PREPARE cm_patch_stmt;

-- ---------------------------------------------------------------------------
-- 3. t_transfer_task.tr_status：注释补充 7=合并中
-- ---------------------------------------------------------------------------
SET @need_status_comment = (
    SELECT COUNT(*)
      FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = @schema_name
       AND TABLE_NAME = 't_transfer_task'
       AND COLUMN_NAME = 'tr_status'
       AND IFNULL(COLUMN_COMMENT, '') NOT LIKE '%7合并中%'
);

SET @sql = IF(@need_status_comment > 0,
    'ALTER TABLE t_transfer_task MODIFY COLUMN tr_status TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''0待开始 1进行中 2暂停 3失败 4完成 5取消 6过期 7合并中''',
    'SELECT 1');
PREPARE cm_patch_stmt FROM @sql;
EXECUTE cm_patch_stmt;
DEALLOCATE PREPARE cm_patch_stmt;

-- ---------------------------------------------------------------------------
-- 4. t_transfer_task.tr_offset：语义改为已完成分片字节总和
-- ---------------------------------------------------------------------------
SET @need_offset_comment = (
    SELECT COUNT(*)
      FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = @schema_name
       AND TABLE_NAME = 't_transfer_task'
       AND COLUMN_NAME = 'tr_offset'
       AND IFNULL(COLUMN_COMMENT, '') NOT LIKE '%已完成分片字节总和%'
);

SET @sql = IF(@need_offset_comment > 0,
    'ALTER TABLE t_transfer_task MODIFY COLUMN tr_offset BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''已完成分片字节总和（派生值，可重算）''',
    'SELECT 1');
PREPARE cm_patch_stmt FROM @sql;
EXECUTE cm_patch_stmt;
DEALLOCATE PREPARE cm_patch_stmt;
