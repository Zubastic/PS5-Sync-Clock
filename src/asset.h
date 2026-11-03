/*
 * PS5 Sync Clock - embedded web assets.
 */

#pragma once

#include <stddef.h>

/*
 * Register an embedded asset under a URL path. Called from generated
 * asset modules at startup.
 */
void asset_register(const char* path, void* data, size_t size,
                    const char* mime);

/*
 * Serve `url` from the registered assets, or a 404 page when the path
 * is unknown. Returns 0 on success, -1 on write error.
 */
int asset_serve(int fd, const char* url);
