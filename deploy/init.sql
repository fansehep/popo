-- 用 mysql 客户端连上 root 之后手动执行。服务不会自动跑这个文件。
-- 用户名和主机都要加引号。'%' 匹配任意地址，127.0.0.1 只匹配本机 TCP。

CREATE USER IF NOT EXISTS 'app'@'%' IDENTIFIED BY 'app_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON popo.* TO 'app'@'%';

CREATE USER IF NOT EXISTS 'report'@'127.0.0.1' IDENTIFIED BY 'report_password';
GRANT SELECT ON popo.* TO 'report'@'127.0.0.1';

-- SHOW GRANTS FOR 'app'@'%';
-- ALTER USER 'app'@'%' IDENTIFIED BY 'new_password';
-- SET PASSWORD FOR 'app'@'%' = 'new_password';
-- REVOKE INSERT ON popo.* FROM 'app'@'%';
-- DROP USER IF EXISTS 'app'@'%';
