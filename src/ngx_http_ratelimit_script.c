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
    "  if current > limit then status = 1 end\n"
    "  return {status, limit, remaining, -1, ttl}\n"
    "end\n"
    "if current + quantity > limit then\n"
    "  local ttl = redis.call('TTL', KEYS[1])\n"
    "  if ttl < 0 then ttl = window end\n"
    "  local remaining = limit - current\n"
    "  if remaining < 0 then remaining = 0 end\n"
    "  return {1, limit, remaining, ttl, ttl}\n"
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
    "local q = quantity\n"
    "if q == 0 then q = 1 end\n"
    "local new_tat = tat + emission * q\n"
    "local allow_at = new_tat - emission * (burst + 1)\n"
    "local diff = now - allow_at\n"
    "local limit = burst + 1\n"
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
    "if quantity == 0 then\n"
    "  return {0, limit, remaining, -1, reset}\n"
    "end\n"
    "redis.call('SET', KEYS[1], string.format('%.6f', new_tat))\n"
    "local ttl = math.ceil(new_tat - now)\n"
    "if ttl < 1 then ttl = 1 end\n"
    "redis.call('EXPIRE', KEYS[1], ttl)\n"
    "return {0, limit, remaining, -1, reset}\n");

typedef struct {
    ngx_str_t  body;
    ngx_str_t  sha;
    u_char     sha_buf[40];
} ngx_http_ratelimit_script_t;

/* Indexed by ngx_http_ratelimit_algo_t. */
static ngx_http_ratelimit_script_t ngx_http_ratelimit_scripts[] = {
    { ngx_null_string, ngx_null_string, { 0 } }, /* fixed window */
    { ngx_null_string, ngx_null_string, { 0 } }, /* token bucket */
    { ngx_null_string, ngx_null_string, { 0 } }  /* gcra */
};

void
ngx_http_ratelimit_script_init(void)
{
    ngx_sha1_t sha1;
    u_char digest[20];
    ngx_http_ratelimit_script_t *s;
    ngx_uint_t i, n;

    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_FIXED_WINDOW].body =
        ngx_http_ratelimit_fixed_window_lua;
    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_TOKEN_BUCKET].body =
        ngx_http_ratelimit_token_bucket_lua;
    ngx_http_ratelimit_scripts[NGX_HTTP_RATELIMIT_ALGO_GCRA].body =
        ngx_http_ratelimit_gcra_lua;

    n = sizeof(ngx_http_ratelimit_scripts)
        / sizeof(ngx_http_ratelimit_scripts[0]);

    for (i = 0; i < n; i++) {
        s = &ngx_http_ratelimit_scripts[i];

        ngx_sha1_init(&sha1);
        ngx_sha1_update(&sha1, s->body.data, s->body.len);
        ngx_sha1_final(digest, &sha1);

        ngx_hex_dump(s->sha_buf, digest, sizeof(digest));

        s->sha.data = s->sha_buf;
        s->sha.len = sizeof(s->sha_buf);
    }
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
