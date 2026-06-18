#!/usr/bin/env bash
#
# Single-node throughput comparison of three rate-limiting approaches, so the
# cost of *distribution* is quantified honestly against the in-process baseline:
#
#   baseline    - location returning 200, no limiting at all (the ceiling)
#   limit_req   - stock NGINX, shared-memory counter, no network hop
#   weserv      - weserv/rate-limit-nginx-module + Redis RATER.LIMIT (GCRA in C)
#   ratelimit   - this module + Redis EVALSHA
#                 (fixed window / token bucket / GCRA / sliding window)
#
# Two scenarios are measured (select with SCENARIO=allow|reject|both, default
# both):
#
#   allow  - every approach is effectively unlimited, so every request passes
#            the limit-decision path and returns 200. Measures the per-request
#            overhead of the decision itself, not 429 generation. Tabulated as
#            Requests/sec against the baseline.
#   reject - every approach gets a low cap; the warmup uses up the small initial
#            allowance, so the measured run is essentially 100% rejected.
#            Measures the reject path under sustained overload (how fast each
#            limiter says no). Tabulated as reject-path Requests/sec + reject %.
#
# ApacheBench drives each location.
#
# On loopback the Redis hop costs only a sub-millisecond local connect, so the
# Redis-backed numbers here are a *floor* on the real overhead: a networked
# managed Redis (ElastiCache / Memorystore / Upstash) adds a round-trip plus TLS
# and per-connection AUTH/SELECT that this test cannot reproduce. limit_req has
# no network at all and will dominate -- the point is to price distribution, not
# to crown limit_req (which this module deliberately defers to for single-node;
# it exists for the shared-across-instances case).
#
# weserv and this module both define `load_module`, and the two cannot coexist
# in one nginx; each approach therefore runs in its own nginx instance (separate
# conf, pid, and port), started and torn down in sequence. One valkey serves the
# Redis-backed approaches; RATER.LIMIT is loaded into it only when weserv is
# included (the module just adds a command and does not affect EVALSHA).
#
# weserv is optional: without NGINX_MOD_WESERV the weserv approach is skipped and
# the RATER.LIMIT redis module (REDIS_MODULE) is not required, so the benchmark
# runs with just nginx + this module + valkey.
#
# Self-contained: starts valkey and each nginx, then tears them down. Run in a
# single shell invocation (shared network namespace):
#
#   bash benchmark/run-comparison.sh                 # defaults: 20000 req, c=50
#   REQUESTS=50000 CONCURRENCY=100 bash benchmark/run-comparison.sh
#   SCENARIO=reject bash benchmark/run-comparison.sh # only the reject scenario
#
# Requires ApacheBench (ab), valkey-server, valkey-cli, nginx + this module.
# weserv (NGINX_MOD_WESERV) and the RATER.LIMIT module (REDIS_MODULE) are only
# needed to include the weserv approach.

set -u

NGINX_BIN="${NGINX_BIN:-}"
NGINX_MODULE="${NGINX_MODULE:-}"
NGINX_MOD_WESERV="${NGINX_MOD_WESERV:-}"
REDIS_MODULE="${REDIS_MODULE:-}"

VALKEY="${VALKEY:-valkey-server}"
VALKEY_CLI="${VALKEY_CLI:-valkey-cli}"
AB="${AB:-ab}"

REDIS_PORT="${REDIS_PORT:-6394}"
HTTP_BASE="${HTTP_BASE:-18090}"
REQUESTS="${REQUESTS:-20000}"
CONCURRENCY="${CONCURRENCY:-50}"
WORK="$(mktemp -d /tmp/ratelimit-cmp.XXXXXX)"
VALKEY_PID="$WORK/valkey.pid"

cleanup() {
    [ -f "$WORK/nginx.pid" ] && \
        "$NGINX_BIN" -p "$WORK" -c "$WORK/nginx.conf" -e logs/error.log -s stop 2>/dev/null
    [ -f "$VALKEY_PID" ] && kill "$(cat "$VALKEY_PID")" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# Tolerate an ASAN-instrumented build of either shared object.
export ASAN_OPTIONS="detect_odr_violation=0:abort_on_error=0:${ASAN_OPTIONS:-}"

# weserv is optional: without NGINX_MOD_WESERV the weserv approach is skipped,
# and the RATER.LIMIT redis module it needs is then not required either (so the
# benchmark runs with just nginx + this module + valkey). A *specified* weserv
# module that is missing is still an error -- the user asked for it.
WITH_WESERV=0
if [ -n "$NGINX_MOD_WESERV" ]; then
    WITH_WESERV=1
fi

command -v "$AB" >/dev/null 2>&1 || { echo "ApacheBench (ab) not found"; exit 2; }
[ -x "$NGINX_BIN" ]    || { echo "nginx binary not found: NGINX_BIN=$NGINX_BIN"; exit 2; }
[ -f "$NGINX_MODULE" ] || { echo "module not found: NGINX_MODULE=$NGINX_MODULE"; exit 2; }
if [ "$WITH_WESERV" = 1 ]; then
    [ -f "$NGINX_MOD_WESERV" ] || { echo "module not found: NGINX_MOD_WESERV=$NGINX_MOD_WESERV"; exit 2; }
    [ -f "$REDIS_MODULE" ]     || { echo "redis module not found (required for weserv): REDIS_MODULE=$REDIS_MODULE"; exit 2; }
else
    echo "note: NGINX_MOD_WESERV unset -- skipping the weserv approach" >&2
fi

mkdir -p "$WORK/logs" "$WORK/html"
# A real static file is the content handler for every approach, so the rate
# limiter (PREACCESS phase) actually runs before content is served and the
# served path is identical across approaches. Using `return 200` instead would
# finalize the request in the REWRITE phase, *before* PREACCESS, bypassing the
# limiter entirely; the `error_page 404` trick would run it but logs a spurious
# open() failure per request.
printf 'ok' > "$WORK/html/t"

# --- start one valkey; load RATER.LIMIT only when weserv is in play ---------
# This module uses EVALSHA and needs no server-side module; the redis module is
# loaded purely so weserv's RATER.LIMIT command exists.
ws_loadmodule=""
[ "$WITH_WESERV" = 1 ] && ws_loadmodule="--loadmodule $REDIS_MODULE"
# shellcheck disable=SC2086
"$VALKEY" --port "$REDIS_PORT" $ws_loadmodule --daemonize yes \
    --pidfile "$VALKEY_PID" --save '' --dir "$WORK" >/dev/null 2>&1
for _ in $(seq 1 50); do
    "$VALKEY_CLI" -p "$REDIS_PORT" ping >/dev/null 2>&1 && break
    sleep 0.1
done
"$VALKEY_CLI" -p "$REDIS_PORT" ping >/dev/null 2>&1 \
    || { echo "valkey failed to start"; exit 2; }

# --- write one nginx.conf for the named approach and scenario --------------
# $1 = approach key, $2 = conf path, $3 = http port, $4 = scenario
#
# Scenarios set the limit parameters; everything else is identical:
#   allow  - effectively unlimited, every request passes (measures the overhead
#            of the limit *decision* on the allow path)
#   reject - a low cap, so after a tiny initial allowance (used up by the warmup)
#            every measured request is rejected (measures the reject path under
#            sustained overload: how fast the limiter says no)
write_conf() {
    approach="$1"; conf="$2"; port="$3"; scenario="$4"
    case "$approach" in
        baseline)   load=""; ;;
        limit_req)  load=""; ;;
        weserv)     load="load_module $NGINX_MOD_WESERV;"; ;;
        *)          load="load_module $NGINX_MODULE;"; ;;   # ratelimit-*
    esac

    if [ "$scenario" = allow ]; then
        lr_rate="10000000r/s"; lr_body_extra="burst=1000000 nodelay"
        rl_req=1000000000;     rl_period=1h; ws_burst="burst=1000000000"
    else
        # A low cap over a window long enough not to roll during the run, so the
        # measured run is essentially 100% rejected. limit_req gets a low rate
        # with its default burst=0 (immediate 503 on excess). weserv keeps its
        # default burst=0 here -- the very behaviour that throttles under
        # concurrency is exactly what we want in the reject scenario.
        lr_rate="1r/s";        lr_body_extra=""
        rl_req=100;            rl_period=1h; ws_burst=""
    fi

    # Per-approach http-context lines and the body of `location /t`. The server
    # block, listen, and the static `location /t { ... }` wrapper are shared so
    # every approach serves the same file through the same content handler and
    # differs only in the limiter directive.
    http_extra=""
    loc_body=""
    case "$approach" in
        baseline)
            ;;
        limit_req)
            http_extra="limit_req_zone \$remote_addr zone=lr:64m rate=$lr_rate;"
            loc_body="limit_req zone=lr $lr_body_extra;"
            ;;
        weserv)
            http_extra="upstream redis { server 127.0.0.1:$REDIS_PORT; keepalive 32; }"
            loc_body="rate_limit \$remote_addr requests=$rl_req period=$rl_period $ws_burst; rate_limit_prefix ws; rate_limit_pass redis;"
            ;;
        ratelimit-fixed|ratelimit-token|ratelimit-gcra|ratelimit-sliding)
            case "$approach" in
                ratelimit-fixed)   algo="";                    prefix=rlf;;
                ratelimit-token)   algo="algo=token_bucket";   prefix=rlt;;
                ratelimit-gcra)    algo="algo=gcra";           prefix=rlg;;
                ratelimit-sliding) algo="algo=sliding_window"; prefix=rls;;
            esac
            http_extra="upstream redis { server 127.0.0.1:$REDIS_PORT; keepalive 32; }
    ratelimit_zone z key=\$remote_addr requests=$rl_req period=$rl_period $algo;"
            loc_body="ratelimit zone=z; ratelimit_prefix $prefix; ratelimit_pass redis;"
            ;;
    esac

    {
        echo "$load"
        cat <<EOF
worker_processes 1;
pid $WORK/nginx.pid;
error_log $WORK/logs/error.log error;
events { worker_connections 1024; }
http {
    access_log off;
    root $WORK/html;
    $http_extra
    server {
        listen 127.0.0.1:$port;
        location /t { $loc_body }
    }
}
EOF
    } > "$conf"
}

# --- measure one approach in one scenario: start nginx, warm up, ab, stop --
# Echoes "rps mean_ms p99_ms reject_pct"; raw ab dump in $WORK/<approach>.ab
measure() {
    approach="$1"; port="$2"; scenario="$3"
    conf="$WORK/$approach.conf"
    write_conf "$approach" "$conf" "$port" "$scenario"

    rm -f "$WORK/nginx.pid"
    : > "$WORK/logs/error.log"
    "$NGINX_BIN" -p "$WORK" -c "$conf" -e logs/error.log 2>>"$WORK/logs/start.log" \
        || { echo "ERR ERR ERR ERR"; return; }
    for _ in $(seq 1 50); do
        curl -s -o /dev/null "http://127.0.0.1:$port/t" && break
        sleep 0.1
    done

    "$VALKEY_CLI" -p "$REDIS_PORT" flushall >/dev/null 2>&1
    "$AB" -n "$REQUESTS" -c "$CONCURRENCY" "http://127.0.0.1:$port/t" \
        >/dev/null 2>&1                                  # warmup (uses the cap)
    "$AB" -n "$REQUESTS" -c "$CONCURRENCY" "http://127.0.0.1:$port/t" \
        >"$WORK/$approach.ab" 2>/dev/null

    "$NGINX_BIN" -p "$WORK" -c "$conf" -e logs/error.log -s stop 2>/dev/null
    for _ in $(seq 1 50); do
        [ -f "$WORK/nginx.pid" ] || break
        sleep 0.1
    done

    # ab omits the "Non-2xx responses" line when there are none, so default to 0.
    complete=$(sed -n 's/^Complete requests: *\([0-9]*\).*/\1/p' "$WORK/$approach.ab")
    nx=$(sed -n 's/^Non-2xx responses: *\([0-9]*\).*/\1/p' "$WORK/$approach.ab")
    of=$(grep -c 'open()' "$WORK/logs/error.log" 2>/dev/null || echo 0)
    nx="${nx:-0}"; complete="${complete:-0}"

    # Guard against a silent no-op: an open() failure means the served path is
    # wrong. The non-2xx expectation flips with the scenario -- allow must serve
    # all 2xx; reject must actually reject (baseline has no limiter, so skip it).
    [ "${of:-0}" -gt 0 ] 2>/dev/null && \
        echo "  WARN $approach: $of open() failures in error log" >&2
    if [ "$scenario" = allow ]; then
        [ "$nx" -gt 0 ] 2>/dev/null && \
            echo "  WARN $approach: $nx non-2xx in allow scenario (expected 0)" >&2
    else
        [ "$approach" != baseline ] && [ "$nx" -eq 0 ] 2>/dev/null && \
            echo "  WARN $approach: nothing rejected; limiter did not engage" >&2
    fi

    rps=$(sed -n 's/^Requests per second: *\([0-9.]*\).*/\1/p' "$WORK/$approach.ab")
    mean=$(sed -n 's/^Time per request: *\([0-9.]*\).*(mean)$/\1/p' "$WORK/$approach.ab")
    p99=$(awk '/^ *99%/{print $2}' "$WORK/$approach.ab")
    rej=$(awk -v n="$nx" -v c="$complete" 'BEGIN{ if (c>0) printf "%.1f", n/c*100; else print "0.0" }')
    echo "${rps:-ERR} ${mean:-ERR} ${p99:-ERR} ${rej:-ERR}"
}

ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b+0>0 && a!="ERR") printf "%.2fx", a/b; else print "n/a" }'; }

# --- run every approach for one scenario and print its table ---------------
run_scenario() {
    scenario="$1"
    local -A RPS MEAN P99 REJ
    local i=0 a port r m p rj

    if [ "$scenario" = allow ]; then
        echo "== allow scenario: effectively unlimited, every request passes =="
    else
        echo "== reject scenario: low cap, every measured request is rejected =="
    fi

    for a in $APPROACHES; do
        port=$((HTTP_BASE + i)); i=$((i + 1))
        read -r r m p rj <<<"$(measure "$a" "$port" "$scenario")"
        RPS[$a]="$r"; MEAN[$a]="$m"; P99[$a]="$p"; REJ[$a]="$rj"
        printf '  measured %-16s rps=%-10s mean=%-7s p99=%-4s reject=%s%%\n' \
            "$a" "$r" "$m" "$p" "$rj"
    done

    if [ "$scenario" = allow ]; then
        base="${RPS[baseline]}"
        printf '\n%-18s %12s %10s %9s %12s\n' \
            "approach" "rps" "mean(ms)" "p99(ms)" "vs baseline"
        printf '%-18s %12s %10s %9s %12s\n' \
            "------------------" "------------" "----------" "---------" "------------"
        for a in $APPROACHES; do
            if [ "$a" = baseline ]; then rel="1.00x (ref)"; else
                rel="$(ratio "$base" "${RPS[$a]}")"; fi
            printf '%-18s %12s %10s %9s %12s\n' \
                "$a" "${RPS[$a]}" "${MEAN[$a]}" "${P99[$a]}" "$rel"
        done
        echo
        echo "vs baseline = baseline_rps / approach_rps (how many times the no-limit"
        echo "ceiling exceeds this approach's throughput). Higher means more overhead."
    else
        printf '\n%-18s %12s %10s %9s %10s\n' \
            "approach" "rps" "mean(ms)" "p99(ms)" "rejected"
        printf '%-18s %12s %10s %9s %10s\n' \
            "------------------" "------------" "----------" "---------" "----------"
        for a in $APPROACHES; do
            printf '%-18s %12s %10s %9s %9s%%\n' \
                "$a" "${RPS[$a]}" "${MEAN[$a]}" "${P99[$a]}" "${REJ[$a]}"
        done
        echo
        echo "rps here is reject-path throughput (how fast each limiter rejects under"
        echo "overload). baseline has no limiter (rejected 0%) and is the serve-everything"
        echo "reference. A Redis-backed reject still costs the full round-trip, so it does"
        echo "not get cheaper than its allow path; limit_req rejects from shared memory."
    fi
}

# --- drive the requested scenarios -----------------------------------------
if [ "$WITH_WESERV" = 1 ]; then
    APPROACHES="baseline limit_req weserv ratelimit-fixed ratelimit-token ratelimit-gcra ratelimit-sliding"
else
    APPROACHES="baseline limit_req ratelimit-fixed ratelimit-token ratelimit-gcra ratelimit-sliding"
fi
SCENARIO="${SCENARIO:-both}"

echo "ApacheBench: -n $REQUESTS -c $CONCURRENCY"
echo "nginx: $("$NGINX_BIN" -v 2>&1)"
echo "valkey: $("$VALKEY_CLI" -p "$REDIS_PORT" info server | sed -n 's/^valkey_version:\(.*\)\r*/\1/p' | tr -d '\r')"
echo "scenario: $SCENARIO"
echo

case "$SCENARIO" in
    allow)  run_scenario allow ;;
    reject) run_scenario reject ;;
    both)   run_scenario allow; echo; run_scenario reject ;;
    *)      echo "unknown SCENARIO: $SCENARIO (use allow|reject|both)"; exit 2 ;;
esac
