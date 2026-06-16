#ifndef NGX_HTTP_RATELIMIT_SCRIPT_H
#define NGX_HTTP_RATELIMIT_SCRIPT_H

#include <ngx_config.h>
#include <ngx_core.h>

/* Compute and cache the SHA1 of the rate limit Lua script. Must be called
 * once during postconfiguration before any EVALSHA is issued. */
void ngx_http_ratelimit_script_init(void);

/* The Lua script body, sent with EVAL on the NOSCRIPT fallback path. */
ngx_str_t *ngx_http_ratelimit_script_body(void);

/* The 40-char hex SHA1 of the script body, sent with EVALSHA. */
ngx_str_t *ngx_http_ratelimit_script_sha(void);

#endif /* NGX_HTTP_RATELIMIT_SCRIPT_H */
