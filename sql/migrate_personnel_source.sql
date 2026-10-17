-- ============================================================================
-- migrate_personnel_source.sql
-- 将旧版 face_db 升级为「MySQL 管人员、程序管特征」结构。
--
-- 适用于已经存在 face_employee / face_feature / face_log 的数据库。
-- 建议先用 mysqldump 备份，再由 MySQL 管理员执行：
--   mysql -u root -p < sql\migrate_personnel_source.sql
-- ============================================================================

USE `face_db`;

DELIMITER //

DROP PROCEDURE IF EXISTS `migrate_personnel_source`//

CREATE PROCEDURE `migrate_personnel_source`()
BEGIN
    DECLARE orphan_count INT DEFAULT 0;

    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'face_employee'
          AND COLUMN_NAME = 'is_active'
    ) THEN
        ALTER TABLE `face_employee`
            ADD COLUMN `is_active` TINYINT(1) NOT NULL DEFAULT 1
            COMMENT '是否启用：1启用，0停用' AFTER `department`;
    END IF;

    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = 'face_employee'
          AND COLUMN_NAME = 'updated_at'
    ) THEN
        ALTER TABLE `face_employee`
            ADD COLUMN `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
            ON UPDATE CURRENT_TIMESTAMP COMMENT '最后更新时间' AFTER `created_at`;
    END IF;

    SELECT COUNT(*) INTO orphan_count
    FROM `face_feature` f
    LEFT JOIN `face_employee` e ON e.`employee_no` = f.`employee_no`
    WHERE e.`employee_no` IS NULL;

    IF orphan_count > 0 THEN
        SIGNAL SQLSTATE '45000'
            SET MESSAGE_TEXT = 'face_feature contains rows without matching face_employee';
    END IF;

    IF NOT EXISTS (
        SELECT 1 FROM information_schema.TABLE_CONSTRAINTS
        WHERE CONSTRAINT_SCHEMA = DATABASE()
          AND TABLE_NAME = 'face_feature'
          AND CONSTRAINT_NAME = 'fk_feature_employee'
    ) THEN
        ALTER TABLE `face_feature`
            ADD CONSTRAINT `fk_feature_employee`
            FOREIGN KEY (`employee_no`) REFERENCES `face_employee` (`employee_no`)
            ON UPDATE CASCADE ON DELETE CASCADE;
    END IF;

    CREATE TABLE IF NOT EXISTS `face_cache_revision` (
        `id`       TINYINT PRIMARY KEY,
        `revision` BIGINT NOT NULL DEFAULT 0
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='人员缓存版本号';

    CREATE TABLE IF NOT EXISTS `face_schema_meta` (
        `id`             TINYINT PRIMARY KEY,
        `schema_version` INT NOT NULL DEFAULT 0,
        `updated_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                         ON UPDATE CURRENT_TIMESTAMP
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='数据库结构版本';

    INSERT IGNORE INTO `face_cache_revision` (`id`, `revision`) VALUES (1, 0);
END//

DELIMITER ;

CALL `migrate_personnel_source`();
DROP PROCEDURE IF EXISTS `migrate_personnel_source`;

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
