-- 推荐先停用人员，保留历史日志：
-- UPDATE face_db.face_employee SET is_active = 0 WHERE employee_no = '工号';
--
-- 彻底删除人员会通过外键级联删除其全部人脸模板，face_log 历史记录保留。
-- 业务操作一律以 employee_no 为唯一标识，不使用自增 id。
SELECT * FROM face_db.face_employee;

DELETE FROM face_db.face_employee WHERE employee_no = '工号';
