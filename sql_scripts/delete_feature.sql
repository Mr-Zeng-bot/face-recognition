-- 删除某工号下的全部人脸模板；删除后缓存版本触发器自动递增，程序约 1 秒内刷新。
-- 业务操作一律以 employee_no 为唯一标识，不使用自增 id。
SELECT * FROM face_db.face_feature;

DELETE FROM face_db.face_feature WHERE employee_no = '工号';
