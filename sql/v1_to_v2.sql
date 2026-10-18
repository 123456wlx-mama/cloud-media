-- cloud-media v1 -> v2 数据迁移
-- 旧库名：NetDisk；目标库：cloud_media（当前 USE 的库）
-- 前置：先执行 sql/schema_v2.sql（CREATE TABLE IF NOT EXISTS，可重复执行）
-- 幂等：全部写入使用 INSERT ... ON DUPLICATE KEY UPDATE；可重复执行
-- 权限：需要 SELECT(旧库)、INSERT/UPDATE(新库)、CREATE TEMPORARY TABLES、CREATE ROUTINE
-- 说明：目录树用临时表 + 存储过程重建；旧库文件夹行不会重复插入 t_file

SET NAMES utf8mb4;
SET @sentinel = '1970-01-01 00:00:00.000000';

-- ---------------------------------------------------------------------------
-- 1. 用户：保留 u_id；同一手机号只迁最小 u_id；密码保持旧 MD5，首次登录升级
-- ---------------------------------------------------------------------------
INSERT INTO t_user
  (u_id, u_tel, u_password, u_salt, u_pwd_algo, u_name, u_status, u_created_at, u_updated_at)
SELECT u.u_id,
       LEFT(TRIM(u.u_tel), 20),
       LEFT(u.u_password, 64),
       MD5(CONCAT(u.u_id, RAND(), NOW(6))),
       1,
       LEFT(IFNULL(NULLIF(TRIM(u.u_name), ''), CONCAT('用户', u.u_id)), 45),
       0,
       NOW(6),
       NOW(6)
  FROM NetDisk.t_user u
 WHERE u.u_id = (
       SELECT MIN(u2.u_id)
         FROM NetDisk.t_user u2
        WHERE TRIM(u2.u_tel) = TRIM(u.u_tel)
       )
ON DUPLICATE KEY UPDATE
  u_name = VALUES(u_name),
  u_updated_at = VALUES(u_updated_at);

-- ---------------------------------------------------------------------------
-- 2. 内容实体：只迁 file；同一 MD5 只保留最小 f_id；路径按内容寻址重算
-- ---------------------------------------------------------------------------
INSERT INTO t_file
  (f_id, f_md5, f_size, f_path, f_ref_count, f_state, f_created_at, f_updated_at)
SELECT f.f_id,
       LOWER(f.f_MD5),
       f.f_size,
       CONCAT(SUBSTRING(LOWER(f.f_MD5), 1, 2), '/',
              SUBSTRING(LOWER(f.f_MD5), 3, 2), '/',
              LOWER(f.f_MD5)),
       0,
       IF(f.f_state = 1, 1, 0),
       NOW(6),
       NOW(6)
  FROM NetDisk.t_file f
 WHERE f.f_type = 'file'
   AND f.f_id = (
       SELECT MIN(f2.f_id)
         FROM NetDisk.t_file f2
        WHERE f2.f_type = 'file'
          AND LOWER(f2.f_MD5) = LOWER(f.f_MD5)
       )
ON DUPLICATE KEY UPDATE
  f_size = VALUES(f_size),
  f_path = VALUES(f_path),
  f_state = VALUES(f_state),
  f_updated_at = VALUES(f_updated_at);

-- ---------------------------------------------------------------------------
-- 3. 目录树：为每个 f_dir 逐级建立目录节点，并记录 path -> uf_id
-- ---------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS tmp_dir_map;
CREATE TEMPORARY TABLE tmp_dir_map (
  u_id  BIGINT UNSIGNED NOT NULL,
  path  VARCHAR(512) NOT NULL,
  uf_id BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (u_id, path)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DROP PROCEDURE IF EXISTS cm_migrate_tree;
DELIMITER $$
CREATE PROCEDURE cm_migrate_tree()
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE v_uid BIGINT UNSIGNED;
  DECLARE v_dir VARCHAR(512);
  DECLARE v_parent BIGINT UNSIGNED;
  DECLARE v_seg VARCHAR(255);
  DECLARE v_path VARCHAR(512);
  DECLARE v_pos INT;
  DECLARE v_total INT;
  DECLARE v_existing BIGINT UNSIGNED;

  DECLARE cur CURSOR FOR
    SELECT u_id, f_dir FROM (
      SELECT DISTINCT uf.u_id, uf.f_dir
        FROM NetDisk.t_user_file uf
        JOIN t_user tu ON tu.u_id = uf.u_id
      UNION
      SELECT DISTINCT uf.u_id, CONCAT(uf.f_dir, uf.f_name, '/') AS f_dir
        FROM NetDisk.t_user_file uf
        JOIN NetDisk.t_file f ON f.f_id = uf.f_id AND f.f_type = 'folder'
        JOIN t_user tu ON tu.u_id = uf.u_id
    ) AS paths;

  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO v_uid, v_dir;
    IF done = 1 THEN
      LEAVE read_loop;
    END IF;

    INSERT INTO tmp_dir_map (u_id, path, uf_id) VALUES (v_uid, '/', 0)
      ON DUPLICATE KEY UPDATE uf_id = 0;

    SET v_parent = 0;
    SET v_path = '/';
    SET v_dir = TRIM(BOTH '/' FROM v_dir);

    IF v_dir <> '' THEN
      SET v_total = 1 + CHAR_LENGTH(v_dir) - CHAR_LENGTH(REPLACE(v_dir, '/', ''));
      SET v_pos = 1;
      WHILE v_pos <= v_total DO
        SET v_seg = LEFT(SUBSTRING_INDEX(SUBSTRING_INDEX(v_dir, '/', v_pos), '/', -1), 255);
        SET v_path = CONCAT(v_path, v_seg, '/');
        SET v_existing = (
          SELECT uf_id
            FROM t_user_file
           WHERE u_id = v_uid
             AND uf_parent_id = v_parent
             AND uf_name = v_seg
             AND uf_deleted_at = @sentinel
           LIMIT 1
        );

        IF v_existing IS NULL THEN
          INSERT INTO t_user_file
            (u_id, uf_parent_id, f_id, uf_name, uf_is_dir,
             uf_deleted_at, uf_created_at, uf_updated_at)
          VALUES
            (v_uid, v_parent, NULL, v_seg, 1,
             @sentinel, NOW(6), NOW(6));
          SET v_parent = LAST_INSERT_ID();
        ELSE
          SET v_parent = v_existing;
        END IF;

        INSERT INTO tmp_dir_map (u_id, path, uf_id) VALUES (v_uid, v_path, v_parent)
          ON DUPLICATE KEY UPDATE uf_id = VALUES(uf_id);

        SET v_pos = v_pos + 1;
      END WHILE;
    END IF;
  END LOOP;
  CLOSE cur;
END$$
DELIMITER ;

CALL cm_migrate_tree();
DROP PROCEDURE cm_migrate_tree;

-- ---------------------------------------------------------------------------
-- 4. 文件节点：按 MD5 关联 v2 的 f_id（兼容旧库同一内容多 f_id）
-- ---------------------------------------------------------------------------
INSERT INTO t_user_file
  (u_id, uf_parent_id, f_id, uf_name, uf_is_dir,
   uf_deleted_at, uf_created_at, uf_updated_at)
SELECT uf.u_id,
       COALESCE(m.uf_id, 0),
       tf.f_id,
       LEFT(uf.f_name, 255),
       0,
       @sentinel,
       uf.f_uploadtime,
       NOW(6)
  FROM NetDisk.t_user_file uf
  JOIN t_user tu ON tu.u_id = uf.u_id
  JOIN NetDisk.t_file lf
    ON lf.f_id = uf.f_id AND lf.f_type = 'file'
  JOIN t_file tf
    ON tf.f_md5 = LOWER(lf.f_MD5)
  LEFT JOIN tmp_dir_map m
    ON m.u_id = uf.u_id
   AND m.path = IF(TRIM(BOTH '/' FROM uf.f_dir) = '', '/',
                   CONCAT('/', TRIM(BOTH '/' FROM uf.f_dir), '/'))
ON DUPLICATE KEY UPDATE
  f_id = VALUES(f_id),
  uf_is_dir = 0,
  uf_updated_at = VALUES(uf_updated_at);

-- ---------------------------------------------------------------------------
-- 5. 分享：旧分享字段挂在 t_user_file 上，这里展开为独立 t_share
--    旧分享没有过期时间，统一给 7 天（如需永久，改 INTERVAL 7 DAY 即可）
-- ---------------------------------------------------------------------------
INSERT INTO t_share
  (s_code, s_user_id, s_uf_id, s_file_id, s_access_code,
   s_status, s_expire_at, s_created_at, s_updated_at)
SELECT LPAD(uf.s_link, 9, '0'),
       uf.u_id,
       n.uf_id,
       n.f_id,
       CASE
         WHEN uf.s_linkPassword IS NULL OR uf.s_linkPassword = 0 THEN NULL
         ELSE LPAD(uf.s_linkPassword, 4, '0')
       END,
       0,
       DATE_ADD(NOW(6), INTERVAL 7 DAY),
       IFNULL(uf.s_linkTime, NOW(6)),
       NOW(6)
  FROM NetDisk.t_user_file uf
  JOIN t_user tu ON tu.u_id = uf.u_id
  LEFT JOIN tmp_dir_map m
    ON m.u_id = uf.u_id
   AND m.path = IF(TRIM(BOTH '/' FROM uf.f_dir) = '', '/',
                   CONCAT('/', TRIM(BOTH '/' FROM uf.f_dir), '/'))
  JOIN t_user_file n
    ON n.u_id = uf.u_id
   AND n.uf_parent_id = COALESCE(m.uf_id, 0)
   AND n.uf_name = LEFT(uf.f_name, 255)
   AND n.uf_deleted_at = @sentinel
 WHERE uf.s_link IS NOT NULL
ON DUPLICATE KEY UPDATE
  s_uf_id = VALUES(s_uf_id),
  s_file_id = VALUES(s_file_id);

-- ---------------------------------------------------------------------------
-- 6. 重算引用计数；无引用的内容标记为孤儿
-- ---------------------------------------------------------------------------
UPDATE t_file f
   SET f_ref_count = (
       SELECT COUNT(*)
         FROM t_user_file uf
        WHERE uf.f_id = f.f_id
          AND uf.uf_deleted_at = @sentinel
       );

UPDATE t_file
   SET f_state = 2
 WHERE f_ref_count = 0
   AND f_state <> 3;

DROP TEMPORARY TABLE IF EXISTS tmp_dir_map;

-- ---------------------------------------------------------------------------
-- 7. 迁移报告（只读；期望无异常）
-- ---------------------------------------------------------------------------
SELECT 'users' AS item, COUNT(*) AS cnt FROM t_user
UNION ALL SELECT 'skipped_users', COUNT(*)
  FROM NetDisk.t_user u
 WHERE u.u_id <> (
       SELECT MIN(u2.u_id) FROM NetDisk.t_user u2
        WHERE TRIM(u2.u_tel) = TRIM(u.u_tel)
       )
UNION ALL SELECT 'files', COUNT(*) FROM t_file
UNION ALL SELECT 'nodes', COUNT(*) FROM t_user_file
UNION ALL SELECT 'shares', COUNT(*) FROM t_share;

SELECT 'orphan_blobs' AS item, COUNT(*) AS cnt FROM t_file WHERE f_state = 2
UNION ALL SELECT 'dangling_nodes', COUNT(*)
  FROM t_user_file u LEFT JOIN t_file f ON f.f_id = u.f_id
 WHERE u.f_id IS NOT NULL AND f.f_id IS NULL
UNION ALL SELECT 'ref_mismatch', COUNT(*)
  FROM t_file f
 WHERE f.f_ref_count <> (
       SELECT COUNT(*) FROM t_user_file u
        WHERE u.f_id = f.f_id AND u.uf_deleted_at = @sentinel
       )
UNION ALL SELECT 'bad_share_target', COUNT(*)
  FROM t_share s LEFT JOIN t_user_file u ON u.uf_id = s.s_uf_id
 WHERE u.uf_id IS NULL;
