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

#include "../../vendor/mongoose.h"

#include <stdio.h>
#include <string.h>

#include "../common.h"
#include "../http_server.h"

/* Mongoose HTTP server implementation data. */
typedef struct MongooseHttpServer
{
	struct mg_mgr		  mgr;
	struct mg_connection *conn;
	char				  listen_addr[128];
	volatile bool		  running;
	HttpRequestHandler	  handler;
	void				 *user_data;
} MongooseHttpServer;

/* Forward declarations. */
static HttpServer *mongoose_create(const char *listen_addr);
static void		   mongoose_destroy(HttpServer *server);
static void mongoose_set_handler(HttpServer *server, HttpRequestHandler handler,
								 void *user_data);
static i32	mongoose_run(HttpServer *server);
static void mongoose_stop(HttpServer *server);

/* Vtable. */
static const HttpServerVtable mongoose_vtable = {
	.createFn = mongoose_create,
	.destroyFn = mongoose_destroy,
	.setHandlerFn = mongoose_set_handler,
	.runFn = mongoose_run,
	.stopFn = mongoose_stop,
};

/*
 * Get mongoose implementation vtable
 */
const HttpServerVtable *
pwh_http_server_get_mongoose_impl(void)
{
	return &mongoose_vtable;
}

/*
 * Mongoose event handler callback
 */
static void
mongoose_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
	MongooseHttpServer *impl = (MongooseHttpServer *) c->fn_data;

	if (ev == MG_EV_HTTP_MSG)
	{
		struct mg_http_message *hm = (struct mg_http_message *) ev_data;
		HttpRequest				req;
		HttpResponse			resp;
		char					method[16];
		char					path[256];

		/* Extract method and path. */
		snprintf(method, sizeof(method), "%.*s", (int) hm->method.len,
				 hm->method.buf);
		snprintf(path, sizeof(path), "%.*s", (int) hm->uri.len, hm->uri.buf);

		/* Build HttpRequest. */
		req.method = method;
		req.path = path;
		req.version = "HTTP/1.1";
		req.headers = NULL;
		req.body = NULL;
		req.body_len = 0;

		/* Initialize response. */
		memset(&resp, 0, sizeof(resp));

		/* Call handler. */
		if (impl->handler)
			impl->handler(&req, &resp, impl->user_data);
		else
			pwh_http_response_text(&resp, 404, "Not Found");

		/* Send response. */
		mg_http_reply(
			c, (int) resp.status_code,
			resp.headers ? resp.headers : "Content-Type: text/plain\r\n", "%s",
			resp.body ? resp.body : "");

		pwh_http_response_free_contents(&resp);
	}
}

/*
 * Create mongoose HTTP server
 */
static HttpServer *
mongoose_create(const char *listen_addr)
{
	HttpServer		   *server;
	MongooseHttpServer *impl;

	server = (HttpServer *) malloc(sizeof(HttpServer));
	if (server == NULL)
		return NULL;

	impl = (MongooseHttpServer *) malloc(sizeof(MongooseHttpServer));
	if (impl == NULL)
	{
		free(server);
		return NULL;
	}

	/* Initialize mongoose manager. */
	mg_mgr_init(&impl->mgr);

	/* Format listen address - if it doesn't start with http://, prepend it. */
	if (strncmp(listen_addr, "http://", 7) == 0)
		snprintf(impl->listen_addr, sizeof(impl->listen_addr), "%s",
				 listen_addr);
	else
		snprintf(impl->listen_addr, sizeof(impl->listen_addr), "http://%s",
				 listen_addr);

	impl->conn = NULL;
	impl->running = false;
	impl->handler = NULL;
	impl->user_data = NULL;

	server->vtable = &mongoose_vtable;
	server->impl = impl;

	return server;
}

/*
 * Destroy mongoose HTTP server
 */
static void
mongoose_destroy(HttpServer *server)
{
	MongooseHttpServer *impl;

	if (server == NULL)
		return;

	impl = (MongooseHttpServer *) server->impl;
	if (impl)
	{
		mg_mgr_free(&impl->mgr);
		free(impl);
	}
	free(server);
}

/*
 * Set request handler
 */
static void
mongoose_set_handler(HttpServer *server, HttpRequestHandler handler,
					 void *user_data)
{
	MongooseHttpServer *impl = (MongooseHttpServer *) server->impl;

	impl->handler = handler;
	impl->user_data = user_data;
}

/*
 * Run mongoose HTTP server (blocking)
 */
static i32
mongoose_run(HttpServer *server)
{
	MongooseHttpServer *impl = (MongooseHttpServer *) server->impl;

	/* Start listening. */
	impl->conn = mg_http_listen(&impl->mgr, impl->listen_addr,
								mongoose_event_handler, impl);
	if (!impl->conn)
		return -1;

	impl->running = true;

	/* Event loop. */
	while (impl->running)
	{
		mg_mgr_poll(&impl->mgr, 1000); /* Poll with 1 second timeout. */
	}

	return 0;
}

/*
 * Stop mongoose HTTP server
 */
static void
mongoose_stop(HttpServer *server)
{
	MongooseHttpServer *impl = (MongooseHttpServer *) server->impl;

	impl->running = false;
}
