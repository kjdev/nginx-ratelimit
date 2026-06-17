#ifndef NGX_HTTP_RATELIMIT_UPSTREAM_H
#define NGX_HTTP_RATELIMIT_UPSTREAM_H

#include <ngx_http.h>

void ngx_http_ratelimit_read_header_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u);

/* Reload the script with EVAL on the current connection and resume reading
 * its reply, after the server reported NOSCRIPT for EVALSHA. */
ngx_int_t ngx_http_ratelimit_resend_eval(ngx_http_request_t *r);

#endif /* NGX_HTTP_RATELIMIT_UPSTREAM_H */
