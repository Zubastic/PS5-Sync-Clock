/*
 * PS5 Sync Clock - tiny HTTP server.
 */

#include "websrv.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "log.h"

static volatile int g_shutdown_requested = 0;

void http_request_shutdown(void) {
  g_shutdown_requested = 1;
}

static const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

static int write_all(int fd, const char* data, size_t size) {
  size_t off = 0;
  while (off < size) {
    const ssize_t n = send(fd, data + off, size - off, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    off += static_cast<size_t>(n);
  }
  return 0;
}

int http_send(int fd, const char* status, const char* mime, const void* data,
              size_t size) {
  char header[512];
  const int len = snprintf(header, sizeof(header),
                           "HTTP/1.1 %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           status, mime, size);
  if (len < 0 || static_cast<size_t>(len) >= sizeof(header)) {
    return -1;
  }
  if (write_all(fd, header, static_cast<size_t>(len)) != 0) {
    return -1;
  }
  if (size > 0 && write_all(fd, static_cast<const char*>(data), size) != 0) {
    return -1;
  }
  return 0;
}

int http_send_json(int fd, const char* json) {
  return http_send(fd, status_text(200), "application/json", json,
                   strlen(json));
}

void http_send_error_json(int fd, const char* message) {
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"detail\":\"%s\"}",
           message);
  http_send_json(fd, buf);
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

int http_get_query_arg(const http_request_t* req, const char* name, char* out,
                       size_t out_size) {
  out[0] = '\0';
  const size_t name_len = strlen(name);
  const char* p = req->query;

  while (*p) {
    const char* amp = strchr(p, '&');
    const size_t pair_len = amp ? static_cast<size_t>(amp - p) : strlen(p);
    if (pair_len > name_len && strncmp(p, name, name_len) == 0 &&
        p[name_len] == '=') {
      /* URL-decode the value into out */
      const char* src = p + name_len + 1;
      const char* end = p + pair_len;
      size_t o = 0;
      while (src < end && o + 1 < out_size) {
        if (*src == '%' && src + 2 < end && hex_value(src[1]) >= 0 &&
            hex_value(src[2]) >= 0) {
          out[o++] = static_cast<char>(hex_value(src[1]) * 16 +
                                       hex_value(src[2]));
          src += 3;
        } else if (*src == '+') {
          out[o++] = ' ';
          ++src;
        } else {
          out[o++] = *src++;
        }
      }
      out[o] = '\0';
      return 1;
    }
    if (!amp) {
      break;
    }
    p = amp + 1;
  }
  return 0;
}

/*
 * Read the whole request: headers + Content-Length body bytes. A single
 * read() may return only headers (the body often arrives in a separate
 * TCP segment), so keep reading until the request is complete.
 */
static size_t read_request(int fd, char* request, size_t request_size) {
  /* accepted sockets can behave as non-blocking; clear it so reads block */
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags != -1) {
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  }

  size_t total = 0;
  while (total < request_size - 1) {
    const ssize_t received =
        read(fd, request + total, request_size - 1 - total);
    if (received <= 0) {
      if (total > 0) {
        break; /* timed out / closed after partial data - use what we have */
      }
      return 0;
    }
    total += static_cast<size_t>(received);
    request[total] = '\0';

    char* sep = strstr(request, "\r\n\r\n");
    if (!sep) {
      continue; /* headers not complete yet */
    }
    const size_t header_len = static_cast<size_t>(sep - request) + 4;

    /* find Content-Length case-insensitively in the header block */
    long content_length = -1;
    {
      char headers[1024];
      const size_t hcopy =
          header_len < sizeof(headers) ? header_len : sizeof(headers) - 1;
      for (size_t i = 0; i < hcopy; ++i) {
        headers[i] = static_cast<char>(request[i] >= 'A' && request[i] <= 'Z'
                                           ? request[i] - 'A' + 'a'
                                           : request[i]);
      }
      headers[hcopy] = '\0';
      const char* cl = strstr(headers, "content-length:");
      if (cl) {
        content_length = strtol(cl + sizeof("content-length:") - 1, nullptr, 10);
      }
    }

    if (content_length < 0 ||
        total >= header_len + static_cast<size_t>(content_length)) {
      break; /* complete (or no body expected) */
    }

    /* body still missing: wait for it before the next read */
    struct pollfd pfd = {fd, POLLIN, 0};
    const int pr = poll(&pfd, 1, 2000);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
      log_message("http: body wait timed out (got %zu of %ld)\n",
                  total - header_len, content_length);
      break;
    }
  }
  return total;
}

static void parse_request(char* request, http_request_t* req) {
  req->method[0] = '\0';
  req->path[0] = '\0';
  req->query[0] = '\0';

  char* method = request;
  char* space = strchr(request, ' ');
  if (!space) {
    return;
  }
  *space = '\0';
  snprintf(req->method, sizeof(req->method), "%s", method);

  char* target = space + 1;
  char* target_end = strchr(target, ' ');
  if (target_end) {
    *target_end = '\0';
  }

  char* qmark = strchr(target, '?');
  if (qmark) {
    *qmark = '\0';
    snprintf(req->query, sizeof(req->query), "%s", qmark + 1);
  }
  snprintf(req->path, sizeof(req->path), "%s", target);
}

void handle_connection(int fd, void (*handler)(const http_request_t* req));

int http_serve(unsigned short port,
               void (*handler)(const http_request_t* req)) {
  const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    log_message("socket failed: errno=%d\n", errno);
    return 1;
  }

  const int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    log_message("bind port %d failed: errno=%d\n", port, errno);
    close(listen_fd);
    return 1;
  }
  if (listen(listen_fd, 4) != 0) {
    log_message("listen failed: errno=%d\n", errno);
    close(listen_fd);
    return 1;
  }

  log_message("web UI listening on port %d\n", port);
  while (!g_shutdown_requested) {
    struct sockaddr_in client = {};
    socklen_t client_len = sizeof(client);
    const int conn_fd = accept(
        listen_fd, reinterpret_cast<struct sockaddr*>(&client), &client_len);
    if (conn_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_message("accept failed: errno=%d\n", errno);
      sleep(1);
      continue;
    }

    struct timeval timeout = {};
    timeout.tv_sec = 5;
    setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char request[4096] = {};
    if (read_request(conn_fd, request, sizeof(request)) > 0) {
      http_request_t req = {};
      req.fd = conn_fd;
      parse_request(request, &req);
      log_message("HTTP %s %s%s%s\n", req.method, req.path,
                  req.query[0] ? "?" : "", req.query);
      handler(&req);
    }
    close(conn_fd);
  }

  close(listen_fd);
  log_message("web UI stopped\n");
  return 0;
}
