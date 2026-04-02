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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common.h"
#include "../http_server.h"

#define DUMB_HTTP_BUFFER_SIZE 8192

typedef struct DumbHttpServer
{
	i32				   port;
	i32				   listen_fd;
	volatile bool	   running;
	HttpRequestHandler handler;
	void			  *user_data;
} DumbHttpServer;

static HttpServer *dumb_create(const char *listen_addr);
static void		   dumb_destroy(HttpServer *server);

static void dumb_set_handler(HttpServer *server, HttpRequestHandler handler,
							 void *user_data);
static i32	dumb_run(HttpServer *server);
static void dumb_stop(HttpServer *server);

static const HttpServerVtable dumb_vtable = {
	.createFn = dumb_create,
	.destroyFn = dumb_destroy,
	.setHandlerFn = dumb_set_handler,
	.runFn = dumb_run,
	.stopFn = dumb_stop,
};

donteliminate const HttpServerVtable *
pwh_http_server_get_impl(void)
{
	return &dumb_vtable;
}

static HttpServer *
dumb_create(const char *listen_addr)
{
	HttpServer	   *server;
	DumbHttpServer *impl;
	const char	   *colon;
	i32				port;

	server = (HttpServer *) malloc(sizeof(HttpServer));
	if (server == NULL)
		return NULL;

	impl = (DumbHttpServer *) malloc(sizeof(DumbHttpServer));
	if (impl == NULL)
	{
		free(server);
		return NULL;
	}

	/* Parse port from address string (format: "host:port" or ":port"). */
	colon = strrchr(listen_addr, ':');
	if (colon)
		port = atoi(colon + 1);
	else
		port = 9187; /* Default port. */

	impl->port = port;
	impl->listen_fd = -1;
	impl->running = false;
	impl->handler = NULL;
	impl->user_data = NULL;

	server->vtable = &dumb_vtable;
	server->impl = impl;

	return server;
}

static void
dumb_destroy(HttpServer *server)
{
	DumbHttpServer *impl;

	if (server == NULL)
		return;

	impl = (DumbHttpServer *) server->impl;
	if (impl)
	{
		if (impl->listen_fd >= 0)
			close(impl->listen_fd);
		free(impl);
	}
	free(server);
}

static void
dumb_set_handler(HttpServer *server, HttpRequestHandler handler,
				 void *user_data)
{
	DumbHttpServer *impl = (DumbHttpServer *) server->impl;

	impl->handler = handler;
	impl->user_data = user_data;
}

static bool
parse_request(const char *buffer, HttpRequest *req)
{
	char *line_end;
	char *space1, *space2;
	u64	  line_len;

	/* Parse request line: "GET /path HTTP/1.1". */
	line_end = strstr(buffer, "\r\n");
	if (line_end == NULL)
		line_end = strstr(buffer, "\n");
	if (line_end == NULL)
		return false;

	line_len = line_end - buffer;
	space1 = memchr(buffer, ' ', line_len);
	if (space1 == NULL)
		return false;

	space2 = memchr(space1 + 1, ' ', line_len - (space1 - buffer + 1));
	if (space2 == NULL)
		return false;

	/* Allocate and copy method. */
	req->method = (char *) malloc(space1 - buffer + 1);
	memcpy(req->method, buffer, space1 - buffer);
	req->method[space1 - buffer] = '\0';

	/* Allocate and copy path. */
	req->path = (char *) malloc(space2 - space1);
	memcpy(req->path, space1 + 1, space2 - space1 - 1);
	req->path[space2 - space1 - 1] = '\0';

	/* Allocate and copy version. */
	req->version = (char *) malloc(line_end - space2);
	memcpy(req->version, space2 + 1, line_end - space2 - 1);
	req->version[line_end - space2 - 1] = '\0';

	req->headers = NULL;
	req->body = NULL;
	req->body_len = 0;

	return true;
}

static void
free_request(HttpRequest *req)
{
	if (req->method)
		free(req->method);
	if (req->path)
		free(req->path);
	if (req->version)
		free(req->version);
	if (req->headers)
		free(req->headers);
	if (req->body)
		free(req->body);
}

static void
handle_connection(DumbHttpServer *impl, i32 client_fd)
{
	char		 buffer[DUMB_HTTP_BUFFER_SIZE];
	ssize_t		 bytes_read;
	HttpRequest	 req;
	HttpResponse resp;
	char		 response_buffer[DUMB_HTTP_BUFFER_SIZE];
	i32			 response_len;

	/* Read request. */
	bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
	if (bytes_read <= 0)
	{
		close(client_fd);
		return;
	}

	buffer[bytes_read] = '\0';

	/* Parse request. */
	if (!parse_request(buffer, &req))
	{
		close(client_fd);
		return;
	}

	/* Initialize response. */
	memset(&resp, 0, sizeof(resp));

	/* Call handler. */
	if (impl->handler)
	{
		impl->handler(&req, &resp, impl->user_data);
	}
	else
	{
		pwh_http_response_set_text(&resp, 404, "Not Found");
	}

	/* Build response. */
	response_len = snprintf(response_buffer, sizeof(response_buffer),
							"HTTP/1.1 %d %s\r\n"
							"Content-Type: text/plain; version=0.0.4\r\n"
							"Content-Length: %zu\r\n"
							"Connection: close\r\n"
							"\r\n"
							"%s",
							resp.status_code, resp.status_text, resp.body_len,
							resp.body ? resp.body : "");

	/* Send response. */
	send(client_fd, response_buffer, response_len, 0);

	/* Cleanup. */
	free_request(&req);
	close(client_fd);
}

static i32
dumb_run(HttpServer *server)
{
	DumbHttpServer	  *impl = (DumbHttpServer *) server->impl;
	struct sockaddr_in addr;
	i32				   client_fd;
	i32				   opt = 1;

	/* Create socket. */
	impl->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (impl->listen_fd < 0)
		return -1;

	/* Set socket options. */
	setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	/* Bind. */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(impl->port);

	if (bind(impl->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		close(impl->listen_fd);
		impl->listen_fd = -1;
		return -1;
	}

	/* Listen. */
	if (listen(impl->listen_fd, 5) < 0)
	{
		close(impl->listen_fd);
		impl->listen_fd = -1;
		return -1;
	}

	impl->running = true;

	/* Accept loop. */
	while (impl->running)
	{
		client_fd = accept(impl->listen_fd, NULL, NULL);
		if (client_fd < 0)
		{
			if (impl->running)
				continue;
			else
				break;
		}

		handle_connection(impl, client_fd);
	}

	close(impl->listen_fd);
	impl->listen_fd = -1;

	return 0;
}

static void
dumb_stop(HttpServer *server)
{
	DumbHttpServer *impl = (DumbHttpServer *) server->impl;

	impl->running = false;

	if (impl->listen_fd >= 0)
	{
		shutdown(impl->listen_fd, SHUT_RDWR);
		close(impl->listen_fd);
		impl->listen_fd = -1;
	}
}
