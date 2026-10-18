-- ============================================================================
--  NetDisk 网盘 —— 服务端 MySQL 数据库初始化脚本
--  数据库：NetDisk   字符集：utf8   引擎：InnoDB
--  适用：server/linux-epoll（Ubuntu + libmysqlclient 版本服务端）
--
--  设计核心：
--    1) t_file        文件实体表（同一份内容全服只存一份，靠 f_MD5 去重实现“秒传”）
--    2) t_user_file   用户-文件关系表（f_dir / f_name 描述“这个用户在哪个目录看到它”）
--    3) f_count       引用计数，由触发器 refPlusOne / refMinusOne 自动维护
--    4) user_file_info 视图，把上面两张表 join 起来，业务 SQL 直接查视图
-- ============================================================================

CREATE DATABASE IF NOT EXISTS `NetDisk` DEFAULT CHARACTER SET utf8 COLLATE utf8_general_ci;
USE `NetDisk`;

-- ---------------------------------------------------------------------------
-- 1. 用户表
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS `t_user`;
CREATE TABLE `t_user` (
  `u_id`       bigint(11)  NOT NULL AUTO_INCREMENT COMMENT '用户ID，注册时自增生成',
  `u_tel`      varchar(20) NOT NULL               COMMENT '手机号（登录账号）',
  `u_password` varchar(45) NOT NULL               COMMENT '密码 MD5（客户端加盐 1234 后生成）',
  `u_name`     varchar(45) NOT NULL               COMMENT '昵称',
  PRIMARY KEY (`u_id`)
) ENGINE=InnoDB AUTO_INCREMENT=9 DEFAULT CHARSET=utf8;

-- ---------------------------------------------------------------------------
-- 2. 文件实体表（按内容去重：同一 f_MD5 全服只有一条记录）
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS `t_file`;
CREATE TABLE `t_file` (
  `f_id`    bigint(11)   NOT NULL AUTO_INCREMENT COMMENT '文件ID（全局唯一，全服共享）',
  `f_size`  bigint(11)   NOT NULL                COMMENT '文件大小（字节）',
  `f_path`  varchar(260) NOT NULL                COMMENT '服务端磁盘绝对路径',
  `f_count` bigint(11)   NOT NULL                COMMENT '引用计数：多少个用户关系指向我',
  `f_MD5`   varchar(45)  NOT NULL                COMMENT '文件内容MD5，秒传判重依据',
  `f_state` int(11)      NOT NULL                COMMENT '0=上传中 1=已完成（只有1才会出现在列表里）',
  `f_type`  varchar(10)  NOT NULL                COMMENT 'file / folder',
  PRIMARY KEY (`f_id`)
) ENGINE=InnoDB AUTO_INCREMENT=120 DEFAULT CHARSET=utf8;

-- ---------------------------------------------------------------------------
-- 3. 用户-文件关系表（一个用户目录下看到的一个条目）
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS `t_user_file`;
CREATE TABLE `t_user_file` (
  `u_id`            bigint(11)   NOT NULL         COMMENT '用户ID',
  `f_id`            bigint(11)   NOT NULL         COMMENT '文件ID（指向 t_file）',
  `f_dir`           varchar(260) NOT NULL         COMMENT '该文件在用户网盘中的目录，如 /05/',
  `f_name`          varchar(260) NOT NULL         COMMENT '该文件在用户网盘中的显示名',
  `f_uploadtime`    datetime     NOT NULL         COMMENT '上传/创建时间',
  `s_link`          bigint(11)   DEFAULT NULL     COMMENT '分享码（9位：1-9 + 8位随机）',
  `s_linkPassword`  bigint(4)    DEFAULT NULL     COMMENT '分享密码（预留，暂未实现）',
  `s_linkTime`      datetime     DEFAULT NULL     COMMENT '分享时间'
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- ---------------------------------------------------------------------------
-- 4. 触发器：维护 f_count 引用计数
--    - 插入用户-文件关系 → 引用计数 +1
--    - 删除用户-文件关系 → 引用计数 -1，减到 0 时物理删除 t_file 记录
--      （服务端业务代码在删除关系后会再查一次 t_file，查不到就 unlink 磁盘文件）
-- ---------------------------------------------------------------------------
DROP TRIGGER IF EXISTS `refPlusOne`;
DELIMITER $$
CREATE TRIGGER `refPlusOne` AFTER INSERT ON `t_user_file`
FOR EACH ROW
BEGIN
    UPDATE t_file SET f_count = f_count + 1 WHERE t_file.f_id = NEW.f_id;
END$$
DELIMITER ;

DROP TRIGGER IF EXISTS `refMinusOne`;
DELIMITER $$
CREATE TRIGGER `refMinusOne` AFTER DELETE ON `t_user_file`
FOR EACH ROW
BEGIN
    UPDATE t_file SET f_count = f_count - 1 WHERE t_file.f_id = OLD.f_id;
    DELETE FROM t_file WHERE f_count = 0 AND t_file.f_id = OLD.f_id;
END$$
DELIMITER ;

-- ---------------------------------------------------------------------------
-- 5. 视图：业务层统一查询入口（t_file JOIN t_user_file）
--    服务端 GetFileInfoRq / DownloadFileRq / DeleteFileRq / MyShareRq 等
--    全部查询这个视图，取出时是“扁平字段列表”，按列数依次 pop_front 解析。
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS `user_file_info`;
CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `user_file_info` AS
select
  `t_file`.`f_size`        AS `f_size`,
  `t_file`.`f_MD5`         AS `f_MD5`,
  `t_file`.`f_type`        AS `f_type`,
  `t_file`.`f_path`        AS `f_path`,
  `t_file`.`f_count`       AS `f_count`,
  `t_file`.`f_state`       AS `f_state`,
  `t_user_file`.`u_id`     AS `u_id`,
  `t_user_file`.`f_id`     AS `f_id`,
  `t_user_file`.`f_dir`    AS `f_dir`,
  `t_user_file`.`f_name`   AS `f_name`,
  `t_user_file`.`f_uploadtime` AS `f_uploadtime`,
  `t_user_file`.`s_link`   AS `s_link`,
  `t_user_file`.`s_linkPassword` AS `s_linkPassword`,
  `t_user_file`.`s_linkTime`     AS `s_linkTime`
from (`t_file` join `t_user_file` on((`t_file`.`f_id` = `t_user_file`.`f_id`)));

-- ============================================================================
--  客户端侧（Qt 客户端）使用的是 SQLite，不在此脚本中。
--  客户端会在 exe 同级 database/<userid>.db 中自动建表：
--    t_upload   / t_download   —— 断点续传任务缓存，字段见 ckernel.cpp::InitDatabase
--  Qt 版服务端会在 exe 同级 NetDeskServer.db 中自动建表：
--    t_user(id, tel, password, name)
-- ============================================================================
