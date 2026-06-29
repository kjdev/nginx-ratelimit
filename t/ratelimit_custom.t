#!/usr/bin/env perl

# Custom user-supplied Lua script (ratelimit_zone ... algo=custom script=FILE).
# The script receives the same ARGV contract as token_bucket/gcra
# (ARGV[1..4] = requests, period, burst, quantity) and must return the fixed
# 5-integer reply {status, limit, remaining, retry_after, reset}. A script that
# violates that contract is rejected fail-closed. Config-time errors (missing
# script=, script= without algo=custom, unreadable/empty/oversized file) abort
# startup. Each block uses a distinct prefix to avoid key collisions.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

no_long_string();

run_tests();

__DATA__

=== TEST 1: a custom script allows up to the limit, then denies
--- http_config
    upstream redis {
       server 127.0.0.1:$TEST_NGINX_REDIS_PORT;
       keepalive 1024;
    }

    ratelimit_zone cu1 key=$remote_addr requests=2 period=1m algo=custom
        script=html/limit.lua;

    error_log logs/error.log debug;
--- config
    location /hit {
        ratelimit zone=cu1;
        ratelimit_prefix cu1z;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- user_files
>>> limit.lua
local limit = tonumber(ARGV[1]) + tonumber(ARGV[3])
local period = tonumber(ARGV[2])
local quantity = tonumber(ARGV[4])
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if quantity == 0 then
  local remaining = limit - current
  if remaining < 0 then remaining = 0 end
  local status = 0
  if current >= limit then status = 1 end
  return {status, limit, remaining, -1, 0}
end
if current + quantity > limit then
  return {1, limit, 0, period, 0}
end
local newval = redis.call('INCRBY', KEYS[1], quantity)
redis.call('EXPIRE', KEYS[1], period)
local remaining = limit - newval
if remaining < 0 then remaining = 0 end
return {0, limit, remaining, -1, period}
--- pipelined_requests eval
["GET /hit", "GET /hit", "GET /hit"]
--- error_code eval
[200, 200, 429]

=== TEST 2: peek (quantity=0) reports state without consuming
--- http_config
    upstream redis {
       server 127.0.0.1:$TEST_NGINX_REDIS_PORT;
       keepalive 1024;
    }

    ratelimit_zone cu2 key=$remote_addr requests=5 period=1m algo=custom
        script=html/limit.lua;

    error_log logs/error.log debug;
--- config
    location /peek {
        ratelimit zone=cu2 quantity=0;
        ratelimit_prefix cu2z;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- user_files
>>> limit.lua
local limit = tonumber(ARGV[1]) + tonumber(ARGV[3])
local period = tonumber(ARGV[2])
local quantity = tonumber(ARGV[4])
local current = tonumber(redis.call('GET', KEYS[1]) or '0')
if quantity == 0 then
  local remaining = limit - current
  if remaining < 0 then remaining = 0 end
  local status = 0
  if current >= limit then status = 1 end
  return {status, limit, remaining, -1, 0}
end
if current + quantity > limit then
  return {1, limit, 0, period, 0}
end
local newval = redis.call('INCRBY', KEYS[1], quantity)
redis.call('EXPIRE', KEYS[1], period)
local remaining = limit - newval
if remaining < 0 then remaining = 0 end
return {0, limit, remaining, -1, period}
--- request
    GET /peek
--- response_headers
X-RateLimit-Limit: 5
X-RateLimit-Remaining: 5
!Retry-After
--- error_code: 200

=== TEST 3: a script that breaks the 5-integer contract fails closed
--- http_config
    upstream redis {
       server 127.0.0.1:$TEST_NGINX_REDIS_PORT;
       keepalive 1024;
    }

    ratelimit_zone cu3 key=$remote_addr requests=2 period=1m algo=custom
        script=html/bad.lua;

    error_log logs/error.log debug;
--- config
    location /bad {
        ratelimit zone=cu3;
        ratelimit_prefix cu3z;
        ratelimit_pass redis;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- user_files
>>> bad.lua
return {1, 2, 3}
--- request
    GET /bad
--- error_code: 500
--- response_body_unlike: 200 OK

=== TEST 4: algo=custom without script= is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m algo=custom;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- must_die
--- error_log
"algo=custom" requires "script="

=== TEST 5: script= without algo=custom is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m
        script=html/limit.lua;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- user_files
>>> limit.lua
return {0, 1, 1, -1, 0}
--- must_die
--- error_log
"script=" requires "algo=custom"

=== TEST 6: a missing script file is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m algo=custom
        script=html/does-not-exist.lua;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- user_files
>>> other.lua
return {0, 1, 1, -1, 0}
--- must_die
--- error_log
ratelimit: cannot open script

=== TEST 7: an empty script file is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m algo=custom
        script=html/empty.lua;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- user_files
>>> empty.lua
--- must_die
--- error_log
is empty

=== TEST 8: an oversized script file is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m algo=custom
        script=html/big.lua;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- user_files eval
">>> big.lua\n" . ("-" x 70000) . "\n"
--- must_die
--- error_log
more than the

=== TEST 9: a non-regular script path (a directory) is rejected at config load
--- http_config
    ratelimit_zone bad key=$remote_addr requests=2 period=1m algo=custom
        script=html;
--- config
    location /t {
        ratelimit zone=bad;
        ratelimit_pass redis;
    }
--- must_die
--- error_log
is not a regular file
