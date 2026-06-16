#ifndef NGX_HTTP_RATELIMIT_MODULE_H
#define NGX_HTTP_RATELIMIT_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

extern ngx_module_t ngx_http_ratelimit_module;

typedef enum {
    NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW = 0
} ngx_http_ratelimit_algo_t;

/* A named rate definition (ratelimit_zone). No shared memory is allocated;
 * the counter lives in Redis. The zone only carries the rate semantics. */
typedef struct {
    ngx_str_t                 name;
    ngx_http_complex_value_t  key;
    ngx_uint_t                requests; /* limit base per window */
    ngx_uint_t                period;   /* window length in seconds */
    ngx_uint_t                burst;    /* default burst headroom */
    ngx_http_ratelimit_algo_t algo;     /* fixed window for now */
} ngx_http_ratelimit_zone_t;

typedef struct {
    ngx_array_t               zones; /* of ngx_http_ratelimit_zone_t */
} ngx_http_ratelimit_main_conf_t;

typedef struct {
    ngx_str_t                  zone_name; /* "ratelimit zone=" target name */
    ngx_http_ratelimit_zone_t *zone;      /* resolved at merge time */
    ngx_int_t                  burst;      /* per-location override, UNSET=zone */

    ngx_http_upstream_conf_t   upstream;
    ngx_http_complex_value_t  *complex_target; /* for ratelimit_pass */

    ngx_str_t                  password;  /* AUTH; empty disables AUTH */
    ngx_int_t                  database;  /* SELECT; <=0 disables SELECT */

    ngx_flag_t                 enable_headers;
    ngx_uint_t                 status_code;
    ngx_uint_t                 limit_log_level;

    ngx_str_t                  prefix;
    ngx_uint_t                 quantity;
} ngx_http_ratelimit_loc_conf_t;

typedef struct {
    ngx_str_t           key;

    ngx_http_request_t *request;

    /* used to parse the redis response */
    ngx_uint_t          state;

    /* flag indicating whether the rate limit has been finalized */
    ngx_flag_t          finalized;

    /* set once EVALSHA hit NOSCRIPT and we fell back to EVAL */
    ngx_flag_t          eval_fallback;

    /* AUTH/SELECT prelude sent only on a freshly opened connection */
    ngx_chain_t        *prelude_chain;   /* built before upstream init */
    ngx_uint_t          prelude_replies; /* +OK replies left to consume */
    ngx_buf_t          *auth_buf;        /* AUTH buffer, zeroed when done */

    /* parsed variables from the redis response */
    ngx_uint_t          limit;
    ngx_uint_t          remaining;
    ngx_uint_t          reset;
    ngx_int_t           retry_after;
} ngx_http_ratelimit_ctx_t;

#endif /* NGX_HTTP_RATELIMIT_MODULE_H */
