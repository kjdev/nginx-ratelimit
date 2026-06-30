#include "ngx_http_ratelimit_upstream.h"
#include "ngx_http_ratelimit_module.h"

static ngx_int_t ngx_http_ratelimit_send_eval(ngx_http_request_t *r);
static void ngx_http_ratelimit_send_eval_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u);

/* Reference: ngx_http_upstream_finalize_request */
void
ngx_http_ratelimit_finalize_upstream_request(ngx_http_request_t *r,
    ngx_http_upstream_t *u,
    ngx_int_t rc)
{
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http rate limit request: %i", rc);

    if (u->cleanup == NULL) {
        /* the request was already finalized */
        ngx_http_finalize_request(r, NGX_DONE);
        return;
    }

    *u->cleanup = NULL;
    u->cleanup = NULL;

    if (u->resolved && u->resolved->ctx) {
        ngx_resolve_name_done(u->resolved->ctx);
        u->resolved->ctx = NULL;
    }

    if (u->state && u->state->response_time == (ngx_msec_t) -1) {
        u->state->response_time = ngx_current_msec - u->start_time;

        if (u->pipe && u->pipe->read_length) {
            u->state->bytes_received +=
                u->pipe->read_length - u->pipe->preread_size;
            u->state->response_length = u->pipe->read_length;
        }

        if (u->peer.connection) {
            u->state->bytes_sent = u->peer.connection->sent;
        }
    }

    u->finalize_request(r, rc);

    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
    }

    if (u->peer.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "close redis connection: %d", u->peer.connection->fd);

        if (u->peer.connection->pool) {
            ngx_destroy_pool(u->peer.connection->pool);
        }

        ngx_close_connection(u->peer.connection);
    }

    u->peer.connection = NULL;

    r->read_event_handler = ngx_http_block_reading;
    r->write_event_handler = ngx_http_core_run_phases;

    if (rc == NGX_DECLINED) {
        return;
    }

    r->connection->log->action = "sending to client";

    /*
     * The response is always produced by re-entering the phase handler: the
     * first finalize only releases the extra reference taken for the upstream
     * round-trip (no response is emitted), then ngx_http_core_run_phases re-
     * enters the handler, which returns the decision (OK/429) or the error
     * status, and the phase engine emits it exactly once.
     *
     * On a hard error the status is surfaced through u->state->status and the
     * first finalize uses NGX_DONE so it does not emit a response. Passing the
     * 5xx to the first finalize instead would send the special response now
     * and the phase re-run would finalize a second time, emitting "header
     * already sent while sending to client" once the response is on the wire.
     */
    if (rc != NGX_OK && rc != NGX_DONE) {
        u->state->status = (rc == NGX_ERROR)
                           ? NGX_HTTP_INTERNAL_SERVER_ERROR
                           : (ngx_uint_t) rc;
        rc = NGX_DONE;
    }

    ngx_http_finalize_request(r, rc);

    ngx_http_core_run_phases(r);
}

/* Reference: ngx_http_upstream_test_connect */
static ngx_int_t
ngx_http_ratelimit_test_connect(ngx_connection_t *c)
{
    int err;
    socklen_t len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to redis";
            (void) ngx_connection_error(
                c, err, "kevent() reported that connect() failed");
            return NGX_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) ==
            -1)
        {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to redis";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

/* Reference: ngx_http_upstream_process_non_buffered_request */
static void
ngx_http_ratelimit_process_redis_response(ngx_http_request_t *r,
    ngx_uint_t do_write)
{
    size_t size;
    ssize_t n;
    ngx_buf_t *b;
    ngx_connection_t *upstream;
    ngx_http_upstream_t *u;

    u = r->upstream;
    upstream = u->peer.connection;

    b = &u->buffer;

    do_write = do_write || u->length == 0;

    for (;;) {

        if (do_write) {

            if (u->out_bufs || u->busy_bufs) {
                ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_DONE);
                return;
            }

            if (u->busy_bufs == NULL) {

                /* Success is signalled only by a fully parsed reply:
                 * process_reply sets u->length = 0 at its done label, after all
                 * 5 elements are read. While u->length == -1 the parse is still
                 * incomplete, so an EOF in that state is a premature close, not
                 * end-of-data. The upstream original treated EOF with
                 * length == -1 as completion because there length == -1 means
                 * "read until the peer closes"; our reply is a fixed-size RESP
                 * array with a definite terminator, so a truncated read must
                 * not be accepted. It falls through to the read->eof branch
                 * below and is mapped to 502 (transport failure), keeping the
                 * verdict from being silently allowed and matching the
                 * header-read phase. */
                if (u->length == 0) {
                    ngx_http_ratelimit_finalize_upstream_request(r, u, 0);
                    return;
                }

                if (upstream->read->eof) {
                    ngx_log_error(NGX_LOG_ERR, upstream->log, 0,
                                  "redis prematurely closed connection");

                    ngx_http_ratelimit_finalize_upstream_request(
                        r, u, NGX_HTTP_BAD_GATEWAY);
                    return;
                }

                if (upstream->read->error) {
                    ngx_http_ratelimit_finalize_upstream_request(
                        r, u, NGX_HTTP_BAD_GATEWAY);
                    return;
                }

                b->pos = b->start;
                b->last = b->start;
            }
        }

        size = b->end - b->last;

        if (size && upstream->read->ready) {

            n = upstream->recv(upstream, b->last, size);

            if (n == NGX_AGAIN) {
                break;
            }

            if (n > 0) {
                u->state->bytes_received += n;
                u->state->response_length += n;

                if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
                    ngx_http_ratelimit_finalize_upstream_request(r, u,
                                                                 NGX_ERROR);
                    return;
                }
            }

            do_write = 1;

            continue;
        }

        break;
    }

    if (ngx_handle_read_event(upstream->read, 0) != NGX_OK) {
        ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    if (upstream->read->active && !upstream->read->ready) {
        ngx_add_timer(upstream->read, u->conf->read_timeout);

    } else if (upstream->read->timer_set) {
        ngx_del_timer(upstream->read);
    }
}

/* Reference: ngx_http_upstream_process_non_buffered_upstream */
static void
ngx_http_ratelimit_read_body_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ngx_connection_t *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http rate limit process redis response");

    c->log->action = "reading from redis";

    if (c->read->timedout) {
        ngx_connection_error(c, NGX_ETIMEDOUT, "redis timed out");
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    ngx_http_ratelimit_process_redis_response(r, 0);
}

/* Reference: ngx_http_upstream_dummy_handler */
static void
ngx_http_ratelimit_dummy_handler(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ratelimit: ngx_http_ratelimit_dummy_handler should not"
                  " be called by the upstream");
}

/* Reference: ngx_http_upstream_send_response */
static void
ngx_http_ratelimit_process_response(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ssize_t n;
    ngx_connection_t *c;
    ngx_http_core_loc_conf_t *clcf;

    c = r->connection;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    /*
     * Unlike the upstream original, we never set u->header_sent or touch
     * r->limit_rate: nothing is sent to the client. The response is always
     * non-buffered and the input filter is installed by create_request, so
     * the buffering and default-filter fallbacks are omitted.
     */
    u->read_event_handler = ngx_http_ratelimit_read_body_handler;

    /* The dummy write handler guarantees we never send to the client. */
    u->write_event_handler = ngx_http_ratelimit_dummy_handler;

    if (u->input_filter_init(u->input_filter_ctx) == NGX_ERROR) {
        ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    if (clcf->tcp_nodelay && ngx_tcp_nodelay(c) != NGX_OK) {
        ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    n = u->buffer.last - u->buffer.pos;

    if (n) {
        u->buffer.last = u->buffer.pos;

        u->state->response_length += n;

        if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
            ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_ERROR);
            return;
        }

        /*
         * The filter sets u->length to 0 once the reply is fully parsed. If
         * the reply was split across reads and only a prefix arrived with the
         * header, the parse is incomplete (u->length still -1); finalizing
         * here would abandon the rest of the reply and fail open (the request
         * would be treated as allowed). Defer to the body read handler, which
         * keeps reading until the reply completes.
         */
        if (u->length == 0) {
            ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_DONE);
            return;
        }

        ngx_http_ratelimit_read_body_handler(r, u);
    } else {
        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;

        /*
         * Carried over from the upstream original, this flushes the client
         * connection even though we never send a body. It is harmless only
         * because ignore_client_abort is set, which suppresses the resulting
         * client-side error. If that setting is ever changed, drop this call
         * and invoke ngx_http_ratelimit_read_body_handler(r, u) directly.
         */
        if (ngx_http_send_special(r, NGX_HTTP_FLUSH) == NGX_ERROR) {
            ngx_http_ratelimit_finalize_upstream_request(r, u, NGX_ERROR);
            return;
        }

        ngx_http_ratelimit_read_body_handler(r, u);
    }
}

/* Reference: ngx_http_upstream_process_header */
void
ngx_http_ratelimit_read_header_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ssize_t n;
    ngx_int_t rc;
    ngx_connection_t *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http rate limit rev handler");

    c->log->action = "reading response header from redis";

    /*
     * The upstream original retries the next peer via ngx_http_upstream_next
     * on timeout, connect failure, read error, or an invalid header. This
     * module talks to a single Redis peer with no failover, so every such
     * case finalizes the request directly with the mapped status.
     */

    if (c->read->timedout) {
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    if (!u->request_sent && ngx_http_ratelimit_test_connect(c) != NGX_OK) {
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_SERVICE_UNAVAILABLE);
        return;
    }

    if (u->buffer.start == NULL) {
        u->buffer.start = ngx_palloc(r->pool, u->conf->buffer_size);
        if (u->buffer.start == NULL) {
            ngx_http_ratelimit_finalize_upstream_request(
                r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;
        u->buffer.end = u->buffer.start + u->conf->buffer_size;
        u->buffer.temporary = 1;

        u->buffer.tag = u->output.tag;

        /* No need to init u->headers_in.headers and u->headers_in.trailers */
    }

    for (;;) {

        n = c->recv(c, u->buffer.last, u->buffer.end - u->buffer.last);

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_http_ratelimit_finalize_upstream_request(
                    r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);

                return;
            }

            return;
        }

        if (n == 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "redis prematurely closed connection");
        }

        /* A premature close or read error here is a Redis transport failure,
         * not a contract violation: map it to 502 so it matches the body-read
         * phase (read->eof / read->error) and the "ratelimit_on_error allow"
         * fail-open path treats every transport drop alike. 500 stays reserved
         * for internal and contract faults. */
        if (n == NGX_ERROR || n == 0) {
            ngx_http_ratelimit_finalize_upstream_request(
                r, u, NGX_HTTP_BAD_GATEWAY);
            return;
        }

        u->state->bytes_received += n;

        u->buffer.last += n;

        rc = u->process_header(r);

        if (rc == NGX_AGAIN) {

            if (u->buffer.last == u->buffer.end) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "redis sent too big header");

                ngx_http_ratelimit_finalize_upstream_request(
                    r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
                return;
            }

            continue;
        }

        break;
    }

    if (rc == NGX_HTTP_UPSTREAM_INVALID_HEADER) {
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* A write-side drop while resending EVAL on NOSCRIPT surfaces here as a
     * transport failure; finalize with 502 so it is handled like every other
     * Redis transport drop (fail-open under "ratelimit_on_error allow"). */
    if (rc == NGX_HTTP_BAD_GATEWAY) {
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_BAD_GATEWAY);
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_ratelimit_finalize_upstream_request(
            r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* rc == NGX_OK */

    u->state->header_time = ngx_current_msec - u->start_time;

    u->length = -1;

    ngx_http_ratelimit_process_response(r, u);
}

/* Write the rebuilt request (EVAL) to the upstream connection. */
static ngx_int_t
ngx_http_ratelimit_send_eval(ngx_http_request_t *r)
{
    ssize_t n, size;
    ngx_buf_t *b;
    ngx_connection_t *c;
    ngx_http_upstream_t *u;

    u = r->upstream;
    c = u->peer.connection;
    b = u->request_bufs->buf;

    while (b->pos < b->last) {
        size = b->last - b->pos;

        n = c->send(c, b->pos, size);

        if (n == NGX_AGAIN) {
            /* The socket is not writable yet; flush the rest from a write
             * event. The read handler stays in place for the reply. */
            u->write_event_handler = ngx_http_ratelimit_send_eval_handler;

            if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

        if (n == NGX_ERROR) {
            /* A write-side connection drop is a Redis transport failure, like
             * the read-path premature close / read error (both map to 502).
             * Return the mapped status so "ratelimit_on_error allow" fails open
             * on it; NGX_ERROR stays reserved for internal faults such as the
             * failed event registration above. */
            return NGX_HTTP_BAD_GATEWAY;
        }

        b->pos += n;
    }

    /* Fully sent; we never write again on this request. */
    u->write_event_handler = ngx_http_ratelimit_dummy_handler;

    return NGX_OK;
}

static void
ngx_http_ratelimit_send_eval_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ngx_int_t rc;

    /* rc carries NGX_HTTP_BAD_GATEWAY on a transport drop and NGX_ERROR on an
     * internal fault; finalize maps both (502 via state->status, NGX_ERROR to
     * 500), so pass it through rather than collapsing every failure into a 500. */
    rc = ngx_http_ratelimit_send_eval(r);
    if (rc != NGX_OK) {
        ngx_http_ratelimit_finalize_upstream_request(r, u, rc);
    }
}

ngx_int_t
ngx_http_ratelimit_resend_eval(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_upstream_t *u;
    ngx_http_ratelimit_ctx_t *ctx;

    u = r->upstream;

    ctx = ngx_http_get_module_ctx(r, ngx_http_ratelimit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    /* Latch the fallback so create_request emits EVAL and we never loop. */
    ctx->eval_fallback = 1;

    rc = u->create_request(r);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    /* Discard the NOSCRIPT reply and reset the parser for the EVAL reply. */
    u->buffer.pos = u->buffer.start;
    u->buffer.last = u->buffer.start;

    ctx->state = 0;
    ctx->limit = 0;
    ctx->remaining = 0;
    ctx->reset = 0;
    /* -1 is the "allowed" sentinel; the parser overwrites it from the reply,
     * but defaulting here avoids emitting a spurious "Retry-After: 0" if the
     * EVAL reply never completes. */
    ctx->retry_after = -1;

    /* AUTH/SELECT were already sent and consumed; never resend them. */
    ctx->prelude_replies = 0;

    return ngx_http_ratelimit_send_eval(r);
}
