BEGIN;
SAVEPOINT pwh_test;
WITH delayed AS (SELECT pg_sleep(0.3))
SELECT COUNT(*) + 1 / CASE WHEN random() < 2 THEN 0 ELSE 1 END
FROM orders o
JOIN users u ON o.user_id = u.user_id
CROSS JOIN delayed;
ROLLBACK TO SAVEPOINT pwh_test;
SELECT pg_sleep(1);
COMMIT;
