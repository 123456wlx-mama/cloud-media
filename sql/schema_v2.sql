-- cloud-media schema v2
-- 目标：MySQL 5.7 / InnoDB / utf8mb4 / ROW_FORMAT=DYNAMIC
-- 说明：本文件只建表，不建库、不切换库；执行前先创建并 USE 目标库。
--       重复执行安全（CREATE TABLE IF NOT EXISTS），但不会修改已存在表的结构。

SET NAMES utf8mb4;

CREATE TABLE IF NOT EXISTS t_user (
  u_id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '用户ID',
  u_tel        VARCHAR(20)  CHARACTER SET ascii NOT NULL COMMENT '手机号/登录名',
  u_password   CHAR(64)     CHARACTER SET ascii NOT NULL COMMENT 'SHA-256(salt+pwd)；迁移期可为32位MD5',
  u_salt       CHAR(32)     CHARACTER SET ascii NOT NULL COMMENT '每用户随机盐',
  u_pwd_algo   TINYINT UNSIGNED NOT NULL DEFAULT 2 COMMENT '1=旧MD5 2=SHA-256',
  u_name       VARCHAR(45)  NOT NULL COMMENT '昵称',
  u_status     TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0正常 1禁用',
  u_created_at DATETIME(6)  NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  u_updated_at DATETIME(6)  NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (u_id),
  UNIQUE KEY uk_u_tel (u_tel)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='用户';

CREATE TABLE IF NOT EXISTS t_file (
  f_id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '文件实体ID',
  f_md5        CHAR(32) CHARACTER SET ascii NOT NULL COMMENT '内容MD5，小写hex',
  f_size       BIGINT UNSIGNED NOT NULL COMMENT '字节数',
  f_path       VARCHAR(128) CHARACTER SET ascii NOT NULL COMMENT '相对 blob 根：aa/bb/<md5>',
  f_ref_count  INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '未删除引用数',
  f_state      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0上传中 1就绪 2孤儿 3缺失',
  f_created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  f_updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (f_id),
  UNIQUE KEY uk_f_md5 (f_md5),
  KEY idx_state_created (f_state, f_created_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='文件内容实体';

CREATE TABLE IF NOT EXISTS t_user_file (
  uf_id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '节点ID',
  u_id          BIGINT UNSIGNED NOT NULL COMMENT '所属用户',
  uf_parent_id  BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '父节点；0=根',
  f_id          BIGINT UNSIGNED NULL DEFAULT NULL COMMENT '目录为NULL',
  uf_name       VARCHAR(255) NOT NULL COMMENT '显示名',
  uf_is_dir     TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1=目录',
  uf_deleted_at DATETIME(6) NOT NULL DEFAULT '1970-01-01 00:00:00.000000' COMMENT '哨兵=未删除',
  uf_created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  uf_updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (uf_id),
  UNIQUE KEY uk_user_parent_name (u_id, uf_parent_id, uf_name, uf_deleted_at),
  KEY idx_list   (u_id, uf_parent_id, uf_deleted_at, uf_name),
  KEY idx_name   (u_id, uf_name),
  KEY idx_recent (u_id, uf_deleted_at, uf_updated_at),
  -- 按创建时间倒序分页（最近上传）：key 顺序必须与 ORDER BY 一致，否则走 filesort
  KEY idx_recent_created (u_id, uf_deleted_at, uf_created_at),
  KEY idx_file   (f_id, u_id)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='用户目录节点';

CREATE TABLE IF NOT EXISTS t_chunk (
  c_task_id    CHAR(32) CHARACTER SET ascii NOT NULL COMMENT '对应 t_transfer_task.tr_task_id',
  c_index      INT UNSIGNED NOT NULL COMMENT '分片序号，从0开始',
  c_offset     BIGINT UNSIGNED NOT NULL COMMENT '文件内偏移',
  c_size       INT UNSIGNED NOT NULL COMMENT '本片字节数',
  c_md5        CHAR(32) CHARACTER SET ascii NOT NULL COMMENT '分片MD5',
  c_state      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0写入中 1完成',
  c_path       VARCHAR(160) CHARACTER SET ascii NOT NULL COMMENT '临时分片相对路径',
  c_created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  c_updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (c_task_id, c_index),
  KEY idx_updated (c_updated_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='上传分片';

CREATE TABLE IF NOT EXISTS t_transfer_task (
  tr_id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  tr_task_id      CHAR(32) CHARACTER SET ascii NOT NULL COMMENT '对外任务ID(UUID hex)',
  tr_user_id      BIGINT UNSIGNED NOT NULL,
  tr_type         TINYINT UNSIGNED NOT NULL COMMENT '1上传 2转存 3秒传',
  tr_file_md5     CHAR(32) CHARACTER SET ascii NULL DEFAULT NULL,
  tr_file_name    VARCHAR(255) NOT NULL,
  tr_total_size   BIGINT UNSIGNED NOT NULL,
  tr_chunk_size   INT UNSIGNED NOT NULL DEFAULT 4194304,
  tr_total_chunks INT UNSIGNED NOT NULL DEFAULT 0,
  tr_done_chunks  INT UNSIGNED NOT NULL DEFAULT 0,
  tr_offset       BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '派生值，可重算',
  tr_parent_uf_id BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '目标目录；0=根',
  tr_status       TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0待开始 1进行中 2暂停 3失败 4完成 5取消 6过期',
  tr_error_code   INT NOT NULL DEFAULT 0,
  tr_expire_at    DATETIME(6) NOT NULL,
  tr_created_at   DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  tr_updated_at   DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (tr_id),
  UNIQUE KEY uk_task_id (tr_task_id),
  KEY idx_user_type_status (tr_user_id, tr_type, tr_status, tr_updated_at),
  KEY idx_expire (tr_status, tr_expire_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='传输任务（写侧）';

CREATE TABLE IF NOT EXISTS t_share (
  s_id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  s_code        CHAR(9) CHARACTER SET ascii NOT NULL COMMENT '9位数字，首位1-9',
  s_user_id     BIGINT UNSIGNED NOT NULL,
  s_uf_id       BIGINT UNSIGNED NOT NULL COMMENT '被分享的节点',
  s_file_id     BIGINT UNSIGNED NULL DEFAULT NULL COMMENT '目录分享为NULL',
  s_access_code CHAR(4) CHARACTER SET ascii NULL DEFAULT NULL COMMENT '可选提取码',
  s_status      TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0有效 1取消',
  s_expire_at   DATETIME(6) NOT NULL,
  s_created_at  DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  s_updated_at  DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (s_id),
  UNIQUE KEY uk_s_code (s_code),
  KEY idx_user_status_created (s_user_id, s_status, s_created_at),
  KEY idx_status_expire (s_status, s_expire_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='分享';

CREATE TABLE IF NOT EXISTS t_play_record (
  p_id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  p_user_id     BIGINT UNSIGNED NOT NULL,
  p_file_id     BIGINT UNSIGNED NOT NULL,
  p_position_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
  p_duration_ms BIGINT UNSIGNED NOT NULL DEFAULT 0,
  p_finished    TINYINT UNSIGNED NOT NULL DEFAULT 0,
  p_updated_at  DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (p_id),
  UNIQUE KEY uk_user_file (p_user_id, p_file_id),
  KEY idx_user_updated (p_user_id, p_updated_at)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COMMENT='播放记录';
