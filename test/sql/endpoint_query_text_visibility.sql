\! psql -d contrib_regression -c "SELECT pg_sleep(2);" > /dev/null 2>&1 &
SELECT pg_sleep(0.1);

\! curl -fsS http://localhost:9187/metrics > /tmp/pwh_default_metrics.txt && grep -q 'query_text=""' /tmp/pwh_default_metrics.txt && echo true > /tmp/pwh_default_redaction.txt
\set default_redaction `tr -d '[:space:]' < /tmp/pwh_default_redaction.txt`
SELECT :'default_redaction' = 'true' AS query_text_redacted_by_default;

ALTER SYSTEM SET what_is_happening.metrics_expose_query_text = on;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

\! curl -fsS http://localhost:9187/metrics > /tmp/pwh_exposed_metrics.txt && grep -q 'query_text="SELECT pg_sleep(2);"' /tmp/pwh_exposed_metrics.txt && echo true > /tmp/pwh_explicit_exposure.txt
\set explicit_exposure `tr -d '[:space:]' < /tmp/pwh_explicit_exposure.txt`
SELECT :'explicit_exposure' = 'true' AS query_text_exposed_explicitly;

ALTER SYSTEM RESET what_is_happening.metrics_expose_query_text;
SELECT pg_reload_conf();
SELECT pg_sleep(2);
\! rm -f /tmp/pwh_default_redaction.txt /tmp/pwh_explicit_exposure.txt

-- End.
