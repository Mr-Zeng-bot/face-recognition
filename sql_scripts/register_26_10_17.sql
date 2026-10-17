SELECT * FROM face_db.face_employee;

INSERT INTO face_db.face_employee
    (employee_no, name, department, is_active)
VALUES
    ('10001', '示例员工', '示例部门', 1)
ON DUPLICATE KEY UPDATE
    name = VALUES(name),
    department = VALUES(department),
    is_active = 1;
    
    SELECT employee_no, COUNT(*) AS template_count
FROM face_db.face_feature
WHERE employee_no = '10001'
GROUP BY employee_no;