#include "ngx_http_ratelimit_util.h"

static size_t ngx_get_num_size(uint64_t i);

ngx_http_upstream_srv_conf_t *
ngx_http_rate_limit_upstream_add(ngx_http_request_t *r, ngx_url_t *url)
{
    ngx_http_upstream_main_conf_t *umcf;
    ngx_http_upstream_srv_conf_t **uscfp;
    ngx_uint_t i;

    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->host.len != url->host.len
            || ngx_strncasecmp(uscfp[i]->host.data, url->host.data,
                               url->host.len) != 0)
        {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: host not match");
            continue;
        }

        if (uscfp[i]->port != url->port) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: port not match: %d != %d",
                           (int) uscfp[i]->port, (int) url->port);
            continue;
        }

#if defined(nginx_version) && nginx_version < 1011006
        if (uscfp[i]->default_port && url->default_port
            && uscfp[i]->default_port != url->default_port)
        {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: default_port not match");
            continue;
        }
#endif

        return uscfp[i];
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "no upstream found: %V", &url->host);

    return NULL;
}

static size_t
ngx_get_num_size(uint64_t i)
{
    size_t n = 0;

    do {
        i = i / 10;
        n++;
    } while (i > 0);

    return n;
}

ngx_int_t
ngx_http_ratelimit_build_command(ngx_http_request_t *r, ngx_buf_t **b,
    ngx_str_t *argv, ngx_uint_t argc)
{
    size_t len;
    u_char *p;
    ngx_uint_t i;

    /* Encode a RESP array of bulk strings:
     *   "*<argc>\r\n" then "$<len>\r\n<data>\r\n" for each argument.
     */

    len = sizeof("*") - 1;
    len += ngx_get_num_size(argc);
    len += sizeof("\r\n") - 1;

    for (i = 0; i < argc; i++) {
        len += sizeof("$") - 1;
        len += ngx_get_num_size(argv[i].len);
        len += sizeof("\r\n") - 1;
        len += argv[i].len;
        len += sizeof("\r\n") - 1;
    }

    *b = ngx_create_temp_buf(r->pool, len);
    if (*b == NULL) {
        return NGX_ERROR;
    }

    p = (*b)->last;

    *p++ = '*';
    p = ngx_sprintf(p, "%ui", argc);
    *p++ = CR;
    *p++ = LF;

    for (i = 0; i < argc; i++) {
        *p++ = '$';
        p = ngx_sprintf(p, "%uz", argv[i].len);
        *p++ = CR;
        *p++ = LF;
        p = ngx_copy(p, argv[i].data, argv[i].len);
        *p++ = CR;
        *p++ = LF;
    }

    if (p - (*b)->pos != (ssize_t) len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit: buffer error %uz != %uz",
                      (size_t) (p - (*b)->pos), len);

        return NGX_ERROR;
    }

    (*b)->last = p;

    return NGX_OK;
}

ngx_int_t
ngx_set_custom_header(ngx_http_request_t *r, ngx_str_t *key, ngx_uint_t value)
{
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    /* Mark the header as not deleted. */
    h->hash = 1;
    h->key = *key;

    h->value.data = ngx_pnalloc(r->pool, ngx_get_num_size(value));
    if (h->value.data == NULL) {
        h->hash = 0;
        return NGX_ERROR;
    }

    h->value.len = ngx_sprintf(h->value.data, "%ui", value) - h->value.data;

    h->lowcase_key = ngx_pnalloc(r->pool, h->key.len);
    if (h->lowcase_key == NULL) {
        return NGX_ERROR;
    }

    ngx_strlow(h->lowcase_key, h->key.data, h->key.len);

    return NGX_OK;
}
