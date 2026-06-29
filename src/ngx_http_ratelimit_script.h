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

/* Compute the 40-char hex SHA1 of body into sha_buf (must be 40 bytes) and
 * point sha at it. Shared by the built-in table and the custom zone path. */
void ngx_http_ratelimit_script_sha1(ngx_str_t *body, u_char *sha_buf,
    ngx_str_t *sha);

/* Read a custom (algo=custom) Lua script body from path into out, allocating
 * from cf->pool. Resolves path relative to the nginx prefix, rejects a missing,
 * empty, or oversized (> NGX_HTTP_RATELIMIT_MAX_SCRIPT_LEN) file with a
 * config-line-numbered error. Returns NGX_OK or NGX_ERROR. */
ngx_int_t ngx_http_ratelimit_script_read_file(ngx_conf_t *cf, ngx_str_t *path,
    ngx_str_t *out);

#endif /* NGX_HTTP_RATELIMIT_SCRIPT_H */
