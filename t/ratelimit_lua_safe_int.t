#!/usr/bin/env perl

# Redis's embedded Lua (5.1) represents every number as an IEEE-754 double,
# so integers above 2^53-1 lose precision once tonumber() touches them.
# requests+burst is returned verbatim as the "limit"/"capacity" reply field
# by every built-in algorithm, so the config loader rejects a zone (or a
# per-location burst override) whose requests+burst would exceed 2^53-1.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: requests+burst exactly at the safe integer limit loads and
round-trips intact
--- http_config eval
"$::HttpConfig
    ratelimit_zone safeint key=\$remote_addr requests=9007199254740991 period=1m;
"
--- config
    location /hit {
        ratelimit zone=safeint;
        ratelimit_prefix si;
        ratelimit_pass redis;
        ratelimit_headers on;

        error_page 404 =200 @ok;
    }

    location @ok {
        default_type text/plain;
        return 200 "200 OK\n";
    }
--- request
    GET /hit
--- response_headers
X-RateLimit-Limit: 9007199254740991
--- error_code: 200

=== TEST 2: requests+burst one past the safe integer limit is rejected at
config load
--- http_config eval
"$::HttpConfig
    ratelimit_zone toobig key=\$remote_addr requests=9007199254740991 period=1m burst=1;
"
--- config
    location /t {
        ratelimit zone=toobig;
        ratelimit_pass redis;
    }
--- must_die
--- error_log
"requests" + "burst" must not exceed 9007199254740991

=== TEST 3: a per-location burst override that pushes requests+burst past
the limit is rejected at config load
--- http_config eval
"$::HttpConfig
    ratelimit_zone small key=\$remote_addr requests=100 period=1m;
"
--- config
    location /t {
        ratelimit zone=small burst=9007199254740991;
        ratelimit_pass redis;
    }
--- must_die
--- error_log
"requests" + "burst" must not exceed 9007199254740991
