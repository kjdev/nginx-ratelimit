#include "ngx_http_ratelimit_module.h"
#include "ngx_http_ratelimit_handler.h"
#include "ngx_http_ratelimit_script.h"
#include "ngx_http_ratelimit_util.h"

static ngx_int_t ngx_http_ratelimit_init(ngx_conf_t *cf);
static ngx_int_t ngx_http_ratelimit_done_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void *ngx_http_ratelimit_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_ratelimit_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_ratelimit_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_ratelimit_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_ratelimit(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_ratelimit_pass(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_conf_enum_t ngx_http_ratelimit_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};

static ngx_conf_num_bounds_t ngx_http_ratelimit_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};

static ngx_command_t ngx_http_ratelimit_commands[] = {

    { ngx_string("ratelimit_zone"),
      NGX_HTTP_MAIN_CONF | NGX_CONF_2MORE,
      ngx_http_ratelimit_zone, 0, 0, NULL },

    { ngx_string("ratelimit"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE123,
      ngx_http_ratelimit, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("ratelimit_prefix"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, prefix), NULL },

    { ngx_string("ratelimit_quantity"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, quantity), NULL },

    { ngx_string("ratelimit_pass"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_http_ratelimit_pass, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("ratelimit_password"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_str_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, password), NULL },

    { ngx_string("ratelimit_database"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, database), NULL },

    { ngx_string("ratelimit_headers"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_FLAG,
      ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, enable_headers), NULL },

    { ngx_string("ratelimit_log_level"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, limit_log_level),
      &ngx_http_ratelimit_log_levels },

    { ngx_string("ratelimit_status"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_num_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, status_code),
      &ngx_http_ratelimit_status_bounds },

    { ngx_string("ratelimit_buffer_size"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_size_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, upstream.buffer_size), NULL },

    { ngx_string("ratelimit_connect_timeout"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, upstream.connect_timeout),
      NULL },

    { ngx_string("ratelimit_send_timeout"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, upstream.send_timeout), NULL },

    { ngx_string("ratelimit_read_timeout"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF |
      NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot, NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_ratelimit_loc_conf_t, upstream.read_timeout), NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_ratelimit_module_ctx = {
    NULL,                     /* preconfiguration */
    ngx_http_ratelimit_init, /* postconfiguration */

    ngx_http_ratelimit_create_main_conf, /* create main configuration */
    NULL,                                /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_ratelimit_create_loc_conf, /* create location configration */
    ngx_http_ratelimit_merge_loc_conf   /* merge location configration */
};

ngx_module_t ngx_http_ratelimit_module = {
    NGX_MODULE_V1,
    &ngx_http_ratelimit_module_ctx, /* module context */
    ngx_http_ratelimit_commands,    /* module directives */
    NGX_HTTP_MODULE,                 /* module type */
    NULL,                            /* init master */
    NULL,                            /* init module */
    NULL,                            /* init process */
    NULL,                            /* init thread */
    NULL,                            /* exit thread */
    NULL,                            /* exit process */
    NULL,                            /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_ratelimit_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_ratelimit_main_conf_t *rmcf;

    rmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ratelimit_main_conf_t));
    if (rmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&rmcf->zones, cf->pool, 4,
                       sizeof(ngx_http_ratelimit_zone_t)) != NGX_OK)
    {
        return NULL;
    }

    return rmcf;
}

static void *
ngx_http_ratelimit_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_ratelimit_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ratelimit_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream.bufs.num = 0;
     *     conf->upstream.next_upstream = 0;
     *     conf->upstream.temp_path = NULL;
     *     conf->upstream.uri = { 0, NULL };
     *     conf->upstream.location = NULL;
     *
     *     conf->prefix = { 0, NULL };
     */

    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;

    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;

    /* the hardcoded values */
    conf->upstream.cyclic_temp_file = 0;
    conf->upstream.buffering = 0;
    conf->upstream.ignore_client_abort = 1;
    conf->upstream.send_lowat = 0;
    conf->upstream.bufs.num = 0;
    conf->upstream.busy_buffers_size = 0;
    conf->upstream.max_temp_file_size = 0;
    conf->upstream.temp_file_write_size = 0;
    conf->upstream.intercept_errors = 1;
    conf->upstream.intercept_404 = 1;
    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 0;

    conf->enable_headers = NGX_CONF_UNSET;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->limit_log_level = NGX_CONF_UNSET_UINT;

    conf->burst = NGX_CONF_UNSET;
    conf->quantity = NGX_CONF_UNSET_UINT;
    conf->database = NGX_CONF_UNSET;

    return conf;
}

static char *
ngx_http_ratelimit_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_ratelimit_loc_conf_t *prev = parent;
    ngx_http_ratelimit_loc_conf_t *conf = child;

    ngx_http_ratelimit_main_conf_t *rmcf;
    ngx_http_ratelimit_zone_t *zone;
    ngx_uint_t i;

    /* Inherit the zone reference when this location has no "ratelimit". */
    if (conf->zone_name.len == 0) {
        conf->zone_name = prev->zone_name;
        conf->zone = prev->zone;
    }

    /* Resolve the zone name against the global registry now that all
     * ratelimit_zone directives have been parsed. */
    if (conf->zone_name.len && conf->zone == NULL) {
        rmcf = ngx_http_conf_get_module_main_conf(cf,
                                                  ngx_http_ratelimit_module);
        zone = rmcf->zones.elts;

        for (i = 0; i < rmcf->zones.nelts; i++) {
            if (zone[i].name.len == conf->zone_name.len
                && ngx_strncmp(zone[i].name.data, conf->zone_name.data,
                               conf->zone_name.len) == 0)
            {
                conf->zone = &zone[i];
                break;
            }
        }

        if (conf->zone == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "unknown ratelimit zone \"%V\"",
                               &conf->zone_name);
            return NGX_CONF_ERROR;
        }
    }

    ngx_conf_merge_value(conf->burst, prev->burst, NGX_CONF_UNSET);

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);

    ngx_conf_merge_size_value(conf->upstream.buffer_size,
                              prev->upstream.buffer_size,
                              (size_t) ngx_pagesize);

    if (conf->upstream.upstream == NULL) {
        conf->upstream.upstream = prev->upstream.upstream;
    }

    /* Inherit the variable target alongside upstream.upstream so a parent
     * "ratelimit_pass $var" still applies to a child that only adds
     * "ratelimit zone=". Without this, the zone check below would misfire on
     * such an inherited target. */
    if (conf->complex_target == NULL) {
        conf->complex_target = prev->complex_target;
    }

    /* An active zone needs a resolved redis target: either a static
     * "ratelimit_pass <name>" (upstream.upstream) or a variable target
     * (complex_target). Without one the PREACCESS handler would dereference a
     * NULL upstream at request time, so reject the config at load instead. */
    if (conf->zone != NULL
        && conf->upstream.upstream == NULL
        && conf->complex_target == NULL)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit zone \"%V\" requires \"ratelimit_pass\"",
                           &conf->zone_name);
        return NGX_CONF_ERROR;
    }

    ngx_conf_merge_value(conf->enable_headers, prev->enable_headers, 0);
    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_TOO_MANY_REQUESTS);
    ngx_conf_merge_uint_value(conf->limit_log_level, prev->limit_log_level,
                              NGX_LOG_ERR);

    ngx_conf_merge_str_value(conf->prefix, prev->prefix, "");
    ngx_conf_merge_uint_value(conf->quantity, prev->quantity, 1);

    ngx_conf_merge_str_value(conf->password, prev->password, "");
    ngx_conf_merge_value(conf->database, prev->database, NGX_CONF_UNSET);

    return NGX_CONF_OK;
}

/* Parse "rate=NNr/{s|m|h}" into a (requests, period-in-seconds) pair. */
static char *
ngx_http_ratelimit_parse_rate(ngx_conf_t *cf, ngx_str_t *value,
    ngx_int_t *requests, ngx_int_t *period)
{
    u_char *p, *last;
    ngx_int_t scale;
    size_t len;

    /* skip "rate=" */
    p = value->data + sizeof("rate=") - 1;
    last = value->data + value->len;

    len = 0;
    while (p + len < last && p[len] >= '0' && p[len] <= '9') {
        len++;
    }

    *requests = ngx_atoi(p, len);
    if (*requests <= 0) {
        return "invalid";
    }

    p += len;

    if (last - p != 3 || p[0] != 'r' || p[1] != '/') {
        return "invalid";
    }

    switch (p[2]) {
    case 's':
        scale = 1;
        break;
    case 'm':
        scale = 60;
        break;
    case 'h':
        scale = 3600;
        break;
    default:
        return "invalid";
    }

    *period = scale;

    return NGX_CONF_OK;
}

static char *
ngx_http_ratelimit_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_ratelimit_main_conf_t *rmcf = conf;

    ngx_str_t *value, s;
    ngx_int_t requests, period, burst;
    ngx_uint_t i, has_rate, has_requests, has_period, has_key;
    ngx_http_ratelimit_zone_t *zone;
    ngx_http_compile_complex_value_t ccv;

    value = cf->args->elts;

    zone = rmcf->zones.elts;
    for (i = 0; i < rmcf->zones.nelts; i++) {
        if (zone[i].name.len == value[1].len
            && ngx_strncmp(zone[i].name.data, value[1].data, value[1].len)
            == 0)
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "duplicate ratelimit zone \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }
    }

    zone = ngx_array_push(&rmcf->zones);
    if (zone == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(zone, sizeof(ngx_http_ratelimit_zone_t));
    zone->name = value[1];
    zone->algo = NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW;

    requests = 0;
    period = 0;
    burst = 0;
    has_rate = has_requests = has_period = has_key = 0;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "key=", 4) == 0) {

            s.len = value[i].len - 4;
            s.data = value[i].data + 4;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &zone->key;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            has_key = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            if (ngx_http_ratelimit_parse_rate(cf, &value[i], &requests,
                                              &period) != NGX_CONF_OK)
            {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            has_rate = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "requests=", 9) == 0) {

            requests = ngx_atoi(value[i].data + 9, value[i].len - 9);
            if (requests <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid requests value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            has_requests = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "period=", 7) == 0) {

            s.len = value[i].len - 7;
            s.data = value[i].data + 7;

            period = ngx_parse_time(&s, 1);
            if (period <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid period time \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            has_period = 1;
            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "algo=", 5) == 0) {

            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            if (s.len == sizeof("fixed_window") - 1
                && ngx_strncmp(s.data, "fixed_window", s.len) == 0)
            {
                zone->algo = NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW;

            } else if (s.len == sizeof("token_bucket") - 1
                       && ngx_strncmp(s.data, "token_bucket", s.len) == 0)
            {
                zone->algo = NGX_HTTP_RATELIMIT_ALGO_TOKEN_BUCKET;

            } else if (s.len == sizeof("gcra") - 1
                       && ngx_strncmp(s.data, "gcra", s.len) == 0)
            {
                zone->algo = NGX_HTTP_RATELIMIT_ALGO_GCRA;

            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid algo \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\"",
                           &value[i]);
        return NGX_CONF_ERROR;
    }

    if (!has_key) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit_zone \"%V\" requires \"key=\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    if (has_rate && (has_requests || has_period)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit_zone \"%V\": \"rate=\" is mutually "
                           "exclusive with \"requests=\"/\"period=\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    if (!has_rate && !(has_requests && has_period)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit_zone \"%V\" requires \"rate=\" or both "
                           "\"requests=\" and \"period=\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    zone->requests = requests;
    zone->period = period;
    zone->burst = burst;

    return NGX_CONF_OK;
}

static char *
ngx_http_ratelimit(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_ratelimit_loc_conf_t *rlcf = conf;

    ngx_str_t *value;
    ngx_int_t burst, quantity;
    ngx_uint_t i;

    if (rlcf->zone_name.len) {
        return "is duplicate";
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            rlcf->zone_name.len = value[i].len - 5;
            rlcf->zone_name.data = value[i].data + 5;

            if (rlcf->zone_name.len == 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone name \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            rlcf->burst = burst;
            continue;
        }

        if (ngx_strncmp(value[i].data, "quantity=", 9) == 0) {

            quantity = ngx_atoi(value[i].data + 9, value[i].len - 9);
            if (quantity < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid quantity value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            rlcf->quantity = quantity;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\"",
                           &value[i]);
        return NGX_CONF_ERROR;
    }

    if (rlcf->zone_name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"ratelimit\" requires \"zone=\"");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_ratelimit_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_ratelimit_loc_conf_t *rlcf = conf;

    ngx_str_t *value;
    ngx_uint_t n;
    ngx_url_t url;

    ngx_http_compile_complex_value_t ccv;

    if (rlcf->upstream.upstream) {
        return "is duplicate";
    }

    value = cf->args->elts;

    n = ngx_http_script_variables_count(&value[1]);
    if (n) {
        rlcf->complex_target =
            ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));

        if (rlcf->complex_target == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
        ccv.cf = cf;
        ccv.value = &value[1];
        ccv.complex_value = rlcf->complex_target;

        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    rlcf->complex_target = NULL;

    ngx_memzero(&url, sizeof(ngx_url_t));

    url.url = value[1];
    url.no_resolve = 1;

    rlcf->upstream.upstream = ngx_http_upstream_add(cf, &url, 0);
    if (rlcf->upstream.upstream == NULL) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_ratelimit_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt *h;
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_ratelimit_main_conf_t *rmcf;
    ngx_http_variable_t *var;
    ngx_str_t name = ngx_string("ratelimit_done");

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);

    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_ratelimit_handler;

    /* Reserve a per-request variable slot for the one-shot "already decided"
     * marker. The handler writes/reads the slot directly via
     * r->main->variables[done_index]; the slot survives the r->ctx wipe an
     * internal redirect does, letting the limiter run exactly once per main
     * request. The variable is registered only so the reserved index resolves
     * (an indexed variable with no definition is rejected as "unknown"); its
     * get handler is never relied upon by the limiter. */
    var = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_ratelimit_done_variable;

    rmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_ratelimit_module);

    rmcf->done_index = ngx_http_get_variable_index(cf, &name);
    if (rmcf->done_index == NGX_ERROR) {
        return NGX_ERROR;
    }

    /* Cache the script SHA1 for EVALSHA before any request runs. */
    ngx_http_ratelimit_script_init();

    return NGX_OK;
}

/* Get handler for the internal "ratelimit_done" marker variable. The limiter
 * reads/writes the slot directly (r->main->variables[done_index]); this only
 * exists so the reserved index has a definition. An explicit "$ratelimit_done"
 * reference before the limiter runs resolves to nothing. */
static ngx_int_t
ngx_http_ratelimit_done_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    v->not_found = 1;

    return NGX_OK;
}
