# Benchmarks

Two self-contained load tests for this module. Each starts its own valkey and
nginx, drives them with ApacheBench, and tears everything down. Run each in a
single shell invocation so the network namespace is shared.

| script               | question it answers                                            |
|----------------------|----------------------------------------------------------------|
| `run-comparison.sh`  | What does distribution cost vs `limit_req` and vs weserv?      |
| `run-keepalive.sh`   | Where does upstream Redis connection keepalive actually pay off? |

## Requirements

- ApacheBench (`ab`), `valkey-server`, `valkey-cli`.
- Built nginx and modules (paths via env var):
  - `NGINX_BIN` — `path/to/nginx` (nginx binary)
  - `NGINX_MODULE` — `path/to/ngx_http_ratelimit_module.so` (this module)
- Optional, only to include the **weserv** approach:
  - `NGINX_MOD_WESERV` — `path/to/ngx_http_rate_limit_module.so` (weserv fork)
  - `REDIS_MODULE` — `path/to/ratelimit.so` (the `RATER.LIMIT` Redis module)

  Without `NGINX_MOD_WESERV` the weserv approach is skipped, `REDIS_MODULE` is
  not needed, and valkey starts without `RATER.LIMIT` — so the benchmark runs
  with just nginx + this module + valkey.

## `run-comparison.sh` — single-node, three approaches

Compares the per-request **decision overhead** of three ways to rate-limit one
node, so the price of distribution is visible against the in-process baseline:

- **baseline** — a location returning 200, no limiting (the throughput ceiling).
- **limit_req** — stock NGINX, shared-memory counter, no network hop.
- **weserv** — `weserv/rate-limit-nginx-module` + Redis `RATER.LIMIT` (GCRA in C).
- **ratelimit** — this module + Redis `EVALSHA`, run for each algorithm
  (`fixed window`, `token_bucket`, `gcra`, `sliding_window`).

It runs two scenarios (`SCENARIO=allow|reject|both`, default `both`):

- **allow** — every approach is effectively unlimited, so every request passes
  and returns 200. Measures the overhead of the limit *decision*, not the cost
  of generating 429s. Tabulated as Requests/sec against the baseline.
- **reject** — every approach gets a low cap; the warmup uses up the small
  initial allowance, so the measured run is ~100% rejected. Measures the reject
  path under sustained overload — how fast each limiter says no.

Each approach serves the same static file through the same content handler and
differs only in the limiter directive, so the measured gap is pure limiter
overhead. (A location whose only content directive is `return 200` would
finalize in the REWRITE phase *before* the PREACCESS limiter runs and silently
measure nothing — the harness asserts the expected non-2xx outcome per scenario
and a clean error log to catch that.)

```sh
bash benchmark/run-comparison.sh                  # both scenarios, -n 20000 -c 50
REQUESTS=50000 CONCURRENCY=100 bash benchmark/run-comparison.sh
SCENARIO=reject bash benchmark/run-comparison.sh  # only the reject scenario
```

weserv and this module both define `load_module` and cannot coexist in one
nginx, so each approach runs in its own nginx instance (separate conf, pid, and
port), started and stopped in sequence. A single valkey with `RATER.LIMIT`
loaded serves both Redis-backed approaches — the extra command does not affect
this module's `EVALSHA` path.

### How to read it, and what loopback hides

`limit_req` keeps its counter in shared memory with no network, so it sits on
the baseline; this is exactly why the module **defers to `limit_req` for
single-node** and exists only for the shared-across-instances case. The
Redis-backed approaches each add one round-trip per request.

On loopback that round-trip is a sub-millisecond local connect, so the measured
overhead is a **floor**: a networked managed Redis (ElastiCache / Memorystore /
Upstash) adds a WAN round-trip plus TLS and a per-connection AUTH/SELECT
handshake that this test cannot reproduce. The point is to *price distribution*,
not to crown `limit_req`. The connection-reuse mechanism that absorbs part of
that real cost is what `run-keepalive.sh` isolates.

### Results

See [`results/`](results/) for captured runs with full environment headers.
A representative loopback run (i7-9700K, valkey 9, `-n 50000 -c 100`):

| approach          | vs baseline (rps ratio) |
|-------------------|------------------------:|
| limit_req         |                   0.95x |
| ratelimit-gcra    |                   1.00x |
| ratelimit-token   |                   0.99x |
| ratelimit-sliding |                   0.99x |
| ratelimit-fixed   |                   0.99x |
| weserv            |                   0.99x |

`limit_req` sits near the baseline (shared memory, no network) — in this run it
was slightly below it (0.95x), which is just loopback noise. The
Redis-backed approaches all cluster at 0.99–1.00x; the spread between weserv,
`EVALSHA`, and this module's four algorithms is within the run-to-run noise of a
loopback setup, so do not read an algorithm ranking into it. In the **reject**
scenario every limiter rejects at the serve-everything ceiling — a Redis-backed
reject still pays the round-trip, but the empty-body 429 is cheaper to send than
the file, so rejecting is no slower than allowing. Full tables and discussion:
[`results/i7-9700k-loopback.md`](results/i7-9700k-loopback.md).

## `run-keepalive.sh` — where keepalive pays off

Drives one effectively-unlimited zone through two upstreams — one with
`keepalive`, one without — against two Redis setups (`plain`, and
`requirepass` + `SELECT`, the managed-Redis case). On loopback the per-request
connect is nearly free so keepalive shows little gain; the saved connection
setup (WAN round-trip + TLS handshake) is what makes it matter against networked
managed Redis. The AUTH/SELECT prelude is pipelined onto every request, so
keepalive's payoff is the saved connection establishment, not the prelude.

```sh
bash benchmark/run-keepalive.sh                   # defaults: -n 20000 -c 50
REQUESTS=50000 CONCURRENCY=100 bash benchmark/run-keepalive.sh
```
