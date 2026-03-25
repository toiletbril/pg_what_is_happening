/*
 * This file is part of pg_what_is_happening.
 * Copyright (C) 2025 toilebril
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 * See top-level LICENSE file.
 */

#include "postgres.h"

#include "http_server.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"

/*
 * Convenience functions using default implementation
 */

HttpServer *
pwh_http_server_create(const char *listen_addr)
{
	const HttpServerVtable *vtable = pwh_http_server_get_mongoose_impl();
	return vtable->createFn(listen_addr);
}

void
pwh_http_server_destroy(HttpServer *server)
{
	if (unlikely(!server || !server->vtable))
		return;
	server->vtable->destroyFn(server);
}

void
pwh_http_server_set_handler(HttpServer *server, HttpRequestHandler handler,
							void *user_data)
{
	if (unlikely(!server || !server->vtable))
		return;
	server->vtable->setHandlerFn(server, handler, user_data);
}

i32
pwh_http_server_run(HttpServer *server)
{
	if (unlikely(!server || !server->vtable))
		return -1;
	return server->vtable->runFn(server);
}

void
pwh_http_server_stop(HttpServer *server)
{
	if (unlikely(!server || !server->vtable))
		return;
	server->vtable->stopFn(server);
}

/*
 * Helper to send simple text response
 */
void
pwh_http_response_text(HttpResponse *resp, i32 status_code, const char *body)
{
	const char *status_text;

	switch (status_code)
	{
		case 200:
			status_text = "OK";
			break;
		case 404:
			status_text = "Not Found";
			break;
		case 500:
			status_text = "Internal Server Error";
			break;
		default:
			status_text = "Unknown";
			break;
	}

	resp->status_code = status_code;
	resp->status_text = status_text;
	resp->body = body;
	resp->body_len = body ? strlen(body) : 0;
	resp->headers = NULL;
}
