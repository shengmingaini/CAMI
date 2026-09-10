-- database/migrations/002_mail_expire_index.sql
--
-- TASK-028 · 002：邮件过期扫描索引（§17 集成用例要求「迁移从 001 到 002 平滑升级」）。
--
-- 说明：本迁移**故意保持幂等**（IF NOT EXISTS），因为 MySQL 的 DDL 隐式提交、
-- 无法回滚；一旦中途失败，schema_migrations 会留下 success=0 的补偿记录（§21），
-- 修复后重跑必须安全，不得出现「半应用状态」。
--
-- 注意：MySQL 不支持 CREATE INDEX IF NOT EXISTS，故用 information_schema 判断后
-- 动态执行；这里是演示「幂等 DDL 的另一种写法」，保证重复执行不报错。

SET @idx_exists := (
  SELECT COUNT(*) FROM information_schema.statistics
  WHERE table_schema = DATABASE() AND table_name = 'mail'
    AND index_name = 'idx_mail_expire'
);
SET @ddl := IF(@idx_exists = 0,
  'CREATE INDEX idx_mail_expire ON mail (expire_at)',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
