#!/usr/bin/env perl

# An internal redirect (index / try_files / error_page / X-Accel-Redirect)
# re-runs the PREACCESS phase. Core wipes r->ctx across the redirect, so the
# limiter must not issue a second EVALSHA and double-count the request. The
# limit must be reached on the Nth request, not the (N/2)th. This mirrors
# limit_req's one-shot-per-main-request behaviour.

use Test::Nginx::Socket 'no_plan';

repeat_each(1);

$ENV{TEST_NGINX_REDIS_PORT} ||= 6379;

our $HttpConfig = qq{
    upstream redis {
       server 127.0.0.1:$ENV{TEST_NGINX_REDIS_PORT};
       keepalive 1024;
    }

    ratelimit_zone rz key=\$arg_k requests=4 period=1m;

    error_log logs/error.log debug;
};

no_long_string();

run_tests();

__DATA__

=== TEST 1: "index" redirect within a limited location counts once per request
--- http_config eval: $::HttpConfig
--- config
    # "/app/" is rate limited and serves a static index. The request "/app/"
    # triggers an internal redirect to "/app/index.html", which matches
    # "location /app/" again and re-runs the PREACCESS phase. Without the
    # one-shot guard each request would consume two counter slots and the
    # limit (4) would be hit on the 2nd request instead of the 5th.
    location /app/ {
        ratelimit zone=rz;
        ratelimit_prefix ir;
        ratelimit_pass redis;

        index index.html;
    }
--- user_files
>>> app/index.html
ok
--- request eval
['GET /app/?k=red', 'GET /app/?k=red', 'GET /app/?k=red', 'GET /app/?k=red', 'GET /app/?k=red']
--- error_code eval
[200, 200, 200, 200, 429]
