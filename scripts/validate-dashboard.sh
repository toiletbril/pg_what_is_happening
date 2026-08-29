#!/bin/sh

set -eu

dashboard=${1:-example/grafana/provisioning/dashboards/pg_what_is_happening.json}

jq -e '
  ([.templating.list[].name] | index("pid") != null) and
  ([.. | objects | .expr? // empty] | all(.[];
    ((split("query_id=~\"$query_id\"") | length) ==
     (split("query_id=~\"$query_id\",pid=~\"$pid\"") | length)))) and
  ([.. | objects | .expr? // empty] |
    all(.[]; contains("on(query_id, parent_node_id)") | not))
' "$dashboard" >/dev/null
