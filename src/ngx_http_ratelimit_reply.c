#include "ngx_http_ratelimit_reply.h"

/* Accumulate one decimal digit into *val, failing closed if the running
 * value would exceed NGX_MAX_INT_T_VALUE. The script contract returns small
 * seconds/counts, so the cap never rejects a valid reply; it only guards
 * against a malformed response overflowing (UB for the signed retry_after,
 * a wrapped header value for the unsigned fields). */
static ngx_int_t
ngx_http_ratelimit_accum_digit(ngx_uint_t *val, u_char ch)
{
    ngx_uint_t d = ch - '0';

    if (*val > (NGX_MAX_INT_T_VALUE - d) / 10) {
        return NGX_ERROR;
    }

    *val = *val * 10 + d;

    return NGX_OK;
}

ngx_int_t
ngx_http_ratelimit_process_reply(ngx_http_ratelimit_ctx_t *ctx, ssize_t bytes)
{
    ngx_buf_t *b;
    ngx_http_upstream_t *u;
    u_char ch, *p;

    u = ctx->request->upstream;
    b = &u->buffer;

    enum {
        sw_start = 0,
        sw_CRLF1,
        sw_ARG1,
        sw_CRLF2,
        sw_ARG2,
        sw_LF1,
        sw_ARG3,
        sw_LF2,
        sw_ARG4,
        sw_ALLOWED,
        sw_LF3,
        sw_ARG5,
        sw_almost_done
    } state;

    state = ctx->state;

    b->pos = b->last;
    b->last += bytes;

    /* Example response:
     * "5\r\n:0\r\n:16\r\n:15\r\n:-1\r\n:2\r\n"
     * Note: the first multi bulk reply byte (`*`) is
     * checked within `u->process_header`.
     */

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

        case sw_start:
            /* our bulk length must always be 5 */
            switch (ch) {
            case '5':
                state = sw_CRLF1;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_CRLF1:
            switch (ch) {
            case CR:
                break;
            case LF:
                state = sw_ARG1;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG1:
            /* 0 indicates the action is allowed
             * 1 indicates that the action was limited/blocked */
            switch (ch) {
            case ':':
                break;
            case '0':
                u->state->status = NGX_HTTP_OK;
                state = sw_CRLF2;
                break;
            case '1':
                u->state->status = NGX_HTTP_TOO_MANY_REQUESTS;
                state = sw_CRLF2;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_CRLF2:
            switch (ch) {
            case CR:
                break;
            case LF:
                state = sw_ARG2;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG2:
            /* X-RateLimit-Limit HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_LF1;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            if (ngx_http_ratelimit_accum_digit(&ctx->limit, ch) != NGX_OK) {
                return NGX_ERROR;
            }

            break;

        case sw_LF1:
            switch (ch) {
            case LF:
                state = sw_ARG3;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG3:
            /* X-RateLimit-Remaining HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_LF2;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            if (ngx_http_ratelimit_accum_digit(&ctx->remaining, ch) != NGX_OK) {
                return NGX_ERROR;
            }

            break;

        case sw_LF2:
            switch (ch) {
            case LF:
                state = sw_ARG4;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG4:
            /* The number of seconds until the user should retry,
             * and always -1 if the action was allowed. */
            if (ch == ':') {
                break;
            }

            if (ch == '-') {
                state = sw_ALLOWED;
                break;
            }

            if (ch == CR) {
                state = sw_LF3;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            /* retry_after is only ever accumulated as a non-negative value
             * here; the -1 allowed sentinel is set in sw_ALLOWED. Aliasing
             * the signed field through ngx_uint_t * is well-defined (same
             * rank, signed/unsigned variant). */
            if (ngx_http_ratelimit_accum_digit(
                    (ngx_uint_t *) &ctx->retry_after, ch) != NGX_OK)
            {
                return NGX_ERROR;
            }

            break;

        case sw_ALLOWED:
            switch (ch) {
            case '1':
                ctx->retry_after = -1;
                break;
            case CR:
                state = sw_LF3;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_LF3:
            switch (ch) {
            case LF:
                state = sw_ARG5;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG5:
            /* X-RateLimit-Reset HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_almost_done;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            if (ngx_http_ratelimit_accum_digit(&ctx->reset, ch) != NGX_OK) {
                return NGX_ERROR;
            }

            break;

        case sw_almost_done:
            /* End of redis response */
            switch (ch) {
            case LF:
                goto done;
            default:
                return NGX_ERROR;
            }
        }
    }

    b->pos = p;
    ctx->state = state;

    return NGX_AGAIN;

done:

    b->pos = p + 1;

    u->keepalive = 1;
    u->length = 0;

    return NGX_OK;
}
