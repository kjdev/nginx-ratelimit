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
rclin() { "$VALKEY_CLI" -p "$REDIS_PORT" -n "$1" "${@:2}"; }

# --- write the custom (algo=custom) Lua script -----------------------------
# A self-contained fixed-window limiter using the custom ARGV contract
# (ARGV[1..4] = requests, period, burst, quantity), returning the 5-integer
# reply. Exercises the body and SHA the module loads from "script=".
mkdir -p "$WORK/logs"
cat > "$WORK/custom.lua" <<'LUA'
local limit = tonumber(ARGV[1]) + tonumber(ARGV[3])
local period = tonumber(ARGV[2])
local quantity = tonumber(ARGV[4])
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if current + quantity > limit then
  return {1, limit, 0, period, 0}
end
local newval = redis.call('INCRBY', KEYS[1], quantity)
redis.call('EXPIRE', KEYS[1], period)
local remaining = limit - newval
if remaining < 0 then remaining = 0 end
return {0, limit, remaining, -1, period}
LUA

# --- write nginx config ----------------------------------------------------
cat > "$WORK/nginx.conf" <<EOF
load_module $MODULE;
worker_processes 1;
pid $WORK/nginx.pid;
error_log $WORK/logs/error.log info;
events { worker_connections 256; }
http {
    access_log off;

    # Keep all runtime temp paths under the test work dir so the run does not
    # depend on the build-time prefix (e.g. /var/lib/nginx) being writable.
    client_body_temp_path $WORK/client_body;
    proxy_temp_path $WORK/proxy;
    fastcgi_temp_path $WORK/fastcgi;
    uwsgi_temp_path $WORK/uwsgi;
    scgi_temp_path $WORK/scgi;

    upstream redis { server 127.0.0.1:$REDIS_PORT; keepalive 16; }

    ratelimit_zone wb key=\$remote_addr requests=4  period=2s;
    ratelimit_zone cc key=\$remote_addr requests=20 period=1m;
    ratelimit_zone ns key=\$remote_addr requests=100 period=1m;
    ratelimit_zone db key=\$remote_addr requests=100 period=1m;
    ratelimit_zone cu key=\$remote_addr requests=100 period=1m algo=custom script=$WORK/custom.lua;

    server {
        listen 127.0.0.1:$HTTP_PORT;

        location /wb { ratelimit zone=wb; ratelimit_prefix wb; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location /cc { ratelimit zone=cc; ratelimit_prefix cc; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location /ns { ratelimit zone=ns; ratelimit_prefix ns; ratelimit_pass redis;
                       error_page 404 =200 /ok; }
        location /cu { ratelimit zone=cu; ratelimit_prefix cu; ratelimit_pass redis;
                       error_page 404 =200 /ok; }

        # Two locations sharing one upstream (keepalive) but selecting different
        # Redis DBs under an identical key name. Isolation must hold across
        # connection reuse: the prelude SELECT runs on every request.
        location /db1 { ratelimit zone=db; ratelimit_prefix di; ratelimit_database 1;
                        ratelimit_pass redis; error_page 404 =200 /ok; }
        location /db2 { ratelimit zone=db; ratelimit_prefix di; ratelimit_database 2;
                        ratelimit_pass redis; error_page 404 =200 /ok; }

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

# --- check 4: SELECT db isolation across keepalive reuse ------------------
echo "== db isolation (shared upstream, db 1 vs db 2, reused connections) =="
rcli flushall >/dev/null
# Interleave so a connection that last ran SELECT 1 is reused for a db-2
# request and vice versa; the per-request SELECT must still route each count
# to its own DB under the identical key name "di_127.0.0.1".
for path in /db1 /db2 /db1 /db2 /db1 /db2 /db2 /db2; do
    code "$path" >/dev/null
done
d1=$(rclin 1 get di_127.0.0.1); d1="${d1:-0}"
d2=$(rclin 2 get di_127.0.0.1); d2="${d2:-0}"
note "db1=$d1 (want 3) db2=$d2 (want 5)"
if [ "$d1" = 3 ] && [ "$d2" = 5 ]; then
    pass "counts stay isolated per SELECT db across connection reuse"
else
    bad "db isolation: db1=$d1 want 3, db2=$d2 want 5"
fi

# --- check 5: custom script NOSCRIPT fallback + EVALSHA cache --------------
# A custom (algo=custom) script takes the same EVALSHA -> NOSCRIPT -> EVAL path
# as the built-ins, keyed by the SHA computed from its loaded body. The first
# request after a SCRIPT FLUSH misses and falls back to EVAL (which loads the
# script); the second request must then hit EVALSHA against the cached SHA.
echo "== custom script (algo=custom): NOSCRIPT fallback then EVALSHA cache =="
rcli flushall >/dev/null
rcli script flush >/dev/null
rcli config resetstat >/dev/null
c1=$(code /cu)
c2=$(code /cu)
evalsha_calls=$(rcli info commandstats | tr -d '\r' \
    | sed -n 's/.*cmdstat_evalsha:calls=\([0-9]*\).*/\1/p')
evalsha_failed=$(rcli info commandstats | tr -d '\r' \
    | sed -n 's/.*cmdstat_evalsha:.*failed_calls=\([0-9]*\).*/\1/p')
eval_calls=$(rcli info commandstats | tr -d '\r' \
    | sed -n 's/.*cmdstat_eval:calls=\([0-9]*\).*/\1/p')
evalsha_calls="${evalsha_calls:-0}"
evalsha_failed="${evalsha_failed:-0}"
eval_calls="${eval_calls:-0}"
note "responses=$c1,$c2 evalsha.calls=$evalsha_calls" \
     "evalsha.failed_calls=$evalsha_failed eval.calls=$eval_calls"
if [ "$c1" = 200 ] && [ "$c2" = 200 ] \
   && [ "$evalsha_failed" -eq 1 ] && [ "$eval_calls" -ge 1 ] \
   && [ "$evalsha_calls" -ge 2 ]; then
    pass "custom script fell back to EVAL once, then served from the cached SHA"
else
    bad "custom script NOSCRIPT fallback / SHA cache"
fi

echo
[ "$fail" = 0 ] && echo "ALL INTEGRATION CHECKS PASSED" || echo "INTEGRATION CHECKS FAILED"
exit "$fail"
