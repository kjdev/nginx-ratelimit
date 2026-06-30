# Directives

Full reference for `ngx_http_ratelimit_module`. See the
[README](../README.md) for an overview and a directive summary table, and
[EXAMPLES.md](EXAMPLES.md) for configuration recipes.

- [`ratelimit_zone`](#ratelimit_zone) — define a named rate (no shared memory)
- [`ratelimit`](#ratelimit) — apply a zone to the current context
- [`ratelimit_pass`](#ratelimit_pass) — the Redis upstream to query
- [`ratelimit_prefix`](#ratelimit_prefix) — prefix to namespace counters
- [`ratelimit_quantity`](#ratelimit_quantity) — units consumed (`0` peeks)
- [`ratelimit_password`](#ratelimit_password) — `AUTH` before each rate check
- [`ratelimit_database`](#ratelimit_database) — `SELECT` before each rate check
- [`ratelimit_headers`](#ratelimit_headers) — emit `X-RateLimit-*` when allowed
- [`ratelimit_status`](#ratelimit_status) — status for a rejected request
- [`ratelimit_on_error`](#ratelimit_on_error) — fail-closed or fail-open when Redis is unreachable
- [`ratelimit_log_level`](#ratelimit_log_level) — log level for the exceeded message
- [`ratelimit_buffer_size`](#ratelimit_buffer_size) — Redis reply buffer size
- [`ratelimit_connect_timeout` / `ratelimit_send_timeout` / `ratelimit_read_timeout`](#ratelimit_connect_timeout--ratelimit_send_timeout--ratelimit_read_timeout) — Redis timeouts

See also [Algorithms](#algorithms) and [Response headers](#response-headers).

## `ratelimit_zone`

```
Syntax:  ratelimit_zone <name> key=<var> (rate=<N>r/<s|m|h> | requests=<N> period=<time>)
                         [burst=<N>]
                         [algo=fixed_window|token_bucket|gcra|sliding_window|custom]
                         [script=<path>];
Default: —
Context: http
```

Defines a named rate. Unlike `limit_req_zone` it allocates **no** shared memory
— the size parameter is intentionally absent because the counter lives in Redis.

- `key=` — the rate-limit key, any NGINX variable or expression
  (e.g. `$binary_remote_addr`, `$jwt_claim_sub`, `$http_x_api_key`).
- `rate=` — shorthand for a per-second/minute/hour rate. `100r/m` means
  `requests=100 period=60s`. Mutually exclusive with `requests=`/`period=`.
- `requests=` + `period=` — set the rate explicitly; `period` accepts NGINX time
  units (`30s`, `5m`, `1h`).
- `burst=` — extra headroom above `requests` (default `0`). Its exact meaning is
  per algorithm (see [Algorithms](#algorithms)).
- `algo=` — `fixed_window` (default), `token_bucket`, `gcra`,
  `sliding_window`, or `custom`.
- `script=` — path to a user-supplied Lua script; **required** with
  `algo=custom` and rejected otherwise. See
  [Custom scripts](CUSTOM_SCRIPT.md).

## `ratelimit`

```
Syntax:  ratelimit zone=<name> [burst=<N>] [quantity=<N>];
Default: —
Context: http, server, location
```

Applies a zone to the current context. `burst=` and `quantity=` override the
zone default / `ratelimit_quantity` for this location.

## `ratelimit_pass`

```
Syntax:  ratelimit_pass <upstream | host:port | $var>;
Default: —
Context: http, server, location
```

The Redis upstream to query. Use a named `upstream { … }` block (recommended, so
`keepalive` can be set) or an address. Variables are allowed and resolve at
request time to a named upstream.

## `ratelimit_prefix`

```
Syntax:  ratelimit_prefix <string>;
Default: ""
Context: http, server, location
```

Prepended to the key as `<prefix>_<key>` to namespace counters that share a
Redis (e.g. different locations or environments).

## `ratelimit_quantity`

```
Syntax:  ratelimit_quantity <number>;
Default: 1
Context: http, server, location
```

How many units this request consumes. `0` **peeks**: it reports the current
state in the response headers without consuming anything. A quantity larger than
the limit is rejected up front.

## `ratelimit_password`

```
Syntax:  ratelimit_password <password>;
Default: —
Context: http, server, location
```

Sends `AUTH <password>` ahead of every rate check (pipelined with the limit
command, no extra round trip). Required by most managed Redis. Sending it on
each request keeps the connection's auth identity correct even when a pooled
keepalive connection is reused across locations. The password is wiped from the
request buffer after use.

## `ratelimit_database`

```
Syntax:  ratelimit_database <number>;
Default: — (database 0)
Context: http, server, location
```

Sends `SELECT <number>` ahead of every rate check (pipelined with the limit
command, no extra round trip). Sending it on each request keeps counts isolated
per database even when a pooled keepalive connection is reused across locations
that select different databases.

## `ratelimit_headers`

```
Syntax:  ratelimit_headers on | off;
Default: off
Context: http, server, location
```

When `on`, emit the `X-RateLimit-*` headers on **allowed** responses too. The
headers are always emitted on a rejected (limited) response regardless of this
setting.

## `ratelimit_status`

```
Syntax:  ratelimit_status <code>;
Default: 429
Context: http, server, location
```

HTTP status for a rejected request (`400`–`599`).

## `ratelimit_on_error`

```
Syntax:  ratelimit_on_error deny | allow;
Default: deny
Context: http, server, location
```

Behaviour when the limiter cannot reach a verdict because Redis is unreachable
(connect refused, timeout, connection dropped).

- `deny` (default) — fail closed. The request is rejected with the Redis
  transport status (`502`/`503`/`504`). This is the safe default: a Redis
  outage cannot be used to bypass the limit.
- `allow` — fail open. The request is let through unlimited and a `warn` line
  is logged (`redis unavailable ..., failing open`).

Only Redis transport failures honour `allow`. An internal or contract error —
a malformed script reply or a configuration fault, surfaced as `500` — always
fails closed regardless of this setting, so a broken script or setup is never
silently allowed.

## `ratelimit_log_level`

```
Syntax:  ratelimit_log_level info | notice | warn | error;
Default: error
Context: http, server, location
```

Log level for the "rate limit exceeded" message. The key value and Redis
credentials are never logged.

## `ratelimit_buffer_size`

```
Syntax:  ratelimit_buffer_size <size>;
Default: one page (4k/8k)
Context: http, server, location
```

Size of the buffer that holds the Redis reply.

## `ratelimit_connect_timeout` / `ratelimit_send_timeout` / `ratelimit_read_timeout`

```
Syntax:  ratelimit_<phase>_timeout <time>;
Default: 60s
Context: http, server, location
```

Timeouts for connecting to, writing to, and reading from Redis.

## Algorithms

Each runs as a single atomic Lua script. `quantity=0` peeks without
mutating state in every algorithm.

| `algo`           | `limit` (X-RateLimit-Limit)   | `burst` meaning | state in Redis |
|------------------|-------------------------------|-----------------|----------------|
| `fixed_window`   | `requests + burst`            | extra requests within the window | counter + TTL |
| `token_bucket`   | `requests + burst` (capacity) | extra bucket capacity | `{tokens, ts}` hash |
| `gcra`           | `burst + 1`                   | burst tolerance | theoretical arrival time |
| `sliding_window` | `requests + burst`            | extra requests within the window | `{c, w, p}` integer hash |
| `custom`         | whatever the script returns   | passed to the script as `ARGV[3]` | whatever the script stores |

- **fixed_window** — `INCR` within a window of `period` seconds; the window
  rolls over via Redis key TTL. Simple and cheap; allows up to `2×limit` across
  a window boundary, which is the classic fixed-window trade-off.
- **token_bucket** — a bucket of `requests + burst` tokens refilling at
  `requests / period` tokens per second; a fresh bucket starts full.
- **gcra** — the generic cell rate algorithm: a smooth `period / requests`
  emission interval with `burst` tolerance. `limit` is `burst + 1`.
- **sliding_window** — a weighted approximation that estimates the rate as
  `prev_count × weight + cur_count`, weighting the previous window's count by
  the fraction still inside the trailing window. State is three integers, as
  light as the fixed window, but it smooths the `2×limit` boundary burst.

Token bucket, GCRA, and sliding window derive the current time from
`redis.call('TIME')`, so the limiter is consistent across NGINX workers and
hosts.

- **custom** — a user-supplied Lua script loaded from `script=`, receiving the
  same `ARGV[1..4]` (`requests`, `period`, `burst`, `quantity`) as token bucket
  and returning the same 5-integer reply. For arbitrary rate-control logic the
  built-ins don't cover. See [Custom scripts](CUSTOM_SCRIPT.md) for the contract
  and constraints.

## Response headers

| Header | Meaning |
|--------|---------|
| `X-RateLimit-Limit` | the effective limit |
| `X-RateLimit-Remaining` | requests/tokens left |
| `X-RateLimit-Reset` | seconds until the limit resets / the bucket refills |
| `Retry-After` | seconds to wait; sent only on a rejected response |
