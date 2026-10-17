-- ============================================================================
-- check_cache_triggers.sql
-- 缓存触发器健康检查（只读，由管理员或独立维护任务执行）。
--
-- 6 个缓存触发器负责在 face_employee / face_feature 发生增删改时递增
-- face_cache_revision，程序靠该版本号感知外部改动并刷新内存缓存。
-- 触发器由管理员在安装/迁移时执行 sql\migrate_personnel_source.sql 创建，
-- 程序运行账号只有 DML 权限，既不检查也不补建触发器。
--
-- 执行：
--   mysql -u root -p -D face_db < sql\check_cache_triggers.sql
--
-- 输出中 state = '缺失' 的触发器需要修复，修复方式是重新执行
-- sql\migrate_personnel_source.sql（该脚本会 DROP 后重建全部 6 个触发器）。
-- ============================================================================

USE `face_db`;

SELECT expected.trigger_name AS trigger_name,
       IF(t.TRIGGER_NAME IS NULL, '缺失', '存在') AS state
FROM (
    SELECT 'trg_face_employee_ai' AS trigger_name UNION ALL
    SELECT 'trg_face_employee_au' UNION ALL
    SELECT 'trg_face_employee_ad' UNION ALL
    SELECT 'trg_face_feature_ai'  UNION ALL
    SELECT 'trg_face_feature_au'  UNION ALL
    SELECT 'trg_face_feature_ad'
) AS expected
LEFT JOIN information_schema.TRIGGERS AS t
       ON t.TRIGGER_SCHEMA = DATABASE()
      AND t.TRIGGER_NAME = expected.trigger_name
ORDER BY expected.trigger_name;
