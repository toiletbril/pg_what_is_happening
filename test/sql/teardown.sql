-- Terminate any active connections that might be holding locks.
DO $$
DECLARE
  r RECORD;
BEGIN
  FOR r IN
    SELECT pid
    FROM pg_stat_activity
    WHERE datname = current_database()
      AND pid != pg_backend_pid()
  LOOP
    PERFORM pg_terminate_backend(r.pid);
  END LOOP;
END $$;

SELECT pg_sleep(0.1);

DROP TABLE IF EXISTS promotions CASCADE;
DROP TABLE IF EXISTS returns CASCADE;
DROP TABLE IF EXISTS shipping CASCADE;
DROP TABLE IF EXISTS warehouses CASCADE;
DROP TABLE IF EXISTS inventory CASCADE;
DROP TABLE IF EXISTS reviews CASCADE;
DROP TABLE IF EXISTS payments CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS products CASCADE;
DROP TABLE IF EXISTS users CASCADE;

DROP EXTENSION IF EXISTS pg_what_is_happening CASCADE;

-- Goodbye.
