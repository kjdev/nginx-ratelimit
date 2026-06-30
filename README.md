# NGINX ratelimit Module

A distributed rate-limiting module for stock NGINX, backed by **managed Redis**.

`ngx_http_ratelimit_module` enforces request rate limits in the `PREACCESS`
phase using a counter that lives in Redis, so the limit is shared across every
NGINX instance pointed at the same Redis. The rate-limit key is any NGINX
variable, which lets you limit per IP, per API key, or per authenticated
identity supplied by another module (e.g. `auth_jwt`).

It is a pure C dynamic module: **no OpenResty / `ngx_lua`, and no Redis
server-side extension**. The atomic counter is a Lua script run with
`EVALSHA`/`EVAL`, which every managed Redis (ElastiCache, Memorystore, Upstash,
…) supports out of the box. This is the gap it fills — existing distributed
solutions require either OpenResty (`lua-resty-*`) or a loadable Redis module
(`weserv/rate-limit-nginx-module`'s `RATER.LIMIT`, redis-cell), neither of
which is available on managed Redis.

Redis is reached over native `ngx_http_upstream` (fully asynchronous, with
connection keepalive); there is no `hiredis` dependency and RESP is encoded and
decoded in-module.

This module is a fork of
[weserv/rate-limit-nginx-module](https://github.com/weserv/rate-limit-nginx-module)
(BSD-3-Clause); see [`NOTICE`](NOTICE) for provenance and the list of changes.

## Status & scope

- Backend is **Redis only**. There is no shared-memory backend; for a
  single-node limit use the stock `limit_req`. This module is for the
  distributed case.
- Algorithms: fixed window, token bucket, GCRA, sliding window, and `custom`
  (bring your own Lua script — see [docs/CUSTOM_SCRIPT.md](docs/CUSTOM_SCRIPT.md)).
- It does **not** extract auth claims. Give it an NGINX variable as the key and
  delegate authentication to `auth_jwt` / `auth_request` / an API-key module.
- Redis Cluster (hash-tag / slot routing) is out of scope for now.

## Install

Built against the NGINX source with `--add-dynamic-module`, then loaded with
`load_module`. Requires NGINX with dynamic-module support and Redis/Valkey 5.0+
(a stock server — nothing to load on it). See [docs/INSTALL.md](docs/INSTALL.md)
for the full build steps.

## Quick start

```nginx
http {
    # A Redis upstream. keepalive pools the connection so the per-request
    # TCP connect is amortised (AUTH/SELECT, if configured, are pipelined
    # with each rate check and cost no extra round trip).
    upstream redis {
        server 127.0.0.1:6379;
        keepalive 32;
    }

    # Define a named rate. No shared memory is allocated; the counter is in Redis.
    ratelimit_zone api key=$binary_remote_addr rate=100r/m;

    server {
        location /api/ {
            ratelimit zone=api;
            ratelimit_pass redis;
            ratelimit_headers on;

            proxy_pass http://backend;
        }
    }
}
```

The 101st request from an address within a minute receives `429 Too Many
Requests`. More recipes — including using a JWT claim as the key — are in
[docs/EXAMPLES.md](docs/EXAMPLES.md).

## How it works

On each request the module computes the key, runs the zone's Lua script against
Redis with `EVALSHA <sha> 1 <key> <args…>`, and either allows the request
(phase returns, request continues) or rejects it with the configured status.
The script returns a fixed 5-integer contract
`{status, limit, remaining, retry_after, reset}`, so the same response parser
serves all algorithms.

- The script SHA is cached at startup. The first call after a Redis restart (or
  `SCRIPT FLUSH`) gets `-NOSCRIPT`; the module transparently resends the request
  once with `EVAL <script>` on the same connection and keeps using `EVALSHA`
  afterwards.
- `AUTH` / `SELECT` (when configured) are **pipelined ahead of every rate
  check** (alongside the `EVALSHA`, no extra round trip), so the connection's
  auth identity and selected database stay correct even on a reused keepalive
  connection.
- If the key variable resolves to an empty value the request is **not limited**
  (the handler declines and the request proceeds).
- If Redis is unreachable or returns an error, the behaviour is controlled by
  `ratelimit_on_error`: by default (`deny`) the request **fails closed**
  (rejected with `503`), not open; set `allow` to **fail open** and let the
  request proceed.

## Directives

Summary; full Syntax/Default/Context and per-algorithm semantics are in
[docs/DIRECTIVES.md](docs/DIRECTIVES.md).

| Directive | Purpose |
|-----------|---------|
| `ratelimit_zone` | Define a named rate (`key=`, `rate=`/`requests=`+`period=`, `burst=`, `algo=`, `script=` for `algo=custom`); allocates no shared memory. |
| `ratelimit` | Apply a zone to the current context. |
| `ratelimit_pass` | The Redis upstream (or address / variable) to query. |
| `ratelimit_prefix` | Prefix prepended to the key to namespace counters. |
| `ratelimit_quantity` | Units this request consumes; `0` peeks without consuming. |
| `ratelimit_password` | `AUTH <password>` on a newly opened connection. |
| `ratelimit_database` | `SELECT <number>` on a newly opened connection. |
| `ratelimit_headers` | Emit `X-RateLimit-*` on allowed responses too. |
| `ratelimit_status` | HTTP status for a rejected request (default `429`). |
| `ratelimit_on_error` | `deny` (fail closed, default) or `allow` (fail open) when Redis is unreachable. |
| `ratelimit_log_level` | Log level for the "rate limit exceeded" message. |
| `ratelimit_buffer_size` | Size of the buffer holding the Redis reply. |
| `ratelimit_connect_timeout` / `_send_timeout` / `_read_timeout` | Redis connect / write / read timeouts. |

## Managed Redis compatibility

The module targets managed Redis directly:

- **No server-side module.** The counter is a Lua script (`EVALSHA`/`EVAL`),
  which stock Redis/Valkey and every managed offering support. There is nothing
  to load on the server.
- **AUTH / TLS / database.** `ratelimit_password` and `ratelimit_database` cover
  the credentials managed Redis requires. Terminate TLS to the managed endpoint
  with an `upstream` to its host/port as usual; AUTH/SELECT are pipelined ahead
  of every rate check (no extra round trip), and `keepalive` amortises the
  connection setup.
- **Connection drop.** If Redis is unreachable the request fails closed
  (`503`) by default (`ratelimit_on_error deny`), so an outage cannot silently
  disable limiting; set `ratelimit_on_error allow` to fail open instead.

How this is verified in this repo:

- `t/ratelimit_auth.t` — AUTH + SELECT against a `requirepass` server, and a
  wrong password failing closed.
- `t/ratelimit_redis_down.t` — an unreachable Redis yields `503`, never a silent
  pass.
- `t/run-integration.sh` — window rollover, concurrency atomicity, and the
  `NOSCRIPT` → `EVAL` fallback against a **stock** Valkey (no module loaded).

## Benchmarks

[`benchmark/`](benchmark/) holds two self-contained load tests:
`run-comparison.sh` compares this module against stock `limit_req` and the weserv
fork on a single node, in both an allow scenario (per-request decision overhead)
and a reject scenario (reject path under overload); `run-keepalive.sh` isolates
the upstream connection-keepalive payoff. On loopback the Redis-backed overhead
is small (within run-to-run noise of the no-limit ceiling) and `limit_req` sits
on the baseline — which is why single-node deployments should use `limit_req`;
this module is for the distributed case. See
[`benchmark/README.md`](benchmark/README.md) for methodology, how to run, and
captured results.

## Documentation

- [docs/INSTALL.md](docs/INSTALL.md) — requirements and build steps.
- [docs/DIRECTIVES.md](docs/DIRECTIVES.md) — full directive reference, algorithm
  semantics, and response headers.
- [docs/CUSTOM_SCRIPT.md](docs/CUSTOM_SCRIPT.md) — bring-your-own Lua script
  (`algo=custom`): the contract, an example, and constraints.
- [docs/EXAMPLES.md](docs/EXAMPLES.md) — configuration recipes.
- [benchmark/README.md](benchmark/README.md) — benchmarks and results.

## License

BSD 3-Clause. This is a derivative work of `weserv/rate-limit-nginx-module`;
the upstream copyright is retained in [`LICENSE`](LICENSE) and the provenance and
changes are recorded in [`NOTICE`](NOTICE).
