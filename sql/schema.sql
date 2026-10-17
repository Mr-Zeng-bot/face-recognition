-- ============================================================================
-- schema.sql —— 人脸识别系统 MySQL 建表语句
-- 数据库字符集必须为 utf8mb4，否则中文姓名/路径会乱码。
--
-- 使用方式（命令行）：
--   mysql -u root -p < schema.sql
-- 或在 MySQL Workbench / Navicat 中直接执行。
-- ============================================================================

CREATE DATABASE IF NOT EXISTS `face_db`
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_general_ci;

USE `face_db`;

-- 人员主数据由 MySQL 管理员维护。程序只读取，不创建/覆盖人员。
CREATE TABLE IF NOT EXISTS `face_employee` (
    `id`          BIGINT       NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    `employee_no` VARCHAR(32)  NOT NULL                COMMENT '工号',
    `name`        VARCHAR(64)  DEFAULT NULL            COMMENT '姓名',
    `department`  VARCHAR(64)  DEFAULT NULL            COMMENT '部门',
    `is_active`   TINYINT(1)   NOT NULL DEFAULT 1      COMMENT '是否启用：1启用，0停用',
    `created_at`  DATETIME     DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    `updated_at`  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP
                              ON UPDATE CURRENT_TIMESTAMP COMMENT '最后更新时间',
    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_employee_no` (`employee_no`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='员工信息表';

-- 特征表由程序写入；人员删除时级联删除该员工的全部模板。
CREATE TABLE IF NOT EXISTS `face_feature` (
    `id`            BIGINT      NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    `employee_no`   VARCHAR(32) NOT NULL                COMMENT '工号',
    `feature`       BLOB        NOT NULL                COMMENT 'ArcFace 512 维 float 特征',
    `feature_dim`   INT         NOT NULL DEFAULT 512    COMMENT '特征维度',
    `model_version` VARCHAR(64) DEFAULT NULL            COMMENT '提取特征所用模型标识',
    `created_at`    DATETIME    DEFAULT CURRENT_TIMESTAMP COMMENT '写入时间',
    PRIMARY KEY (`id`),
    KEY `idx_employee_no` (`employee_no`),
    CONSTRAINT `fk_feature_employee`
        FOREIGN KEY (`employee_no`) REFERENCES `face_employee` (`employee_no`)
        ON UPDATE CASCADE ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='人脸特征表';

-- 日志只追加，不随人员删除；保留识别当时的人员快照。
CREATE TABLE IF NOT EXISTS `face_log` (
    `id`           BIGINT       NOT NULL AUTO_INCREMENT COMMENT '自增主键',
    `employee_no`  VARCHAR(32)  DEFAULT NULL            COMMENT '识别到的工号',
    `name`         VARCHAR(64)  DEFAULT NULL            COMMENT '识别到的姓名',
    `similarity`   FLOAT        DEFAULT NULL            COMMENT '库中最高余弦相似度',
    `result`       VARCHAR(32)  DEFAULT NULL            COMMENT '匹配 / 未匹配',
    `image_path`   VARCHAR(255) DEFAULT NULL            COMMENT '抓拍人脸图片路径',
    `capture_time` DATETIME     DEFAULT CURRENT_TIMESTAMP COMMENT '抓拍时间',
    PRIMARY KEY (`id`),
    KEY `idx_capture_time` (`capture_time`),
    KEY `idx_employee_no` (`employee_no`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='识别日志表';

-- 小程序直接轮询一行版本号，避免每秒重载全部特征。
CREATE TABLE IF NOT EXISTS `face_cache_revision` (
    `id`       TINYINT PRIMARY KEY,
    `revision` BIGINT  NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='人员缓存版本号';

CREATE TABLE IF NOT EXISTS `face_schema_meta` (
    `id`             TINYINT PRIMARY KEY,
    `schema_version` INT NOT NULL DEFAULT 0,
    `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                     ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='数据库结构版本';

INSERT IGNORE INTO `face_cache_revision` (`id`, `revision`) VALUES (1, 0);

-- 人员和模板的任意变化都会递增缓存版本，程序在下个轮询周期自动刷新。
DROP TRIGGER IF EXISTS `trg_face_employee_ai`;
DROP TRIGGER IF EXISTS `trg_face_employee_au`;
DROP TRIGGER IF EXISTS `trg_face_employee_ad`;
DROP TRIGGER IF EXISTS `trg_face_feature_ai`;
DROP TRIGGER IF EXISTS `trg_face_feature_au`;
DROP TRIGGER IF EXISTS `trg_face_feature_ad`;

DELIMITER //

CREATE TRIGGER `trg_face_employee_ai` AFTER INSERT ON `face_employee`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

CREATE TRIGGER `trg_face_employee_au` AFTER UPDATE ON `face_employee`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

CREATE TRIGGER `trg_face_employee_ad` AFTER DELETE ON `face_employee`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

CREATE TRIGGER `trg_face_feature_ai` AFTER INSERT ON `face_feature`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

CREATE TRIGGER `trg_face_feature_au` AFTER UPDATE ON `face_feature`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

CREATE TRIGGER `trg_face_feature_ad` AFTER DELETE ON `face_feature`
FOR EACH ROW
BEGIN
    UPDATE `face_cache_revision` SET `revision` = `revision` + 1 WHERE `id` = 1;
END//

DELIMITER ;

INSERT INTO `face_schema_meta` (`id`, `schema_version`) VALUES (1, 1)
ON DUPLICATE KEY UPDATE `schema_version` = 1;

-- 常用查询示例：
-- 启用人员：
--   SELECT employee_no, name, department FROM face_employee WHERE is_active = 1;
-- 停用人员：
--   UPDATE face_employee SET is_active = 0 WHERE employee_no = '工号';
-- 删除人员和全部模板（日志保留）：
--   DELETE FROM face_employee WHERE employee_no = '工号';
