#ifndef PWH_GUCS_H
#define PWH_GUCS_H

#include "postgres.h"

#include "common.h"
#include "shared_memory.h"
#include "utils/guc.h"

#ifdef WITH_BGWORKER
#include "bg_worker.h"
#endif

#define PWH_GUC_SCHEMA "what_is_happening."

#ifdef WITH_BGWORKER
#define PWH_GUC_METRICS_LISTEN_ADDRESS_NAME \
	(PWH_GUC_SCHEMA "metrics_listen_address")
extern char *PWH_GUC_METRICS_LISTEN_ADDRESS;
#endif

#define PWH_GUC_IS_ENABLED_NAME (PWH_GUC_SCHEMA "is_enabled")
extern bool PWH_GUC_IS_ENABLED;

#define PWH_GUC_MAX_TRACKED_QUERIES_NAME (PWH_GUC_SCHEMA "max_tracked_queries")
extern i32 PWH_GUC_MAX_TRACKED_QUERIES;

#define PWH_GUC_MAX_NODES_PER_QUERY_NAME (PWH_GUC_SCHEMA "max_nodes_per_query")
extern i32 PWH_GUC_MAX_NODES_PER_QUERY;

#define PWH_GUC_MAX_QUERY_TEXT_LEN_NAME (PWH_GUC_SCHEMA "max_query_text_length")
extern i32 PWH_GUC_MAX_QUERY_TEXT_LEN;

#define PWH_GUC_SIGNAL_TIMEOUT_MS_NAME (PWH_GUC_SCHEMA "signal_timeout_ms")
extern i32 PWH_GUC_SIGNAL_TIMEOUT_MS;

#define PWH_GUC_MIN_COST_TO_TRACK_NAME (PWH_GUC_SCHEMA "min_cost_to_track")
extern double PWH_GUC_MIN_COST_TO_TRACK;

extern void pwh_define_gucs(void);

#endif /* PWH_GUCS_H */
