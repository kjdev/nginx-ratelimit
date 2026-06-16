#ifndef NGX_HTTP_RATELIMIT_SCRIPT_H
#define NGX_HTTP_RATELIMIT_SCRIPT_H

#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_http_ratelimit_module.h"

/* Compute and cache the SHA1 of every rate limit Lua script. Must be called
 * once during postconfiguration before any EVALSHA is issued. */
void ngx_http_ratelimit_script_init(void);

/* The Lua script body for the algorithm, sent with EVAL on the NOSCRIPT
 * fallback path. */
ngx_str_t *ngx_http_ratelimit_script_body(ngx_http_ratelimit_algo_t algo);

/* The 40-char hex SHA1 of the algorithm's script body, sent with EVALSHA. */
ngx_str_t *ngx_http_ratelimit_script_sha(ngx_http_ratelimit_algo_t algo);

#endif /* NGX_HTTP_RATELIMIT_SCRIPT_H */
