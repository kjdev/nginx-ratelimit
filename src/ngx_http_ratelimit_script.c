#include "ngx_http_ratelimit_script.h"
#include <ngx_sha1.h>

/*
 * Each algorithm is a self-contained Lua script performing an atomic
 * read-modify-write in a single Redis call. Every script returns the fixed
 * 5-integer contract {status, limit, remaining, retry_after, reset}: status 0
 * allowed / 1 limited, retry_after -1 when allowed (otherwise seconds, always
 * >= 1), remaining/reset non-negative seconds. The contract keeps reply.c
 * unchanged across algorithms.
 */

/*
 * Fixed-window counter.
 *
 *   KEYS[1]  counter key
 *   ARGV[1]  limit     max requests allowed per window
 *   ARGV[2]  window    window length in seconds
 *   ARGV[3]  quantity  increment for this request; 0 peeks without consuming
 *
 * reset is the seconds until the window rolls over.
 */
static ngx_str_t ngx_http_ratelimit_fixed_window_lua = ngx_string(
    "local limit = tonumber(ARGV[1])\n"
    "local window = tonumber(ARGV[2])\n"
    "local quantity = tonumber(ARGV[3])\n"
    "local current = tonumber(redis.call('GET', KEYS[1]) or '0')\n"
    "if quantity == 0 then\n"
    "  local ttl = redis.call('TTL', KEYS[1])\n"
    "  if ttl < 0 then ttl = 0 end\n"
    "  local remaining = limit - current\n"
    "  if remaining < 0 then remaining = 0 end\n"
    "  local status = 0\n"
    "  if current >= limit then status = 1 end\n"
    "  return {status, limit, remaining, -1, ttl}\n"
    "end\n"
    "if current + quantity > limit then\n"
    "  local ttl = redis.call('TTL', KEYS[1])\n"
    "  if ttl < 0 then ttl = window end\n"
    "  local remaining = limit - current\n"
    "  if remaining < 0 then remaining = 0 end\n"
    "  local retry_after = ttl\n"
    "  if retry_after < 1 then retry_after = 1 end\n"
    "  return {1, limit, remaining, retry_after, ttl}\n"
    "end\n"
    "local newval = redis.call('INCRBY', KEYS[1], quantity)\n"
    "local ttl\n"
    "if newval == quantity then\n"
    "  redis.call('EXPIRE', KEYS[1], window)\n"
    "  ttl = window\n"
    "else\n"
    "  ttl = redis.call('TTL', KEYS[1])\n"
    "  if ttl < 0 then\n"
    "    redis.call('EXPIRE', KEYS[1], window)\n"
    "    ttl = window\n"
    "  end\n"
    "end\n"
    "local remaining = limit - newval\n"
    "if remaining < 0 then remaining = 0 end\n"
    "return {0, limit, remaining, -1, ttl}\n");

/*
 * Token bucket. State is a hash {tokens, ts} refilled lazily on each call.
 *
 *   KEYS[1]  bucket key
 *   ARGV[1]  requests  sustained requests per period (sets the refill rate)
 *   ARGV[2]  period    period in seconds
 *   ARGV[3]  burst     extra capacity beyond the sustained requests
 *   ARGV[4]  quantity  tokens to consume; 0 peeks without consuming
 *
 * capacity = requests + burst, refill rate = requests / period tokens/sec.
 * The server clock (redis TIME) drives refill so the limiter is consistent
 * across nginx workers. limit = capacity, remaining = floor(tokens),
 * reset = seconds until the bucket is full again.
 */
static ngx_str_t ngx_http_ratelimit_token_bucket_lua = ngx_string(
    "local requests = tonumber(ARGV[1])\n"
    "local period = tonumber(ARGV[2])\n"
    "local burst = tonumber(ARGV[3])\n"
    "local quantity = tonumber(ARGV[4])\n"
    "local capacity = requests + burst\n"
    "local rate = requests / period\n"
    "local t = redis.call('TIME')\n"
    "local now = tonumber(t[1]) + tonumber(t[2]) / 1000000\n"
    "local data = redis.call('HMGET', KEYS[1], 'tokens', 'ts')\n"
    "local tokens = tonumber(data[1])\n"
    "local ts = tonumber(data[2])\n"
    "if tokens == nil then\n"
    "  tokens = capacity\n"
    "  ts = now\n"
    "end\n"
    "local elapsed = now - ts\n"
    "if elapsed > 0 then\n"
    "  tokens = tokens + elapsed * rate\n"
    "  if tokens > capacity then tokens = capacity end\n"
    "  ts = now\n"
    "end\n"
    "local ttl = math.ceil(capacity / rate)\n"
    "if ttl < 1 then ttl = 1 end\n"
    "local reset = math.ceil((capacity - tokens) / rate)\n"
    "if reset < 0 then reset = 0 end\n"
    "if quantity == 0 then\n"
    "  local status = 0\n"
    "  if tokens < 1 then status = 1 end\n"
    "  return {status, capacity, math.floor(tokens), -1, reset}\n"
    "end\n"
    "if tokens >= quantity then\n"
    "  tokens = tokens - quantity\n"
    "  redis.call('HSET', KEYS[1], 'tokens', string.format('%.6f', tokens),"
    " 'ts', string.format('%.6f', ts))\n"
    "  redis.call('EXPIRE', KEYS[1], ttl)\n"
    "  reset = math.ceil((capacity - tokens) / rate)\n"
    "  if reset < 0 then reset = 0 end\n"
    "  return {0, capacity, math.floor(tokens), -1, reset}\n"
    "end\n"
    "redis.call('HSET', KEYS[1], 'tokens', string.format('%.6f', tokens),"
    " 'ts', string.format('%.6f', ts))\n"
    "redis.call('EXPIRE', KEYS[1], ttl)\n"
    "local retry_after = math.ceil((quantity - tokens) / rate)\n"
    "if retry_after < 1 then retry_after = 1 end\n"
    "return {1, capacity, math.floor(tokens), retry_after, reset}\n");

/*
 * GCRA (generic cell rate algorithm). State is the theoretical arrival time
 * (TAT) stored as a single value.
 *
 *   KEYS[1]  TAT key
 *   ARGV[1]  requests  requests per period (sets the emission interval)
 *   ARGV[2]  period    period in seconds
 *   ARGV[3]  burst     burst tolerance; allows burst extra requests at once
 *   ARGV[4]  quantity  units to consume; 0 peeks without consuming
 *
 * emission interval T = period / requests, burst tolerance = T * (burst + 1).
 * limit = burst + 1 (the GCRA burst capacity), remaining = floor(diff / T),
 * reset = seconds until the limiter fully drains.
 */
static ngx_str_t ngx_http_ratelimit_gcra_lua = ngx_string(
    "local requests = tonumber(ARGV[1])\n"
    "local period = tonumber(ARGV[2])\n"
    "local burst = tonumber(ARGV[3])\n"
    "local quantity = tonumber(ARGV[4])\n"
    "local emission = period / requests\n"
    "local t = redis.call('TIME')\n"
    "local now = tonumber(t[1]) + tonumber(t[2]) / 1000000\n"
    "local tat = tonumber(redis.call('GET', KEYS[1]))\n"
    "if tat == nil then tat = now end\n"
    "if tat < now then tat = now end\n"
    "local limit = burst + 1\n"
    "local horizon = emission * (burst + 1)\n"
    "if quantity == 0 then\n"
    "  -- peek: report the current state without advancing the TAT, so a\n"
    "  -- fresh limiter reads full capacity and reset 0, like the other algos\n"
    "  local avail = math.floor((now - (tat - horizon)) / emission)\n"
    "  if avail > limit then avail = limit end\n"
    "  local status = 0\n"
    "  if avail < 1 then status = 1 end\n"
    "  local reset = math.ceil(tat - now)\n"
    "  if reset < 0 then reset = 0 end\n"
    "  return {status, limit, avail, -1, reset}\n"
    "end\n"
    "local new_tat = tat + emission * quantity\n"
    "local allow_at = new_tat - horizon\n"
    "local diff = now - allow_at\n"
    "if diff < 0 then\n"
    "  local retry_after = math.ceil(-diff)\n"
    "  if retry_after < 1 then retry_after = 1 end\n"
    "  local reset = math.ceil(tat - now)\n"
    "  if reset < 0 then reset = 0 end\n"
    "  return {1, limit, 0, retry_after, reset}\n"
    "end\n"
    "local remaining = math.floor(diff / emission)\n"
    "local reset = math.ceil(new_tat - now)\n"
    "if reset < 0 then reset = 0 end\n"
    "redis.call('SET', KEYS[1], string.format('%.6f', new_tat))\n"
    "local ttl = math.ceil(new_tat - now)\n"
    "if ttl < 1 then ttl = 1 end\n"
    "redis.call('EXPIRE', KEYS[1], ttl)\n"
    "return {0, limit, remaining, -1, reset}\n");

/*
 * Sliding window counter. State is a hash {c, w, p}: the current window's
 * count, the current window index, and the previous window's count. All
 * integers, so storage stays as light as the fixed window.
 *
 *   KEYS[1]  state key
 *   ARGV[1]  limit     max requests allowed per window (requests + burst)
 *   ARGV[2]  window    window length in seconds
 *   ARGV[3]  quantity  increment for this request; 0 peeks without consuming
 *
 * The rate is approximated as prev * weight + cur, where weight is the
 * fraction of the previous window still inside the trailing window. This
 * smooths the boundary burst the fixed window allows. The server clock
 * (redis TIME) drives the window index so the limiter is consistent across
 * nginx workers. limit = requests + burst, remaining = floor(limit - rate),
 * reset = seconds until the current window rolls over.
 */
static ngx_str_t ngx_http_ratelimit_sliding_window_lua = ngx_string(
    "local limit = tonumber(ARGV[1])\n"
    "local window = tonumber(ARGV[2])\n"
    "local quantity = tonumber(ARGV[3])\n"
    "local t = redis.call('TIME')\n"
    "local now = tonumber(t[1]) + tonumber(t[2]) / 1000000\n"
    "local cur_win = math.floor(now / window)\n"
    "local elapsed = now - cur_win * window\n"
    "local weight = (window - elapsed) / window\n"
    "local data = redis.call('HMGET', KEYS[1], 'c', 'w', 'p')\n"
    "local c = tonumber(data[1])\n"
    "local w = tonumber(data[2])\n"
    "local p = tonumber(data[3])\n"
    "if c == nil then\n"
    "  c = 0\n"
    "  w = cur_win\n"
    "  p = 0\n"
    "end\n"
    "if w == cur_win - 1 then\n"
    "  p = c\n"
    "  c = 0\n"
    "  w = cur_win\n"
    "elseif w ~= cur_win then\n"
    "  p = 0\n"
    "  c = 0\n"
    "  w = cur_win\n"
    "end\n"
    "local est = p * weight + c\n"
    "local remaining = math.floor(limit - est)\n"
    "if remaining < 0 then remaining = 0 end\n"
    "local win_remaining = math.ceil(window - elapsed)\n"
    "if win_remaining < 0 then win_remaining = 0 end\n"
    "if quantity == 0 then\n"
    "  local status = 0\n"
    "  if est >= limit then status = 1 end\n"
    "  local reset = 0\n"
    "  if c > 0 or p > 0 then reset = win_remaining end\n"
    "  return {status, limit, remaining, -1, reset}\n"
    "end\n"
    "if est + quantity > limit then\n"
    "  local headroom = limit - quantity - c\n"
    "  local retry_after\n"
    "  if p > 0 and headroom >= 0 then\n"
    "    retry_after = math.ceil((window - elapsed) - headroom * window / p)\n"
    "  else\n"
    "    retry_after = win_remaining\n"
    "  end\n"
    "  if retry_after < 1 then retry_after = 1 end\n"
    "  return {1, limit, remaining, retry_after, win_remaining}\n"
    "end\n"
    "c = c + quantity\n"
    "redis.call('HSET', KEYS[1], 'c', c, 'w', w, 'p', p)\n"
    "redis.call('EXPIRE', KEYS[1], window * 2)\n"
    "est = p * weight + c\n"
    "remaining = math.floor(limit - est)\n"
    "if remaining < 0 then remaining = 0 end\n"
    "return {0, limit, remaining, -1, win_remaining}\n");

typedef struct {
    ngx_str_t  body;
    ngx_str_t  sha;
    u_char     sha_buf[40];
} ngx_http_ratelimit_script_t;

/* Indexed by ngx_http_ratelimit_algo_t. */
static ngx_http_ratelimit_script_t ngx_http_ratelimit_scripts[] = {
    { ngx_null_string, ngx_null_string, { 0 } }, /* fixed window */
    { ngx_null_string, ngx_null_string, { 0 } }, /* token bucket */
    { ngx_null_string, ngx_null_string, { 0 } }, /* gcra */
    { ngx_null_string, ngx_null_string, { 0 } }  /* sliding window */
};

void
ngx_http_ratelimit_script_sha1(ngx_str_t *body, u_char *sha_buf, ngx_str_t *sha)
{
    ngx_sha1_t sha1;
    u_char digest[20];

    ngx_sha1_init(&sha1);
    ngx_sha1_update(&sha1, body->data, body->len);
    ngx_sha1_final(digest, &sha1);

    ngx_hex_dump(sha_buf, digest, sizeof(digest));

    sha->data = sha_buf;
    sha->len = 40;
}

void
ngx_http_ratelimit_script_init(void)
{
    ngx_http_ratelimit_script_t *s;
    ngx_uint_t i, n;

    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW].body =
        ngx_http_ratelimit_fixed_window_lua;
    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_TOKEN_BUCKET].body =
        ngx_http_ratelimit_token_bucket_lua;
    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_GCRA].body =
        ngx_http_ratelimit_gcra_lua;
    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_SLIDING_WINDOW].body =
        ngx_http_ratelimit_sliding_window_lua;

    n = sizeof(ngx_http_ratelimit_scripts)
        / sizeof(ngx_http_ratelimit_scripts[0]);

    for (i = 0; i < n; i++) {
        s = &ngx_http_ratelimit_scripts[i];
        ngx_http_ratelimit_script_sha1(&s->body, s->sha_buf, &s->sha);
    }
}

ngx_int_t
ngx_http_ratelimit_script_read_file(ngx_conf_t *cf, ngx_str_t *path,
    ngx_str_t *out)
{
    u_char *buf;
    size_t size;
    ssize_t n;
    ngx_str_t name;
    ngx_file_t file;
    ngx_file_info_t fi;

    /* Resolve a relative path against the nginx prefix, mirroring how
     * directives such as ssl_certificate locate their files. */
    name = *path;
    if (ngx_conf_full_name(cf->cycle, &name, 0) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = name;
    file.log = cf->log;

    file.fd = ngx_open_file(name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "ratelimit: cannot open script \"%V\"", &name);
        return NGX_ERROR;
    }

    if (ngx_fd_info(file.fd, &fi) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "ratelimit: " ngx_fd_info_n " \"%V\" failed", &name);
        goto failed;
    }

    if (!ngx_is_file(&fi)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit: script \"%V\" is not a regular file",
                           &name);
        goto failed;
    }

    size = (size_t) ngx_file_size(&fi);

    if (size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit: script \"%V\" is empty", &name);
        goto failed;
    }

    if (size > NGX_HTTP_RATELIMIT_MAX_SCRIPT_LEN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit: script \"%V\" is %uz bytes, "
                           "more than the %d byte limit",
                           &name, size, NGX_HTTP_RATELIMIT_MAX_SCRIPT_LEN);
        goto failed;
    }

    buf = ngx_palloc(cf->pool, size);
    if (buf == NULL) {
        goto failed;
    }

    n = ngx_read_file(&file, buf, size, 0);
    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, ngx_errno,
                           "ratelimit: " ngx_read_file_n " \"%V\" failed",
                           &name);
        goto failed;
    }

    if ((size_t) n != size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ratelimit: script \"%V\" read %z of %uz bytes",
                           &name, n, size);
        goto failed;
    }

    ngx_close_file(file.fd);

    out->data = buf;
    out->len = size;

    return NGX_OK;

failed:

    ngx_close_file(file.fd);

    return NGX_ERROR;
}

ngx_str_t *
ngx_http_ratelimit_script_body(ngx_http_ratelimit_algo_t algo)
{
    return &ngx_http_ratelimit_scripts[algo].body;
}

ngx_str_t *
ngx_http_ratelimit_script_sha(ngx_http_ratelimit_algo_t algo)
{
    return &ngx_http_ratelimit_scripts[algo].sha;
}
