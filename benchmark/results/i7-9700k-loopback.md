# Comparison results — i7-9700K, loopback Redis

Captured with `benchmark/run-comparison.sh`. Numbers are loopback-only and carry
run-to-run noise larger than the gaps between approaches; treat differences
below ~5–10% as parity (see the allow and reject tables below — the Redis-backed
ordering, and even where `limit_req` lands relative to the baseline, reshuffles
between them).

## Environment

- CPU: Intel Core i7-9700K @ 3.60GHz (8 cores)
- nginx: 1.30.2 (`worker_processes 1`)
- valkey: 9.0.4, loopback, `RATER.LIMIT` module loaded
- Driver: ApacheBench, `-n 50000 -c 100`
- Each approach serves the same static file through the same content handler and
  differs only in the limiter directive, so the gap is pure limiter overhead.

## Allow scenario (effectively unlimited, every request passes)

Measures the overhead of the limit *decision* on the allow path.

| approach          |      rps | mean (ms) | p99 (ms) | vs baseline |
|-------------------|---------:|----------:|---------:|------------:|
| baseline          | 29281.87 |     3.415 |        5 | 1.00x (ref) |
| limit_req         | 30951.84 |     3.231 |        4 |       0.95x |
| weserv            | 29551.74 |     3.384 |        5 |       0.99x |
| ratelimit-fixed   | 29451.35 |     3.395 |        5 |       0.99x |
| ratelimit-token   | 29645.99 |     3.373 |        5 |       0.99x |
| ratelimit-gcra    | 29348.70 |     3.407 |        5 |       1.00x |
| ratelimit-sliding | 29691.35 |     3.368 |        5 |       0.99x |

`vs baseline` = baseline_rps / approach_rps (how many times the no-limit ceiling
exceeds this approach's throughput; higher = more overhead).

- **Every approach lands within loopback noise of the baseline.** In this run
  `limit_req` even measured slightly *faster* than the no-limit baseline (0.95x)
  and the four Redis-backed algorithms plus weserv all sit at 0.99–1.00x — the
  per-request differences are smaller than the run-to-run variance, so no
  approach can be said to beat another here.
- **`limit_req` costs essentially nothing.** Its counter is in shared memory with
  no network hop. This is why the module defers to it for single-node
  deployments.
- **The Redis-backed approaches each add one local round-trip per request.**
  weserv (`RATER.LIMIT`, GCRA in C) and this module's four algorithms (fixed
  window, token bucket, GCRA, sliding window) all land in the same band; the
  within-band ordering is inside the run-to-run noise of a loopback setup (it
  reshuffles between repeats), so do not read an algorithm ranking into it.
- **This is a floor, not the deployment number.** Against a networked managed
  Redis every request additionally pays a WAN round-trip plus TLS and a
  per-connection AUTH/SELECT handshake — costs loopback cannot reproduce, and
  the reason the keepalive payoff (see `run-keepalive.sh`) only shows there.

## Reject scenario (low cap, every measured request is rejected)

Measures the reject path under sustained overload — how fast each limiter says
no. The warmup uses up the small allowance, so the measured run is 100%
rejected (`baseline` has no limiter and is the serve-everything reference).

| approach          |      rps | mean (ms) | p99 (ms) | rejected |
|-------------------|---------:|----------:|---------:|---------:|
| baseline          | 30770.88 |     3.250 |        4 |     0.0% |
| limit_req         | 30987.42 |     3.227 |        4 |   100.0% |
| weserv            | 30485.63 |     3.280 |        4 |   100.0% |
| ratelimit-fixed   | 30539.46 |     3.274 |        4 |   100.0% |
| ratelimit-token   | 30869.51 |     3.239 |        4 |   100.0% |
| ratelimit-gcra    | 30561.45 |     3.272 |        4 |   100.0% |
| ratelimit-sliding | 30375.05 |     3.292 |        4 |   100.0% |

- **Rejecting is not slower than allowing.** A Redis-backed reject still pays the
  full round-trip (the Lua script / `RATER.LIMIT` runs and returns "deny"), but
  the 429 it then sends carries no body, which is cheaper than serving the
  static file — the two effects cancel, so reject-path throughput lands at the
  serve-everything ceiling. limit_req rejects straight from shared memory.
- The practical takeaway: an overload of rejected traffic does not cost more per
  request than normal traffic; the limiter is not a bottleneck that gets worse
  under attack (on loopback — a networked Redis still adds its round-trip).

## Note on configuration

Both correctness traps this benchmark must avoid:

- A location whose only content directive is `return 200` finalizes in the
  REWRITE phase, *before* the PREACCESS limiter runs — it would silently measure
  no limiting. Every approach therefore serves a real static file (content
  phase, after PREACCESS). The harness asserts zero non-2xx and a clean error
  log per run to catch any regression of this.
- weserv ties the GCRA burst tolerance to `burst=` (default 0), so even at a
  huge rate two requests within one emission interval 429 under concurrency; it
  is given a large `burst=` to stay unthrottled. This module's algorithms derive
  burst capacity from the request count, so they need no equivalent.
