-- ============================================================================
-- CAMI DDL 验证 + 账号角色上限(n-cap) 演示脚本
-- 针对库: cami_global  (账号域 + 分片支撑表)
-- 运行方式 (沙箱 Docker):
--   docker exec -i <mysql-global容器名> mysql -uroot -pcami_dev_2026 cami_global \
--     < docker/mysql/verify-ddl.sql
-- 运行方式 (本地 MySQL):
--   mysql -uroot -p cami_global < docker/mysql/verify-ddl.sql
-- ============================================================================

SELECT '=== 1. cami_global 表清单 ===' AS step;
SHOW TABLES;

SELECT '=== 2. account.character_count 列 + CHECK 约束 ===' AS step;
SHOW COLUMNS FROM account LIKE 'character_count';
SELECT tc.CONSTRAINT_NAME, cc.CHECK_CLAUSE
  FROM information_schema.CHECK_CONSTRAINTS cc
  JOIN information_schema.TABLE_CONSTRAINTS tc
    ON cc.CONSTRAINT_NAME = tc.CONSTRAINT_NAME
 WHERE tc.TABLE_SCHEMA = 'cami_global'
   AND tc.TABLE_NAME   = 'account'
   AND tc.CONSTRAINT_TYPE = 'CHECK';

SELECT '=== 3. 分片支撑表存在性 (account_character / player_name_reservation) ===' AS step;
SELECT TABLE_NAME
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = 'cami_global'
   AND TABLE_NAME IN ('account_character', 'player_name_reservation');

-- ===================== 账号创角上限演示 (业务 n = 5) =====================
-- 强制逻辑: Data Service 用原子条件更新占名额, affected_rows=0 即拒绝。
--   UPDATE account SET character_count = character_count + 1
--    WHERE account_id = ? AND character_count < n;
SELECT '=== 4. 创角上限演示: 单账号最多 5 角色 ===' AS step;
SET @n = 5;
INSERT INTO account (username, pass_hash, status) VALUES ('__ddl_verify__', 0xDEADBEEF, 1);
SET @aid = LAST_INSERT_ID();

-- 第 1~5 次: 应成功 (ROW_COUNT = 1)
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_1;
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_2;
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_3;
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_4;
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_5;

-- 第 6 次: 应被拒 (ROW_COUNT = 0)
UPDATE account SET character_count = character_count + 1 WHERE account_id = @aid AND character_count < @n; SELECT ROW_COUNT() AS attempt_6_rejected;

SELECT character_count AS final_count_expect_5 FROM account WHERE account_id = @aid;

-- 清理演示账号
DELETE FROM account WHERE account_id = @aid;
SELECT '=== 验证完成, 演示账号已清理 ===' AS step;
