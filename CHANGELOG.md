# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This module is a fork of
[weserv/rate-limit-nginx-module](https://github.com/weserv/rate-limit-nginx-module)
(BSD-3-Clause); see [`NOTICE`](NOTICE) for provenance and the full list of
changes from upstream.

## [0.1.0] - 2026-06-24

The entries below describe the work done since the fork.

### Added

- Redis-backed distributed rate limiting enforced in the `PREACCESS` phase,
  with the counter living in Redis so the limit is shared across every NGINX
  instance pointed at the same server.
- `limit_req`-style named zones: `ratelimit_zone` defines a rate
  (`key=`, `rate=`/`requests=`+`period=`, `burst=`, `algo=`) and `ratelimit`
  applies it to a context. No shared memory is allocated.
- Four algorithms selectable via `algo=`: fixed window, token bucket, GCRA, and
  sliding window. All share a fixed 5-integer Lua return contract
  `{status, limit, remaining, retry_after, reset}`.
- A `quantity=0` peek mode (`ratelimit_quantity`) that reports current state
  without consuming.
- Atomic counters via Lua `EVALSHA`, with transparent `-NOSCRIPT` fallback that
  resends once as `EVAL` on the same connection and keeps using `EVALSHA`
  afterwards. Works against stock/managed Redis with no server-side module.
- Native `ngx_http_upstream` Redis transport (fully asynchronous, connection
  keepalive), with RESP encoded and decoded in-module — no `hiredis`
  dependency.
- Managed-Redis credentials: `ratelimit_password` (`AUTH`) and
  `ratelimit_database` (`SELECT`).
- Directives `ratelimit_pass`, `ratelimit_prefix`, `ratelimit_headers`
  (`X-RateLimit-*` on allowed responses), `ratelimit_status`,
  `ratelimit_log_level`, `ratelimit_buffer_size`, and
  `ratelimit_connect_timeout` / `_send_timeout` / `_read_timeout`.
- Documentation: `README.md`, `docs/INSTALL.md`, `docs/DIRECTIVES.md`,
  `docs/EXAMPLES.md`, and a `Dockerfile` for building against stock NGINX.
- Benchmark harness (`benchmark/`): a comparison against stock `limit_req` and
  the weserv fork, plus an upstream keepalive isolation test.
- Integration and unit tests covering window rollover, concurrency atomicity,
  the `NOSCRIPT` → `EVAL` fallback, AUTH/SELECT, per-algorithm peek/admit/limit
  behaviour, burst, empty-key decline, and the Redis-down path.

### Changed

- **Breaking:** redesigned the directives from the upstream `rate_limit*` shape
  into `limit_req`-style named zones (`ratelimit_zone` + `ratelimit`).
- Replaced the upstream `RATER.LIMIT` Redis-module command with a Lua `EVALSHA`
  command layer, removing the dependency on a loadable Redis module.
- Renamed the module identity and all internal symbols to a single-layer
  `ngx_http_ratelimit_*` prefix; directives use the `ratelimit_*` prefix.
- `AUTH` / `SELECT` are sent only on a freshly opened connection; a reused
  keepalive connection goes straight to `EVALSHA`.

### Fixed

- Fail closed (`503`) when Redis is unreachable or returns an error, so an
  outage cannot silently disable limiting.
- Limit once per main request across internal redirects.
- Guard the RESP reply parser against integer-accumulation overflow and against
  a partial reply left over from the header read.
- Wipe the `AUTH` password via pool cleanup on early-error paths.
- Reject, at config load, an active zone with no `ratelimit_pass` target and a
  duplicate `ratelimit_pass` with a variable target.
- Align each algorithm's peek status with its consume threshold; report GCRA and
  sliding-window peek from current state without advancing it.
- Emit the hard-error response once, and log the zone name rather than the raw
  rate key on a rate-limit-exceeded event.
