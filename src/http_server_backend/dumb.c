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
#include <netdb.h>
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
	i32					 port;
	char				 host[256];
	i32					 listen_fd;
	volatile bool		 is_running;
	HttpRequestHandlerFn handler_fn;
	void				*custom_context;
} DumbHttpServer;

static HttpServer *dumb_create(const char *listen_addr);
static void		   dumb_destroy(HttpServer *server);

static void dumb_set_handler(HttpServer *server, HttpRequestHandlerFn handler,
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
	const char	   *port_text;
	char		   *port_end;
	long			parsed_port;
	i32				port;
	u64				host_len;

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
	{
		if (strchr(listen_addr, ':') != colon)
			goto invalid_address;
		port_text = colon + 1;
		errno = 0;
		parsed_port = strtol(port_text, &port_end, 10);
		if (errno != 0 || port_end == port_text || *port_end != '\0' ||
			parsed_port < 1 || parsed_port > 65535)
			goto invalid_address;
		port = (i32) parsed_port;
		host_len = colon - listen_addr;
	}
	else
	{
		port = 9187; /* Default port. */
		host_len = strlen(listen_addr);
	}

	impl->port = port;
	if (host_len >= sizeof(impl->host))
		goto invalid_address;
	if (host_len == 0)
		snprintf(impl->host, sizeof(impl->host), "0.0.0.0");
	else
	{
		memcpy(impl->host, listen_addr, host_len);
		impl->host[host_len] = '\0';
	}
	impl->listen_fd = -1;
	impl->is_running = false;
	impl->handler_fn = NULL;
	impl->custom_context = NULL;

	server->vtable = &dumb_vtable;
	server->impl = impl;

	return server;

invalid_address:
	free(impl);
	free(server);
	return NULL;
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
dumb_set_handler(HttpServer *server, HttpRequestHandlerFn handler,
				 void *user_data)
{
	DumbHttpServer *impl = (DumbHttpServer *) server->impl;

	impl->handler_fn = handler;
	impl->custom_context = user_data;
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
	if (req->method == NULL)
		return false;
	memcpy(req->method, buffer, space1 - buffer);
	req->method[space1 - buffer] = '\0';

	/* Allocate and copy path. */
	req->path = (char *) malloc(space2 - space1);
	if (req->path == NULL)
	{
		free(req->method);
		return false;
	}
	memcpy(req->path, space1 + 1, space2 - space1 - 1);
	req->path[space2 - space1 - 1] = '\0';

	/* Allocate and copy version. */
	req->version = (char *) malloc(line_end - space2);
	if (req->version == NULL)
	{
		free(req->path);
		free(req->method);
		return false;
	}
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
send_all(i32 fd, const char *data, u64 length)
{
	while (length > 0)
	{
		ssize_t sent = send(fd, data, length, 0);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			return;
		data += sent;
		length -= sent;
	}
}

static void
handle_connection(DumbHttpServer *impl, i32 client_fd)
{
	char		   buffer[DUMB_HTTP_BUFFER_SIZE];
	ssize_t		   bytes_read;
	HttpRequest	   req;
	HttpResponse   resp;
	char		   response_buffer[1024];
	i32			   response_len;
	struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
	u64			   offset = 0;

	setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	while (offset < sizeof(buffer) - 1)
	{
		bytes_read =
			recv(client_fd, buffer + offset, sizeof(buffer) - 1 - offset, 0);
		if (bytes_read < 0 && errno == EINTR)
			continue;
		if (bytes_read <= 0)
			break;
		offset += bytes_read;
		buffer[offset] = '\0';
		if (strstr(buffer, "\r\n\r\n") != NULL ||
			strstr(buffer, "\n\n") != NULL)
			break;
	}
	if (offset == 0)
	{
		close(client_fd);
		return;
	}
	buffer[offset] = '\0';

	/* Parse request. */
	if (!parse_request(buffer, &req))
	{
		close(client_fd);
		return;
	}

	/* Initialize response. */
	memset(&resp, 0, sizeof(resp));

	/* Call handler. */
	if (impl->handler_fn)
	{
		impl->handler_fn(&req, &resp, impl->custom_context);
	}
	else
	{
		pwh_http_response_set_text(&resp, 404, "Not Found");
	}

	/* Build response. */
	response_len =
		snprintf(response_buffer, sizeof(response_buffer),
				 "HTTP/1.1 %d %s\r\n"
				 "%s"
				 "Content-Length: %llu\r\n"
				 "Connection: close\r\n"
				 "\r\n",
				 resp.status_code, resp.status_text,
				 resp.headers ? resp.headers
							  : "Content-Type: text/plain; charset=utf-8\r\n",
				 (unsigned long long) resp.body_len);

	if (response_len > 0 && response_len < (i32) sizeof(response_buffer))
	{
		send_all(client_fd, response_buffer, response_len);
		if (resp.body != NULL)
			send_all(client_fd, resp.body, resp.body_len);
	}

	/* Cleanup. */
	pwh_http_response_destroy_body(&resp);
	free_request(&req);
	close(client_fd);
}

static i32
dumb_run(HttpServer *server)
{
	DumbHttpServer	*impl = (DumbHttpServer *) server->impl;
	struct addrinfo	 hints;
	struct addrinfo *addresses = NULL;
	char			 port[16];
	i32				 client_fd;
	i32				 opt = 1;

	/* Create socket. */
	impl->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (impl->listen_fd < 0)
		return -1;

	/* Set socket options. */
	setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port, sizeof(port), "%d", impl->port);
	if (getaddrinfo(impl->host, port, &hints, &addresses) != 0 ||
		addresses == NULL ||
		bind(impl->listen_fd, addresses->ai_addr, addresses->ai_addrlen) < 0)
	{
		if (addresses != NULL)
			freeaddrinfo(addresses);
		close(impl->listen_fd);
		impl->listen_fd = -1;
		return -1;
	}
	freeaddrinfo(addresses);

	/* Listen. */
	if (listen(impl->listen_fd, 5) < 0)
	{
		close(impl->listen_fd);
		impl->listen_fd = -1;
		return -1;
	}

	impl->is_running = true;

	/* Accept loop. */
	while (impl->is_running)
	{
		client_fd = accept(impl->listen_fd, NULL, NULL);
		if (client_fd < 0)
		{
			if (impl->is_running)
				continue;

			break;
		}

		handle_connection(impl, client_fd);
	}

	close(impl->listen_fd);

	impl->is_running = false;
	impl->listen_fd = -1;

	return 0;
}

static void
dumb_stop(HttpServer *server)
{
	DumbHttpServer *impl = (DumbHttpServer *) server->impl;

	impl->is_running = false;

	if (impl->listen_fd >= 0)
	{
		shutdown(impl->listen_fd, SHUT_RDWR);
		close(impl->listen_fd);
		impl->listen_fd = -1;
	}
}
