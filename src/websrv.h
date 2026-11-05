/*
 * PS5 Sync Clock - tiny HTTP server.
 *
 * A minimal HTTP/1.1 server used to serve the embedded web UI and the
 * JSON endpoints. Every response is sent with "Connection: close"
 * (one request per TCP connection).
 *
 * Notes learned on live consoles:
 *  - a single read() may return only the request headers while the body
 *    arrives in a later TCP segment, so the read loop continues until
 *    Content-Length bytes have been received;
 *  - accepted sockets can behave as non-blocking, so O_NONBLOCK is
 *    cleared explicitly and poll() guards every wait for more data.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * A parsed HTTP request. Populated by the server loop before the
 * request is dispatched to a handler.
 *
 *   fd     - client socket; handlers write responses here
 *   method - "GET" / "POST" (upper-case, NUL-terminated)
 *   path   - request-target path without the query string
 *   query  - raw query string without the leading '?', NUL-terminated
 */
typedef struct http_request {
  int fd;
  char method[8];
  char path[1024];
  char query[2048];
} http_request_t;

/*
 * Send a complete HTTP response with default headers
 * (Cache-Control: no-store, Connection: close). `status` is the full
 * reason string including the numeric code, e.g. "200 OK" or
 * "404 Not Found" - the wire format needs the code on the status line.
 * Returns 0 on success, -1 on write error.
 */
int http_send(int fd, const char* status, const char* mime, const void* data,
              size_t size);

/*
 * Send a JSON 200 response.
 */
int http_send_json(int fd, const char* json);

/*
 * Send {"status":"error","detail":"<message>"} as JSON.
 */
void http_send_error_json(int fd, const char* message);

/*
 * Look up a single query-parameter value by name in req->query,
 * URL-decoding it into `out`. Returns 1 if present, 0 if absent.
 */
int http_get_query_arg(const http_request_t* req, const char* name, char* out,
                       size_t out_size);

/*
 * Bind a listening socket on `port` and run the single-threaded
 * accept/handle loop, dispatching every request to `handler`.
 * The loop runs until http_request_shutdown() is called from inside a
 * handler, or websrv_request_restart() asks for a rest-mode recreate.
 * Returns 0 on clean shutdown, 1 if the socket failed, 2 when a
 * restart was requested (caller must call http_serve again).
 */
int http_serve(unsigned short port,
               void (*handler)(const http_request_t* req));

/*
 * Ask the accept loop to stop after the current request completes
 * (used by the "stop payload" endpoint).
 */
void http_request_shutdown(void);

/*
 * Ask the accept loop to stop so main() can recreate the whole server
 * (called by the rest-mode watcher after wake).
 */
void websrv_request_restart(void);
