/*
 * PS5 Sync Clock - HTTP API routing and handlers.
 *
 * Endpoints:
 *   GET  /                - embedded web UI
 *   GET  /api/time        - JSON with the current value of every clockid
 *   GET  /api/set?ts=N    - set the console time to unix seconds N
 *                           (time travels as a URL parameter; the request
 *                           body is never parsed, which keeps this working
 *                           regardless of socket read quirks)
 *   POST /api/ntp         - query NTP servers from the console and apply
 *   POST /api/quit        - stop the payload
 */

#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asset.h"
#include "clock.h"
#include "log.h"
#include "sntp.h"
#include "websrv.h"

#ifndef SYNC_CLOCK_VERSION
#define SYNC_CLOCK_VERSION "dev"
#endif

namespace {

void handle_api_time(const http_request_t* req) {
  char json[4096];
  clock_build_state_json(json, sizeof(json));
  http_send_json(req->fd, json);
}

void handle_api_set(const http_request_t* req) {
  char value[64] = {};
  if (!http_get_query_arg(req, "ts", value, sizeof(value)) || !value[0]) {
    http_send_error_json(req->fd, "missing ts parameter");
    return;
  }

  char* end = nullptr;
  const long long seconds = strtoll(value, &end, 10);
  const char* error = nullptr;
  if (!clock_set_from_unix_seconds(seconds, &error)) {
    char detail[128];
    snprintf(detail, sizeof(detail), "%s (%s)", error ? error : "invalid",
             value);
    http_send_error_json(req->fd, detail);
    return;
  }

  char detail[128];
  snprintf(detail, sizeof(detail), "set to %lld (domains 17/16 + realtime)",
           seconds);
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"detail\":\"%s\"}", detail);
  http_send_json(req->fd, buf);
  notify_message("Sync Clock: time set to %lld", seconds);
}

void handle_api_ntp(const http_request_t* req) {
  struct timespec ts = {};
  if (!sntp_fetch_time(&ts)) {
    http_send_error_json(req->fd, "no valid NTP response");
    return;
  }

  if (!clock_apply_time(&ts)) {
    http_send_error_json(req->fd, "privilege escalation or clock write failed");
    return;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"status\":\"ok\",\"detail\":\"NTP %lld applied (domains 17/16 "
           "+ realtime)\"}",
           static_cast<long long>(ts.tv_sec));
  http_send_json(req->fd, buf);
  notify_message("Sync Clock: NTP time %lld applied",
                 static_cast<long long>(ts.tv_sec));
}

void handle_api_quit(const http_request_t* req) {
  http_send_json(req->fd, "{\"status\":\"ok\",\"detail\":\"payload stopping\"}");
  notify_message("Sync Clock: payload stopped");
  http_request_shutdown();
}

}  // namespace

void api_handle_request(const http_request_t* req) {
  if (strcmp(req->method, "GET") == 0 &&
      (strcmp(req->path, "/") == 0 || strcmp(req->path, "/index.html") == 0)) {
    asset_serve(req->fd, "/");
    return;
  }
  if (strcmp(req->path, "/api/time") == 0) {
    handle_api_time(req);
    return;
  }
  if (strncmp(req->path, "/api/set", 8) == 0) {
    handle_api_set(req);
    return;
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/ntp") == 0) {
    handle_api_ntp(req);
    return;
  }
  if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/quit") == 0) {
    handle_api_quit(req);
    return;
  }
  http_send(req->fd, "404 Not Found", "text/plain", "not found\n", 9);
}
