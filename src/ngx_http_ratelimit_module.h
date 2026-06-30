#ifndef NGX_HTTP_RATELIMIT_MODULE_H
#define NGX_HTTP_RATELIMIT_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

extern ngx_module_t ngx_http_ratelimit_module;

/* Upper bound on the rate key length; a RESP bulk string argument fits in a
 * 16-bit length here, keeping the request frame small. */
#define NGX_HTTP_RATELIMIT_MAX_KEY_LEN  65535

/* Upper bound on a custom (algo=custom) Lua script body, validated when the
 * file is read at config parse time. Keeps the script body load bounded and
 * the EVAL fallback frame small. */
#define NGX_HTTP_RATELIMIT_MAX_SCRIPT_LEN  65536

/* Behaviour when the limiter cannot reach a verdict because Redis is
 * unreachable (connect refused, timeout, connection dropped). DENY keeps the
 * safe fail-closed default (the request is rejected); ALLOW fails open and lets
 * the request through. Selected by "ratelimit_on_error". Internal and contract
 * errors (a 500 from a malformed reply or a config fault) stay fail-closed
 * regardless: only the transport-error statuses honour ALLOW. */
#define NGX_HTTP_RATELIMIT_ON_ERROR_DENY   0
#define NGX_HTTP_RATELIMIT_ON_ERROR_ALLOW  1

typedef enum {
    NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW = 0,
    NGX_HTTP_RATELIMIT_ALGO_TOKEN_BUCKET,
    NGX_HTTP_RATELIMIT_ALGO_GCRA,
    NGX_HTTP_RATELIMIT_ALGO_SLIDING_WINDOW,
    /* User-supplied Lua script loaded from "script=". Kept last so the static
     * ngx_http_ratelimit_scripts[] table (indexed by the built-in algorithms)
     * is never indexed by it; the body and SHA live on the zone instead. */
    NGX_HTTP_RATELIMIT_ALGO_CUSTOM
} ngx_http_ratelimit_algo_t;

/* A named rate definition (ratelimit_zone). No shared memory is allocated;
 * the counter lives in Redis. The zone only carries the rate semantics. */
typedef struct {
    ngx_str_t                  name;
    ngx_http_complex_value_t   key;
    ngx_uint_t                 requests; /* limit base per window */
    ngx_uint_t                 period;  /* window length in seconds */
    ngx_uint_t                 burst;   /* default burst headroom */
    ngx_http_ratelimit_algo_t  algo;    /* fixed/sliding window, token bucket,
                                         * gcra, or custom */

    /* algo=custom only. The script body is read from script_path at config
     * parse time; the SHA1 is computed in postconfiguration into sha_buf and
     * referenced by script_sha. Empty for built-in algorithms. */
    ngx_str_t  script_path;
    ngx_str_t  script_body;
    ngx_str_t  script_sha;
    u_char     sha_buf[40];
} ngx_http_ratelimit_zone_t;

typedef struct {
    ngx_array_t  zones;              /* of ngx_http_ratelimit_zone_t */

    /* Index of the reserved per-request variable slot used as the one-shot
     * "already decided" marker. It survives the r->ctx wipe that an internal
     * redirect performs, so the limiter runs once per main request. */
    ngx_int_t    done_index;
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
    ngx_uint_t                 on_error; /* DENY (fail-closed) | ALLOW (open) */

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

    /* AUTH/SELECT prelude prepended to the EVALSHA on every request */
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
