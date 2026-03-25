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

#ifndef PWH_HTTP_SERVER_H
#define PWH_HTTP_SERVER_H

#include "common.h"

/* HTTP request structure. */
typedef struct
{
	char *method;	/* GET, POST, etc. */
	char *path;		/* /metrics, etc. */
	char *version;	/* HTTP/1.1. */
	char *headers;	/* Raw headers. */
	char *body;		/* Request body. */
	usize body_len; /* Body length. */
} HttpRequest;

/* HTTP response structure. */
typedef struct
{
	i32			status_code; /* 200, 404, etc. */
	const char *status_text; /* OK, Not Found, etc. */
	char	   *headers;	 /* Response headers. */
	const char *body;		 /* Response body. */
	usize		body_len;	 /* Body length. */
	bool		free_body;	 /* Whether to free body after sending. */
} HttpResponse;

typedef struct HttpServer HttpServer;

typedef void (*HttpRequestHandler)(const HttpRequest *req, HttpResponse *resp,
								   void *user_data);

/* Virtual function table for HTTP server backends. */
typedef struct HttpServerVtable
{
	HttpServer *(*createFn)(const char *listen_addr);
	void (*destroyFn)(HttpServer *server);
	void (*setHandlerFn)(HttpServer *server, HttpRequestHandler handler,
						 void *user_data);
	i32 (*runFn)(HttpServer *server);
	void (*stopFn)(HttpServer *server);
} HttpServerVtable;

/* HTTP server instance. */
struct HttpServer
{
	const HttpServerVtable *vtable;
	void				   *impl; /* Backend-specific data. */
};

extern const HttpServerVtable *pwh_http_server_get_dumb_impl(void);
extern const HttpServerVtable *pwh_http_server_get_mongoose_impl(void);

extern HttpServer *pwh_http_server_create(const char *listen_addr);
extern void		   pwh_http_server_destroy(HttpServer *server);
extern void		   pwh_http_server_set_handler(HttpServer		 *server,
											   HttpRequestHandler handler,
											   void				 *user_data);
extern i32		   pwh_http_server_run(HttpServer *server);
extern void		   pwh_http_server_stop(HttpServer *server);

extern void pwh_http_response_text(HttpResponse *resp, i32 status_code,
								   const char *body);

#endif /* PWH_HTTP_SERVER_H. */
