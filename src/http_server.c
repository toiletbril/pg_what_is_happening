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

/* Shut up the compiler */
#ifndef HTTP_BACKEND
#define HTTP_BACKEND "\0"
#endif

static const HttpServerVtable *choose_backend(void);

HttpServer *
pwh_http_server_create(const char *listen_addr)
{
	const HttpServerVtable *vtable = choose_backend();

	if (vtable == NULL)
	{
		elog(WARNING, "No HTTP backend has been compiled in");
		return NULL;
	}

	return vtable->createFn(listen_addr);
}

void
pwh_http_server_destroy(HttpServer *server)
{
	Assert(server != NULL);
	Assert(server->vtable != NULL);
	server->vtable->destroyFn(server);
}

void
pwh_http_server_set_handler(HttpServer *server, HttpRequestHandler handler,
							void *user_data)
{
	Assert(server != NULL);
	Assert(server->vtable != NULL);
	server->vtable->setHandlerFn(server, handler, user_data);
}

i32
pwh_http_server_run(HttpServer *server)
{
	Assert(server != NULL);
	Assert(server->vtable != NULL);
	return server->vtable->runFn(server);
}

void
pwh_http_server_stop(HttpServer *server)
{
	Assert(server != NULL);
	Assert(server->vtable != NULL);
	server->vtable->stopFn(server);
}

void
pwh_http_response_text(HttpResponse *resp, u32 status_code, char *body)
{
	resp->status_code = status_code;

	switch (status_code)
	{
		case 200:
			resp->status_text = "OK";
			break;
		case 404:
			resp->status_text = "Not Found";
			break;
		case 500:
			resp->status_text = "Internal Server Error";
			break;
		default:
			resp->status_text = "Unknown";
			break;
	}

	resp->body = pstrdup(body);
	resp->body_len = body ? strlen(body) : 0;
	resp->headers = NULL;
}

void
pwh_http_response_free_contents(HttpResponse *resp)
{
	pfree(resp->body);
}

static const HttpServerVtable *
choose_backend(void)
{
	const char *b = HTTP_BACKEND;

	if (streq(b, "mongoose"))
	{
		return pwh_http_server_get_mongoose_impl();
	}
	if (streq(b, "dumb"))
	{
		return pwh_http_server_get_dumb_impl();
	}

	return NULL;
}
