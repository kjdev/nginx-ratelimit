# Custom scripts (`algo=custom`)

When the built-in algorithms (`fixed_window`, `token_bucket`, `gcra`,
`sliding_window`) don't fit, you can supply your own Lua script. The module
loads it, runs it atomically in Redis with `EVALSHA`/`EVAL`, and reads back the
same 5-integer reply the built-ins return.

```nginx
ratelimit_zone api key=$http_x_api_key rate=100r/m algo=custom
               script=/etc/nginx/lua/limit.lua;

location /api/ {
    ratelimit zone=api;
    ratelimit_pass redis;
}
```

`script=` is **required** with `algo=custom` and rejected for every other
algorithm. The path is resolved relative to the NGINX prefix when not absolute.

## The contract

Your script is invoked exactly like the token-bucket / GCRA built-ins:

| | |
|----------|------------------------------------------------------------|
| `KEYS[1]` | the rate key (zone `key=`, optionally `ratelimit_prefix`-prefixed) |
| `ARGV[1]` | `requests` — the zone's `requests` (or `rate=` numerator) |
| `ARGV[2]` | `period` — the window/period in seconds |
| `ARGV[3]` | `burst` — effective burst (`ratelimit ... burst=` overrides the zone) |
| `ARGV[4]` | `quantity` — units to consume; **`0` means peek** (report state, consume nothing) |

It **must** return a Redis array of exactly **5 integers**:

```
{ status, limit, remaining, retry_after, reset }
```

| field | meaning | constraints |
|-------|---------|-------------|
| `status` | `0` allowed, `1` limited | exactly `0` or `1` |
| `limit` | value for `X-RateLimit-Limit` | `>= 0` |
| `remaining` | value for `X-RateLimit-Remaining` | `>= 0` |
| `retry_after` | `Retry-After` seconds; `-1` when allowed | `-1`, or an integer `>= 1` |
| `reset` | seconds for `X-RateLimit-Reset` | `>= 0` |

All five must be integers. `redis.call(...)` results that are floats must be
passed through `math.floor` / `math.ceil` before being returned. A reply that
isn't a 5-integer array is rejected **fail-closed** (the request gets a 5xx, it
is never let through) — see [Failure handling](#failure-handling).

## Example: a fixed-window limiter

A minimal custom script equivalent to the built-in fixed window, honouring
`quantity=0` as a peek:

```lua
local limit    = tonumber(ARGV[1]) + tonumber(ARGV[3])  -- requests + burst
local period   = tonumber(ARGV[2])
local quantity = tonumber(ARGV[4])
local current  = tonumber(redis.call('GET', KEYS[1]) or '0')

if quantity == 0 then                       -- peek: report, consume nothing
  local remaining = math.max(limit - current, 0)
  local status = (current >= limit) and 1 or 0
  return {status, limit, remaining, -1, 0}
end

if current + quantity > limit then          -- would exceed: reject
  return {1, limit, 0, period, 0}
end

local newval = redis.call('INCRBY', KEYS[1], quantity)
redis.call('EXPIRE', KEYS[1], period)
return {0, limit, math.max(limit - newval, 0), -1, period}
```

## Time and replication

Reading the current time inside the script must use `redis.call('TIME')`, the
same approach the built-in token bucket / GCRA / sliding window take, so the
limiter stays consistent across NGINX workers and hosts. `TIME` is
non-deterministic; Redis 5+ effects replication (the default on the managed
Redis / Valkey targets this module supports) replicates the resulting writes
rather than the script, so this is safe. Do not pass an NGINX-side clock in.

## Loading, caching, and the size limit

- The script body is read **at config parse time** (`nginx -t` / reload). A
  missing, empty, or oversized file aborts startup with a line-numbered error.
- The body is capped at **64 KiB** (65536 bytes).
- Its SHA1 is computed once at startup; requests use `EVALSHA`, falling back to
  `EVAL` once on a server `NOSCRIPT` (e.g. after a Redis `SCRIPT FLUSH` or
  failover), exactly like the built-ins.
- Changing the file requires a reload to take effect — the body and SHA are
  fixed at load time.

## Failure handling

The reply parser requires the 5-integer contract. Any deviation — a string
reply, the wrong element count, a non-integer, a negative value other than the
`-1` allowed sentinel, or a Redis error — fails the request **closed**: the
limiter does not let the request through. A script error (`redis.error_reply`,
a runtime error, a `redis.call` failure) surfaces the same way. Write and test
your script accordingly; a buggy script rejects traffic rather than leaking it.

## See also

- [DIRECTIVES.md](DIRECTIVES.md) — `ratelimit_zone` reference and the built-in
  algorithms.
- [EXAMPLES.md](EXAMPLES.md) — configuration examples.
