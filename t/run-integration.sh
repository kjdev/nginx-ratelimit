#!/usr/bin/env bash
#
# Integration checks that Test::Nginx cannot express cleanly: window rollover
# (needs wall-clock time), concurrency atomicity (needs parallel clients), and
# the NOSCRIPT -> EVAL fallback (needs a server-side SCRIPT FLUSH mid-run).
#
# Self-contained: starts its own valkey and nginx, runs the checks against a
# stock server (no RATER.LIMIT module), and tears everything down. Run it in a
# single shell invocation so the network namespace is shared:
#
#   bash t/run-integration.sh
#
# Exits 0 only if every check passes.

set -u

NGINX="${TEST_NGINX_BINARY:-}"
MODULE="${TEST_NGINX_LOAD_MODULES:-}"

VALKEY="${VALKEY:-valkey-server}"
VALKEY_CLI="${VALKEY_CLI:-valkey-cli}"

REDIS_PORT="${REDIS_PORT:-6391}"
HTTP_PORT="${HTTP_PORT:-18080}"
WORK="$(mktemp -d /tmp/ratelimit-it.XXXXXX)"
chmod 0755 "$WORK"
VALKEY_PID="$WORK/valkey.pid"

fail=0
note() { printf '  %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
bad()  { printf '[FAIL] %s\n' "$*"; fail=1; }

cleanup() {
    [ -f "$WORK/nginx.pid" ] && \
        "$NGINX" -p "$WORK" -c "$WORK/nginx.conf" -e logs/error.log -s stop 2>/dev/null
    [ -f "$VALKEY_PID" ] && kill "$(cat "$VALKEY_PID")" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

# Tolerate an ASAN-instrumented build (the shared object and nginx register the
# same globals); harmless here and ignored so a debug build can run unchanged.
export ASAN_OPTIONS="detect_odr_violation=0:abort_on_error=0:${ASAN_OPTIONS:-}"

[ -x "$NGINX" ] || { echo "nginx binary not found: TEST_NGINX_BINARY=$NGINX"; exit 2; }
[ -f "$MODULE" ] || { echo "module not found: TEST_NGINX_LOAD_MODULES=$MODULE"; exit 2; }

# --- start a fresh stock valkey -------------------------------------------
"$VALKEY" --port "$REDIS_PORT" --daemonize yes --pidfile "$VALKEY_PID" \
    --save '' --dir "$WORK" >/dev/null 2>&1
for _ in $(seq 1 50); do
    "$VALKEY_CLI" -p "$REDIS_PORT" ping >/dev/null 2>&1 && break
    sleep 0.1
done

rcli() { "$VALKEY_CLI" -p "$REDIS_PORT" "$@"; }

# --- write nginx config ----------------------------------------------------
mkdir -p "$WORK/logs"
cat > "$WORK/nginx.conf" <<EOF
load_module $MODULE;
worker_processes 1;
pid $WORK/nginx.pid;
error_log $WORK/logs/error.log info;
events { worker_connections 256; }
http {
    access_log off;
    upstream redis { server 127.0.0.1:$REDIS_PORT; keepalive 16; }

    ratelimit_zone wb key=\$remote_addr requests=4  period=2s;
    ratelimit_zone cc key=\$remote_addr requests=20 period=1m;
    ratelimit_zone ns key=\$remote_addr requests=100 period=1m;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /wb { ratelimit zone=wb; ratelimit_prefix wb; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location /cc { ratelimit zone=cc; ratelimit_prefix cc; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location /ns { ratelimit zone=ns; ratelimit_prefix ns; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location = /ok { return 200 "ok"; }
    }
}
EOF

"$NGINX" -p "$WORK" -c "$WORK/nginx.conf" -e logs/error.log \
    || { echo "nginx failed to start"; exit 2; }
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/ns" && break
    sleep 0.1
done

code() { curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT$1"; }

# --- check 1: window boundary ---------------------------------------------
echo "== window boundary (requests=4 period=2s) =="
rcli flushall >/dev/null
ok=1
for i in 1 2 3 4; do
    c=$(code /wb); [ "$c" = 200 ] || { ok=0; note "request $i got $c, want 200"; }
done
c=$(code /wb); [ "$c" = 429 ] || { ok=0; note "5th got $c, want 429"; }
note "waiting for the window to roll over"
sleep 3
c=$(code /wb); [ "$c" = 200 ] || { ok=0; note "post-window got $c, want 200"; }
[ "$ok" = 1 ] && pass "counter resets after the window expires" \
              || bad  "window boundary"

# --- check 2: concurrency atomicity ---------------------------------------
echo "== concurrency atomicity (requests=20, 100 parallel) =="
rcli flushall >/dev/null
allowed=$(seq 1 100 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' \
            "http://127.0.0.1:$HTTP_PORT/cc" | grep -c '^200$')
if [ "$allowed" = 20 ]; then
    pass "exactly 20 of 100 concurrent requests allowed (allowed=$allowed)"
else
    bad "concurrency: allowed=$allowed, want 20"
fi

# --- check 3: NOSCRIPT -> EVAL fallback -----------------------------------
echo "== NOSCRIPT fallback (SCRIPT FLUSH then one request) =="
rcli flushall >/dev/null
rcli script flush >/dev/null
rcli config resetstat >/dev/null
c=$(code /ns)
evalsha_failed=$(rcli info commandstats | tr -d '\r' \
    | sed -n 's/.*cmdstat_evalsha:.*failed_calls=\([0-9]*\).*/\1/p')
eval_calls=$(rcli info commandstats | tr -d '\r' \
    | sed -n 's/.*cmdstat_eval:calls=\([0-9]*\).*/\1/p')
evalsha_failed="${evalsha_failed:-0}"
eval_calls="${eval_calls:-0}"
note "response=$c evalsha.failed_calls=$evalsha_failed eval.calls=$eval_calls"
if [ "$c" = 200 ] && [ "$evalsha_failed" -ge 1 ] && [ "$eval_calls" -ge 1 ]; then
    pass "EVALSHA missed (NOSCRIPT) and EVAL fallback served the request"
else
    bad "NOSCRIPT fallback"
fi

echo
[ "$fail" = 0 ] && echo "ALL INTEGRATION CHECKS PASSED" || echo "INTEGRATION CHECKS FAILED"
exit "$fail"
