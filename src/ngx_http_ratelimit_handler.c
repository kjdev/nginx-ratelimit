#include "ngx_http_ratelimit_handler.h"
#include "ngx_http_ratelimit_reply.h"
#include "ngx_http_ratelimit_script.h"
#include "ngx_http_ratelimit_upstream.h"
#include "ngx_http_ratelimit_util.h"

static ngx_int_t ngx_http_ratelimit_set_uint_arg(ngx_http_request_t *r,
    ngx_str_t *arg, ngx_uint_t value);
static ngx_int_t ngx_http_ratelimit_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_ratelimit_build_prelude(ngx_http_request_t *r,
    ngx_http_ratelimit_ctx_t *ctx);
static ngx_int_t ngx_http_ratelimit_consume_prelude_reply(
    ngx_http_request_t *r, ngx_buf_t *b);
static ngx_int_t ngx_http_ratelimit_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_ratelimit_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_ratelimit_filter_init(void *data);
static ngx_int_t ngx_http_ratelimit_filter(void *data, ssize_t bytes);
static void ngx_http_ratelimit_abort_request(ngx_http_request_t *r);
static void ngx_http_ratelimit_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);
static void ngx_http_ratelimit_wipe_auth(void *data);

static ngx_str_t x_limit_header = ngx_string("X-RateLimit-Limit");
static ngx_str_t x_remaining_header = ngx_string("X-RateLimit-Remaining");
static ngx_str_t x_reset_header = ngx_string("X-RateLimit-Reset");
static ngx_str_t x_retry_after_header = ngx_string("Retry-After");

ngx_int_t
ngx_http_ratelimit_handler(ngx_http_request_t *r)
{
    ngx_http_upstream_t *u;
    ngx_http_ratelimit_ctx_t *ctx;
    ngx_http_ratelimit_loc_conf_t *rlcf;
    size_t len;
    u_char *p, *n;
    ngx_uint_t status;
    ngx_str_t target;
    ngx_url_t url;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_ratelimit_module);

    if (rlcf->zone == NULL) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_ratelimit_module);

    if (ctx != NULL) {
        if (!ctx->finalized) {
            return NGX_AGAIN;
        }

        status = r->upstream->state->status;

        /* Return appropriate status */

        if (status == NGX_HTTP_TOO_MANY_REQUESTS) {
            ngx_log_error(rlcf->limit_log_level, r->connection->log, 0,
                          "rate limit exceeded for zone \"%V\"",
                          &rlcf->zone->name);

            return rlcf->status_code;
        }

        if (status == NGX_HTTP_OK) {
            return NGX_OK;
        }

        /* A hard upstream error (redis unreachable, AUTH/SELECT rejected,
         * timeout, ...) surfaces its HTTP status here so the phase engine
         * emits it exactly once on this re-entry, failing the request closed. */
        if (status >= NGX_HTTP_BAD_REQUEST) {
            return (ngx_int_t) status;
        }

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit unexpected status: %ui", status);

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_ratelimit_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_complex_value(r, &rlcf->zone->key, &ctx->key) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ctx->key.len == 0) {
        return NGX_DECLINED;
    }

    len = rlcf->prefix.len;

    if (len > 0) {
        /* Build "<prefix>_<key>". The key is a length-delimited value (not
         * NUL-terminated), so copy by length rather than with ngx_cpystrn. */
        n = ngx_pnalloc(r->pool, len + 1 + ctx->key.len);
        if (n == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        p = ngx_cpymem(n, rlcf->prefix.data, len);
        *p++ = '_';
        p = ngx_cpymem(p, ctx->key.data, ctx->key.len);

        ctx->key.len = p - n;
        ctx->key.data = n;
    }

    if (ctx->key.len > NGX_HTTP_RATELIMIT_MAX_KEY_LEN) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ratelimit zone \"%V\" key is %uz bytes, "
                      "more than the %d byte limit",
                      &rlcf->zone->name, ctx->key.len,
                      NGX_HTTP_RATELIMIT_MAX_KEY_LEN);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request = r;

    /* Build the AUTH/SELECT prelude up front (the only fallible step) so the
     * post-init splice into request_bufs cannot fail. ctx is passed in since
     * it is not registered with the module until after upstream setup. */
    if (ngx_http_ratelimit_build_prelude(r, ctx) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u = r->upstream;

    if (rlcf->complex_target) {
        /* Variables used in the ratelimit_pass directive */

        if (ngx_http_complex_value(r, rlcf->complex_target, &target) !=
            NGX_OK)
        {
            return NGX_ERROR;
        }

        if (target.len == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "ratelimit: empty \"ratelimit_pass\" target");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        url.host = target;
        url.port = 0;
        url.no_resolve = 1;

        rlcf->upstream.upstream = ngx_http_ratelimit_upstream_add(r, &url);

        if (rlcf->upstream.upstream == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "ratelimit: upstream \"%V\" not found", &target);

            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    ngx_str_set(&u->schema, "redis2://");
    u->output.tag = (ngx_buf_tag_t) &ngx_http_ratelimit_module;

    u->conf = &rlcf->upstream;

    u->create_request = ngx_http_ratelimit_create_request;
    u->reinit_request = ngx_http_ratelimit_reinit_request;
    u->process_header = ngx_http_ratelimit_process_header;
    u->abort_request = ngx_http_ratelimit_abort_request;
    u->finalize_request = ngx_http_ratelimit_finalize_request;

    ngx_http_set_ctx(r, ctx, ngx_http_ratelimit_module);

    u->input_filter_init = ngx_http_ratelimit_filter_init;
    u->input_filter = ngx_http_ratelimit_filter;
    u->input_filter_ctx = ctx;

    r->main->count++;

    /* Initiate the upstream connection by calling NGINX upstream. */
    ngx_http_upstream_init(r);

    /* Override the read event handler to our own */
    u->read_event_handler = ngx_http_ratelimit_rev_handler;

    /* AUTH/SELECT is sent only on a freshly opened connection. A reused
     * keepalive connection (peer.cached) is already authenticated, so the
     * prelude is skipped. The prelude must precede the request, so it is only
     * spliced while the request has not been sent yet (true for an async TCP
     * connect, which returns NGX_AGAIN). A synchronous connect would have sent
     * the request already; that path is left to the natural -NOAUTH handling. */
    if (ctx->prelude_chain != NULL && !u->peer.cached && !u->request_sent) {
        ngx_chain_t *cl;

        for (cl = ctx->prelude_chain; cl->next; cl = cl->next) { /* void */
        }

        cl->next = u->request_bufs;
        u->request_bufs = ctx->prelude_chain;

    } else {
        /* Prelude not sent: do not wait for its replies. */
        ctx->prelude_replies = 0;
    }

    return NGX_AGAIN;
}

/* Format an unsigned value into a fresh request-pool buffer and point arg at
 * it. Collapses the repeated ngx_pnalloc + ngx_sprintf idiom for ARGV. */
static ngx_int_t
ngx_http_ratelimit_set_uint_arg(ngx_http_request_t *r, ngx_str_t *arg,
    ngx_uint_t value)
{
    u_char *p;

    p = ngx_pnalloc(r->pool, NGX_INT_T_LEN);
    if (p == NULL) {
        return NGX_ERROR;
    }

    arg->data = p;
    arg->len = ngx_sprintf(p, "%ui", value) - p;

    return NGX_OK;
}

static ngx_int_t
ngx_http_ratelimit_create_request(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t *cl;
    ngx_http_ratelimit_ctx_t *ctx;
    ngx_http_ratelimit_loc_conf_t *rlcf;
    ngx_http_ratelimit_algo_t algo;
    ngx_str_t argv[8];
    ngx_uint_t burst, argc;

    ctx = ngx_http_get_module_ctx(r, ngx_http_ratelimit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_ratelimit_module);

    algo = rlcf->zone->algo;

    /* A per-location burst overrides the zone default. */
    burst = (rlcf->burst == NGX_CONF_UNSET)
            ? rlcf->zone->burst : (ngx_uint_t) rlcf->burst;

    /* EVALSHA <sha> 1 <key> <args...>, falling back to EVAL <script> 1 <key>
     * ... once the server reports NOSCRIPT. The script is selected per zone
     * algorithm. */
    if (ctx->eval_fallback) {
        ngx_str_set(&argv[0], "EVAL");
        argv[1] = *ngx_http_ratelimit_script_body(algo);
    } else {
        ngx_str_set(&argv[0], "EVALSHA");
        argv[1] = *ngx_http_ratelimit_script_sha(algo);
    }

    ngx_str_set(&argv[2], "1");

    /* KEYS[1] */
    argv[3] = ctx->key;

    if (algo == NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW) {

        /* ARGV: <limit> <window> <quantity>; limit is requests plus burst
         * headroom over the window. */
        if (ngx_http_ratelimit_set_uint_arg(r, &argv[4],
                                            rlcf->zone->requests + burst)
            != NGX_OK
            || ngx_http_ratelimit_set_uint_arg(r, &argv[5], rlcf->zone->period)
            != NGX_OK
            || ngx_http_ratelimit_set_uint_arg(r, &argv[6], rlcf->quantity)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        argc = 7;

    } else {

        /* Token bucket / GCRA share ARGV: <requests> <period> <burst>
         * <quantity>. The script derives rate and capacity from them. */
        if (ngx_http_ratelimit_set_uint_arg(r, &argv[4], rlcf->zone->requests)
            != NGX_OK
            || ngx_http_ratelimit_set_uint_arg(r, &argv[5], rlcf->zone->period)
            != NGX_OK
            || ngx_http_ratelimit_set_uint_arg(r, &argv[6], burst)
            != NGX_OK
            || ngx_http_ratelimit_set_uint_arg(r, &argv[7], rlcf->quantity)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        argc = 8;
    }

    rc = ngx_http_ratelimit_build_command(r, &b, argv, argc);
    if (rc != NGX_OK) {
        return rc;
    }

    /* Allocate a buffer chain for NGINX. */
    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    /* We are only sending one buffer. */
    b->last_buf = 1;

    /* Attach the buffer to the request. */
    r->upstream->request_bufs = cl;

    return NGX_OK;
}

/* Append one RESP command (argv/argc) as a new chain link onto *ll. */
static ngx_int_t
ngx_http_ratelimit_append_command(ngx_http_request_t *r, ngx_chain_t ***ll,
    ngx_str_t *argv, ngx_uint_t argc, ngx_buf_t **out_buf)
{
    ngx_buf_t *b;
    ngx_chain_t *cl;

    if (ngx_http_ratelimit_build_command(r, &b, argv, argc) != NGX_OK) {
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    **ll = cl;
    *ll = &cl->next;

    if (out_buf != NULL) {
        *out_buf = b;
    }

    return NGX_OK;
}

/*
 * Build the AUTH/SELECT prelude for this request. The chain is assembled here
 * (the only step that allocates) and held in the ctx; the handler splices it
 * ahead of the EVALSHA request only when the connection is freshly opened.
 */
static ngx_int_t
ngx_http_ratelimit_build_prelude(ngx_http_request_t *r,
    ngx_http_ratelimit_ctx_t *ctx)
{
    ngx_http_ratelimit_loc_conf_t *rlcf;
    ngx_chain_t *head, **ll;
    ngx_pool_cleanup_t *cln;
    ngx_str_t argv[2];
    ngx_uint_t count;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_ratelimit_module);

    head = NULL;
    ll = &head;
    count = 0;

    if (rlcf->password.len > 0) {
        ngx_str_set(&argv[0], "AUTH");
        argv[1] = rlcf->password;

        if (ngx_http_ratelimit_append_command(r, &ll, argv, 2, &ctx->auth_buf)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        /* Wipe the password unconditionally when the pool is destroyed. The
         * prompt wipe in finalize_request covers the normal path, but early
         * errors before upstream init return without registering the ctx (and
         * thus without finalize running); this cleanup is the backstop. */
        cln = ngx_pool_cleanup_add(r->pool, 0);
        if (cln == NULL) {
            return NGX_ERROR;
        }
        cln->handler = ngx_http_ratelimit_wipe_auth;
        cln->data = ctx->auth_buf;

        count++;
    }

    if (rlcf->database > 0) {
        ngx_str_set(&argv[0], "SELECT");

        /* database > 0 here, so the unsigned formatting is exact. */
        if (ngx_http_ratelimit_set_uint_arg(r, &argv[1],
                                            (ngx_uint_t) rlcf->database)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (ngx_http_ratelimit_append_command(r, &ll, argv, 2, NULL)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        count++;
    }

    ctx->prelude_chain = head;
    ctx->prelude_replies = count;

    return NGX_OK;
}

/*
 * Consume one AUTH/SELECT reply ("+OK" / "-ERR") sitting at the head of the
 * buffer. NGX_AGAIN if the line has not fully arrived; NGX_ERROR on a redis
 * error reply (the password value is never logged).
 */
static ngx_int_t
ngx_http_ratelimit_consume_prelude_reply(ngx_http_request_t *r, ngx_buf_t *b)
{
    u_char *p;
    ngx_str_t reply;

    for (p = b->pos; p < b->last; p++) {
        if (*p == LF) {
            break;
        }
    }

    if (p == b->last) {
        /* the reply line has not fully arrived yet */
        return NGX_AGAIN;
    }

    if (*b->pos == '+') {
        b->pos = p + 1;
        return NGX_OK;
    }

    reply.data = b->pos;
    reply.len = p - b->pos;

    if (reply.len > 0 && reply.data[reply.len - 1] == CR) {
        reply.len--;
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ratelimit: redis auth/select error reply: \"%V\"", &reply);

    return NGX_ERROR;
}

static ngx_int_t
ngx_http_ratelimit_reinit_request(ngx_http_request_t *r)
{
    ngx_http_upstream_t *u;

    u = r->upstream;

    /* Override the read event handler to our own */
    u->read_event_handler = ngx_http_ratelimit_rev_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_ratelimit_process_header(ngx_http_request_t *r)
{
    ngx_http_upstream_t *u;
    ngx_http_ratelimit_ctx_t *ctx;
    ngx_buf_t *b;
    u_char chr, *p;
    ngx_str_t buf;

    u = r->upstream;
    b = &u->buffer;

    ctx = ngx_http_get_module_ctx(r, ngx_http_ratelimit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    /* Consume the AUTH/SELECT "+OK" replies that precede the EVALSHA reply
     * on a freshly opened connection, before reply.c sees the 5-integer
     * array. */
    while (ctx->prelude_replies > 0) {
        ngx_int_t prc;

        prc = ngx_http_ratelimit_consume_prelude_reply(r, b);
        if (prc != NGX_OK) {
            return prc;
        }

        ctx->prelude_replies--;
    }

    if (b->last - b->pos < (ssize_t) sizeof(u_char)) {
        return NGX_AGAIN;
    }

    /* the first char is the response type */
    chr = *b->pos;

    /* a multi bulk reply carries the 5-integer contract parsed by reply.c */
    if (chr == '*') {
        ++b->pos;

        u->state->status = NGX_HTTP_OK;

        return NGX_OK;
    }

    /* an error reply ("-NOSCRIPT ...", "-ERR ...") spans up to CRLF */
    if (chr == '-') {

        for (p = b->pos + 1; p < b->last; p++) {
            if (*p == LF) {
                break;
            }
        }

        if (p == b->last) {
            /* the error line has not fully arrived yet */
            return NGX_AGAIN;
        }

        /* On NOSCRIPT, load the script with EVAL on this connection once and
         * resume reading its reply. */
        if (!ctx->eval_fallback
            && b->last - b->pos >= (ssize_t) (sizeof("-NOSCRIPT") - 1)
            && ngx_strncmp(b->pos + 1, "NOSCRIPT", sizeof("NOSCRIPT") - 1)
            == 0)
        {
            if (ngx_http_ratelimit_resend_eval(r) != NGX_OK) {
                return NGX_ERROR;
            }

            return NGX_AGAIN;
        }

        buf.data = b->pos;
        buf.len = p - b->pos;

        if (buf.len > 0 && buf.data[buf.len - 1] == CR) {
            buf.len--;
        }

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ratelimit: redis error reply: \"%V\"", &buf);

        return NGX_ERROR;
    }

    buf.data = b->pos;
    buf.len = b->last - b->pos;

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ratelimit: redis sent invalid response: \"%V\"", &buf);

    return NGX_HTTP_UPSTREAM_INVALID_HEADER;
}

static ngx_int_t
ngx_http_ratelimit_filter_init(void *data)
{
    return NGX_OK;
}

static ngx_int_t
ngx_http_ratelimit_filter(void *data, ssize_t bytes)
{
    ngx_http_ratelimit_ctx_t *ctx = data;

    return ngx_http_ratelimit_process_reply(ctx, bytes);
}

static void
ngx_http_ratelimit_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http rate limit request");
    return;
}

static void
ngx_http_ratelimit_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_http_ratelimit_ctx_t *ctx;
    ngx_http_ratelimit_loc_conf_t *rlcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http rate limit request");

    ctx = ngx_http_get_module_ctx(r, ngx_http_ratelimit_module);
    if (ctx == NULL) {
        return;
    }

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_ratelimit_module);

    if (r->upstream->state->status == NGX_HTTP_TOO_MANY_REQUESTS
        || rlcf->enable_headers)
    {
        /* X-RateLimit-Limit HTTP header */
        (void) ngx_http_ratelimit_set_custom_header(r, &x_limit_header,
                                                    ctx->limit);

        /* X-RateLimit-Remaining HTTP header */
        (void) ngx_http_ratelimit_set_custom_header(r, &x_remaining_header,
                                                    ctx->remaining);

        /* X-RateLimit-Reset */
        (void) ngx_http_ratelimit_set_custom_header(r, &x_reset_header,
                                                    ctx->reset);

        /* Retry-After (always -1 if the action was allowed) */
        if (ctx->retry_after != -1) {
            (void) ngx_http_ratelimit_set_custom_header(r,
                                                        &x_retry_after_header,
                                                        (ngx_uint_t) ctx->
                                                        retry_after);
        }
    }

    /* Wipe the AUTH password from the request buffer once it is no longer
     * needed (the request pool is freed but not zeroed). */
    if (ctx->auth_buf != NULL) {
        ngx_memzero(ctx->auth_buf->start,
                    ctx->auth_buf->end - ctx->auth_buf->start);
        ctx->auth_buf = NULL;
    }

    ctx->finalized = 1;
}

/* Pool cleanup backstop: zero the AUTH password buffer regardless of whether
 * the request reached finalize_request (early-error paths skip it). Idempotent
 * with the wipe in finalize_request; a re-zeroed buffer stays zeroed. */
static void
ngx_http_ratelimit_wipe_auth(void *data)
{
    ngx_buf_t *b = data;

    ngx_memzero(b->start, b->end - b->start);
}
