-- Test HTTP server resilience and edge case handling.
-- This test stresses the HTTP server without querying actual metrics.

-- Test 1: Verify server responds to basic GET request.
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_basic.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_basic.txt')::text, E'[\\n\\r]+', '', 'g') = '200' AS basic_get_works;

-- Test 2: Concurrent requests - spawn 50 requests in parallel.
-- Server should handle concurrent load gracefully.
\! rm -f /tmp/pwh_test_parallel.txt /tmp/pwh_test_failures.txt; for i in $(seq 1 50); do (curl -fsS -o /dev/null http://localhost:9187/metrics || echo fail >> /tmp/pwh_test_failures.txt) & done; wait; test ! -s /tmp/pwh_test_failures.txt && echo ok > /tmp/pwh_test_parallel.txt
SELECT pg_read_file('/tmp/pwh_test_parallel.txt')::text = E'ok\n' AS concurrent_requests_completed;

-- Test 3: Invalid HTTP method - POST to /metrics should return 404.
\! curl -s -X POST -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_post.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_post.txt')::text, E'[\\n\\r]+', '', 'g') = '404' AS post_returns_404;

-- Test 4: Invalid HTTP method - PUT to /metrics should return 404.
\! curl -s -X PUT -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_put.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_put.txt')::text, E'[\\n\\r]+', '', 'g') = '404' AS put_returns_404;

-- Test 5: Invalid HTTP method - DELETE to /metrics should return 404.
\! curl -s -X DELETE -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_delete.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_delete.txt')::text, E'[\\n\\r]+', '', 'g') = '404' AS delete_returns_404;

-- Test 6: Invalid path - /notfound should return 404.
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/notfound > /tmp/pwh_test_notfound.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_notfound.txt')::text, E'[\\n\\r]+', '', 'g') = '404' AS invalid_path_returns_404;

-- Test 7: Invalid path - /metrics/foo should return 404.
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics/foo > /tmp/pwh_test_subpath.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_subpath.txt')::text, E'[\\n\\r]+', '', 'g') = '404' AS subpath_returns_404;

-- Test 8: Verify server still responds after stress.
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_final.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_final.txt')::text, E'[\\n\\r]+', '', 'g') = '200' AS server_still_healthy;

-- Test 9: Query parameters do not change endpoint routing.
\! curl -s -o /dev/null -w "%{http_code}" 'http://localhost:9187/metrics?probe=1' > /tmp/pwh_test_query.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_query.txt')::text, E'[\\n\\r]+', '', 'g') = '200' AS query_parameters_work;

-- Test 10: An incomplete request line does not damage the server.
\! printf 'GET /metrics\r\n\r\n' | nc -w 2 localhost 9187 > /dev/null || true
\! curl -s -o /dev/null -w "%{http_code}" http://localhost:9187/metrics > /tmp/pwh_test_malformed.txt
SELECT regexp_replace(pg_read_file('/tmp/pwh_test_malformed.txt')::text, E'[\\n\\r]+', '', 'g') = '200' AS malformed_request_survived;

-- Cleanup temp files.
\! rm -f /tmp/pwh_test_*.txt
