/*
 * PS5 Sync Clock - HTTP API routing and handlers.
 */

#pragma once

#include "websrv.h"

/*
 * Dispatch one parsed HTTP request. Called by the server loop for every
 * accepted connection.
 */
void api_handle_request(const http_request_t* req);
