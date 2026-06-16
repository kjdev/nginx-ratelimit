#include "ngx_http_ratelimit_script.h"
#include <ngx_sha1.h>

/*
 * Fixed-window counter. Atomic read-modify-write in a single Lua call.
 *
 *   KEYS[1]  counter key
 *   ARGV[1]  limit     max requests allowed per window
 *   ARGV[2]  window    window length in seconds
 *   ARGV[3]  quantity  increment for this request; 0 peeks without consuming
 *
 * Returns the fixed 5-integer contract {status, limit, remaining,
 * retry_after, reset}: status 0 allowed / 1 limited, retry_after -1 when
 * allowed, reset is the seconds until the window rolls over.
 */
static ngx_str_t ngx_http_ratelimit_script_lua = ngx_string(
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

static u_char ngx_http_ratelimit_script_sha_buf[40];
static ngx_str_t ngx_http_ratelimit_script_sha_str = {
    0, ngx_http_ratelimit_script_sha_buf
};

void
ngx_http_ratelimit_script_init(void)
{
    ngx_sha1_t sha1;
    u_char digest[20];

    ngx_sha1_init(&sha1);
    ngx_sha1_update(&sha1, ngx_http_ratelimit_script_lua.data,
                    ngx_http_ratelimit_script_lua.len);
    ngx_sha1_final(digest, &sha1);

    ngx_hex_dump(ngx_http_ratelimit_script_sha_buf, digest, sizeof(digest));

    ngx_http_ratelimit_script_sha_str.len =
        sizeof(ngx_http_ratelimit_script_sha_buf);
}

ngx_str_t *
ngx_http_ratelimit_script_body(void)
{
    return &ngx_http_ratelimit_script_lua;
}

ngx_str_t *
ngx_http_ratelimit_script_sha(void)
{
    return &ngx_http_ratelimit_script_sha_str;
}
