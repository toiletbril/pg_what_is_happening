/*
 * Extension-related variables.
 */

#include "gucs.h"

bool PWH_GUC_IS_ENABLED = true;
bool PWH_GUC_METRICS_EXPOSE_QUERY_TEXT = false;
#ifdef WITH_BGWORKER
char *PWH_GUC_METRICS_LISTEN_ADDRESS = NULL;
#endif

i32 PWH_GUC_MAX_TRACKED_QUERIES = 32;
i32 PWH_GUC_MAX_NODES_PER_QUERY = 128;
i32 PWH_GUC_MAX_QUERY_TEXT_LEN = 1024;
i32 PWH_GUC_SIGNAL_TIMEOUT_MS = 32;
i32 PWH_GUC_SAMPLE_INTERVAL_MS = 250;
#ifdef WITH_BGWORKER
i32 PWH_GUC_METRICS_MAX_RESPONSE_BYTES = 64 * 1024 * 1024;
#endif

double PWH_GUC_MIN_COST_TO_TRACK = 50000;

#ifdef WITH_BGWORKER
static bool check_listen_address(char **newval, void **extra, GucSource source);
#endif

void
pwh_define_gucs(void)
{
	DefineCustomBoolVariable(
		PWH_GUC_IS_ENABLED_NAME, "Enable pg_what_is_happening extension", NULL,
		&PWH_GUC_IS_ENABLED, true, PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(PWH_GUC_METRICS_EXPOSE_QUERY_TEXT_NAME,
							 "Expose query text through the metrics endpoint",
							 NULL, &PWH_GUC_METRICS_EXPOSE_QUERY_TEXT, false,
							 PGC_SIGHUP, 0, NULL, NULL, NULL);

#ifdef WITH_BGWORKER
	DefineCustomStringVariable(
		PWH_GUC_METRICS_LISTEN_ADDRESS_NAME,
		"Listen address for metrics endpoint (host:port)", NULL,
		&PWH_GUC_METRICS_LISTEN_ADDRESS, "127.0.0.1:9187", PGC_POSTMASTER, 0,
		check_listen_address, NULL, NULL);
#endif

	DefineCustomIntVariable(PWH_GUC_MAX_TRACKED_QUERIES_NAME,
							"Maximum number of concurrent queries to track",
							NULL, &PWH_GUC_MAX_TRACKED_QUERIES, 32, 2, 256,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable(PWH_GUC_MAX_NODES_PER_QUERY_NAME,
							"Maximum plan nodes tracked per query", NULL,
							&PWH_GUC_MAX_NODES_PER_QUERY, 128, 16, 256,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable(PWH_GUC_MAX_QUERY_TEXT_LEN_NAME,
							"Maximum query text length to store", NULL,
							&PWH_GUC_MAX_QUERY_TEXT_LEN, 1024, 64, 8192,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);

	DefineCustomIntVariable(PWH_GUC_SIGNAL_TIMEOUT_MS_NAME,
							"Timeout waiting for signal handler response", NULL,
							&PWH_GUC_SIGNAL_TIMEOUT_MS, 32, 1, 10000,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomIntVariable(PWH_GUC_SAMPLE_INTERVAL_MS_NAME,
							"Minimum interval between metric samples", NULL,
							&PWH_GUC_SAMPLE_INTERVAL_MS, 250, 100, 60000,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

#ifdef WITH_BGWORKER
	DefineCustomIntVariable(PWH_GUC_METRICS_MAX_RESPONSE_BYTES_NAME,
							"Maximum metrics response size in bytes", NULL,
							&PWH_GUC_METRICS_MAX_RESPONSE_BYTES,
							64 * 1024 * 1024, 1024, 1024 * 1024 * 1024,
							PGC_SIGHUP, 0, NULL, NULL, NULL);
#endif

	DefineCustomRealVariable(PWH_GUC_MIN_COST_TO_TRACK_NAME,
							 "Minimum total cost of a query to track", NULL,
							 &PWH_GUC_MIN_COST_TO_TRACK, 50000, 0, DBL_MAX,
							 PGC_SIGHUP, 0, NULL, NULL, NULL);
}

#ifdef WITH_BGWORKER
static bool
check_listen_address(char **newval, void **extra, GucSource source)
{
	char *value = *newval;

	if (value == NULL || *value == '\0')
	{
		GUC_check_errdetail("Listen address cannot be empty");
		return false;
	}

	char *host_start = value;
	char *colon;
	char *host_end;
	if (value[0] == '[')
	{
		host_start = value + 1;
		host_end = strchr(host_start, ']');
		if (host_end == NULL || host_end[1] != ':')
		{
			GUC_check_errdetail(
				"IPv6 listen addresses must use format [address]:port");
			return false;
		}
		colon = host_end + 1;
	}
	else
	{
		colon = strrchr(value, ':');
		host_end = colon;
		if (colon != NULL && strchr(value, ':') != colon)
		{
			GUC_check_errdetail(
				"IPv6 listen addresses must use format [address]:port");
			return false;
		}
	}
	if (colon == NULL)
	{
		GUC_check_errdetail("Listen address must be in format host:port");
		return false;
	}

	u64 host_len = host_end - host_start;
	if (host_len == 0)
	{
		GUC_check_errdetail("Host part cannot be empty");
		return false;
	}

	if (host_len > 255)
	{
		GUC_check_errdetail("Host part too long (max 255 characters)");
		return false;
	}

	char *endptr;
	long  port = strtol(colon + 1, &endptr, 10);

	if (*endptr != '\0' || endptr == colon + 1)
	{
		GUC_check_errdetail("Port must be a numeric value");
		return false;
	}

	if (port < 1 || port > 65535)
	{
		GUC_check_errdetail("Port must be between 1 and 65535");
		return false;
	}

	return true;
}
#endif
